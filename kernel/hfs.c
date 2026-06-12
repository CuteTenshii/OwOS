/* hfs.c — read-only HFS+ driver (see hfs.h).
 *
 * Everything is big-endian. The Volume Header (byte 1024) points at the Catalog
 * B-tree via the catalog file's fork extents. The catalog's leaf nodes hold the
 * folder tree as records keyed by (parentCNID, name); we walk the leaf chain
 * linearly (firstLeafNode -> fLink -> ...), which finds every record regardless
 * of tree depth and sidesteps HFS+'s gnarly Unicode key-ordering -- we only ever
 * compare keys for equality, case-insensitively over ASCII. File data is read
 * from the data fork's inline extents (allocation blocks -> device sectors).
 */

#include <stddef.h>

#include "hfs.h"
#include "vga.h"
#include "kstring.h"

/* ------------------------------------------------------------- BE readers */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void put_u32(uint32_t v)
{
	char b[11]; int i = 10; b[i--] = '\0';
	if (!v) b[i--] = '0';
	while (v) { b[i--] = (char)('0' + v % 10); v /= 10; }
	vga_write(&b[i + 1]);
}

/* --------------------------------------------------------------- state */
#define HFS_SIG   0x482Bu     /* 'H+' */
#define HFSX_SIG  0x4858u     /* 'HX' */
#define ROOT_CNID 2u
#define MAX_NODE  8192        /* catalog node-size cap we support */

struct extent { uint32_t start, count; };   /* in allocation blocks */

static struct {
	hfs_read_fn read;
	uint32_t base;            /* absolute LBA of byte 0 of the volume */
	int      mounted;
	uint32_t block_size;      /* allocation block size (bytes) */
	uint32_t total_blocks;
	struct extent cat_ext[8]; /* catalog file fork extents */
	uint32_t node_size;       /* catalog B-tree node size */
	uint32_t first_leaf;      /* first leaf node index */
	char     label[64];
} vol;

static uint8_t nodebuf[MAX_NODE];     /* one catalog node, reused (serialized) */

/* A resolved catalog object. */
struct obj {
	int           is_dir;
	uint32_t      cnid;       /* folder id (dirs) */
	uint64_t      size;       /* data fork logical size (files) */
	struct extent ext[8];     /* data fork inline extents (files) */
};

/* ------------------------------------------------------------- device I/O */

/* Read `len` bytes from absolute device byte offset into buf (sector-by-sector). */
static int dev_read(uint64_t byteoff, uint8_t *buf, uint32_t len)
{
	uint8_t s[512];
	while (len) {
		uint32_t lba  = vol.base + (uint32_t)(byteoff / 512);
		uint32_t soff = (uint32_t)(byteoff % 512);
		if (vol.read(lba, s) != 0)
			return -1;
		uint32_t n = 512 - soff;
		if (n > len) n = len;
		kmemcpy(buf, s + soff, n);
		buf += n; byteoff += n; len -= n;
	}
	return 0;
}

/* Map a logical offset within the catalog file to an absolute device byte
 * offset, walking the catalog fork extents. Returns 0 on success. */
static int cat_dev_off(uint64_t logoff, uint64_t *devoff)
{
	uint64_t pos = 0;
	for (int i = 0; i < 8 && vol.cat_ext[i].count; i++) {
		uint64_t span = (uint64_t)vol.cat_ext[i].count * vol.block_size;
		if (logoff < pos + span) {
			*devoff = (uint64_t)vol.cat_ext[i].start * vol.block_size +
			          (logoff - pos);
			return 0;
		}
		pos += span;
	}
	return -1;
}

/* Read catalog node `idx` into nodebuf. */
static int read_node(uint32_t idx)
{
	uint64_t devoff;
	if (cat_dev_off((uint64_t)idx * vol.node_size, &devoff) != 0)
		return -1;
	return dev_read(devoff, nodebuf, vol.node_size);
}

/* ------------------------------------------------------- node/record access */

/* Byte offset of record `i` within the current node (from the trailing offset
 * array: the last 2 bytes hold record 0's offset, etc.). */
static uint32_t rec_off(int i)
{
	return be16(nodebuf + vol.node_size - 2 * (i + 1));
}

/* Convert an HFSUniStr255 name (UTF-16BE) to ASCII into out[cap]. */
static void uni_ascii(const uint8_t *p, int n, char *out, int cap)
{
	int o = 0;
	for (int i = 0; i < n && o < cap - 1; i++) {
		uint16_t u = be16(p + i * 2);
		out[o++] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
	}
	out[o] = '\0';
}

/* A catalog record, located within the current node. */
struct catrec {
	uint32_t parent;          /* key: parent CNID */
	const uint8_t *name;      /* key: UTF-16BE name units */
	int       namelen;        /* in UTF-16 units */
	uint16_t  type;           /* 1 folder, 2 file, 3/4 thread */
	const uint8_t *data;      /* record body (after the key) */
};

static void parse_rec(int i, struct catrec *c)
{
	uint32_t ro = rec_off(i);
	uint16_t keyLen = be16(nodebuf + ro);
	c->parent  = be32(nodebuf + ro + 2);
	c->namelen = be16(nodebuf + ro + 6);
	c->name    = nodebuf + ro + 8;
	c->data    = nodebuf + ro + 2 + keyLen;   /* key length excludes its field */
	c->type    = be16(c->data);
}

/* Hidden if the name is empty or begins with a control character -- this skips
 * the "HFS+ Private Data" / hardlink directories whose names start with NULs. */
static int rec_hidden(const struct catrec *c)
{
	if (c->namelen == 0)
		return 1;
	return be16(c->name) < 0x20;
}

/* Fill an obj from a folder/file record body. */
static void rec_to_obj(const struct catrec *c, struct obj *o)
{
	if (c->type == 1) {                       /* folder */
		o->is_dir = 1;
		o->cnid   = be32(c->data + 8);
		o->size   = 0;
	} else {                                  /* file */
		o->is_dir = 0;
		o->cnid   = be32(c->data + 8);
		const uint8_t *fork = c->data + 88;   /* HFSPlusForkData (data fork) */
		o->size = be64(fork);
		for (int i = 0; i < 8; i++) {
			o->ext[i].start = be32(fork + 16 + i * 8);
			o->ext[i].count = be32(fork + 16 + i * 8 + 4);
		}
	}
}

/* ------------------------------------------------------- catalog scanning */

/* Iterate every catalog leaf record, calling visit(); stop early if it returns
 * non-zero (that value is returned). 0 if the whole tree was scanned. Reuses
 * nodebuf, so visit() must consume what it needs before returning to continue. */
static int for_each_leaf(int (*visit)(const struct catrec *, void *), void *ctx)
{
	uint32_t node = vol.first_leaf;
	int guard = 0;
	while (node && guard++ < 1 << 20) {
		if (read_node(node) != 0)
			return 0;
		if (nodebuf[8] != 0xFF)               /* kind: leaf == 0xFF */
			return 0;
		int nrec = be16(nodebuf + 10);
		uint32_t fLink = be32(nodebuf + 0);
		for (int i = 0; i < nrec; i++) {
			struct catrec c;
			parse_rec(i, &c);
			int r = visit(&c, ctx);
			if (r)
				return r;
		}
		node = fLink;
	}
	return 0;
}

/* find a child by (parent, name) ------------------------------------------- */
struct find_ctx {
	uint32_t parent;
	const char *name;
	int found;
	struct obj out;
};

/* Full case-insensitive ASCII compare of two NUL-terminated names. */
static int names_match(const char *want, const char *have)
{
	int i = 0;
	for (; want[i] && have[i]; i++)
		if (lc(want[i]) != lc(have[i]))
			return 0;
	return want[i] == '\0' && have[i] == '\0';
}

static int visit_find2(const struct catrec *c, void *vp)
{
	struct find_ctx *f = vp;
	if (c->parent != f->parent || (c->type != 1 && c->type != 2))
		return 0;
	char nm[256];
	uni_ascii(c->name, c->namelen, nm, sizeof nm);
	if (names_match(f->name, nm)) {
		rec_to_obj(c, &f->out);
		f->found = 1;
		return 1;                             /* stop scanning */
	}
	return 0;
}

static int find_child(uint32_t parent, const char *name, struct obj *out)
{
	struct find_ctx f;
	f.parent = parent; f.name = name; f.found = 0;
	for_each_leaf(visit_find2, &f);
	if (!f.found)
		return -1;
	*out = f.out;
	return 0;
}

/* Resolve `path` to an object. The root ("" / "/") is the folder CNID 2. */
static int resolve(const char *path, struct obj *out)
{
	out->is_dir = 1;
	out->cnid   = ROOT_CNID;
	out->size   = 0;

	if (!path)
		return 0;
	while (*path == '/') path++;

	while (*path) {
		const char *comp = path;
		int len = 0;
		while (path[len] && path[len] != '/') len++;
		char name[256];
		int k = 0;
		for (; k < len && k < 255; k++) name[k] = comp[k];
		name[k] = '\0';
		path += len;
		while (*path == '/') path++;

		if (k == 0 || (k == 1 && name[0] == '.'))
			continue;
		if (!out->is_dir)
			return -1;
		struct obj child;
		if (find_child(out->cnid, name, &child) != 0)
			return -1;
		*out = child;
	}
	return 0;
}

/* ------------------------------------------------------------- mounting */

static int parse_volume_header(const uint8_t *vh)
{
	uint16_t sig = be16(vh);
	if (sig != HFS_SIG && sig != HFSX_SIG)
		return -1;
	vol.block_size   = be32(vh + 40);
	vol.total_blocks = be32(vh + 44);
	if (vol.block_size < 512 || (vol.block_size % 512) != 0)
		return -1;

	const uint8_t *cf = vh + 272;             /* catalogFile fork */
	for (int i = 0; i < 8; i++) {
		vol.cat_ext[i].start = be32(cf + 16 + i * 8);
		vol.cat_ext[i].count = be32(cf + 16 + i * 8 + 4);
	}
	if (vol.cat_ext[0].count == 0)
		return -1;
	return 0;
}

/* The volume label is the name of the root folder's own catalog record, which
 * is the one folder record whose parent CNID is 1. */
static int visit_label(const struct catrec *c, void *vp)
{
	(void)vp;
	if (c->type == 1 && c->parent == 1) {
		uni_ascii(c->name, c->namelen, vol.label, sizeof vol.label);
		return 1;
	}
	return 0;
}

int hfs_mount(hfs_read_fn read, uint32_t base_lba)
{
	uint8_t vh[512];
	vol.mounted = 0;
	vol.read = read;
	vol.base = base_lba;

	if (!read || read(base_lba + 2, vh) != 0)  /* volume header @ byte 1024 */
		return -1;
	if (parse_volume_header(vh) != 0)
		return -1;

	/* Catalog B-tree header is node 0: read its first sector for the header
	 * record (treeDepth/rootNode/firstLeafNode/nodeSize at node+14). */
	uint64_t devoff;
	if (cat_dev_off(0, &devoff) != 0)
		return -1;
	uint8_t hdr[512];
	if (dev_read(devoff, hdr, 512) != 0)
		return -1;
	const uint8_t *hr = hdr + 14;             /* BTHeaderRec */
	vol.first_leaf = be32(hr + 10);
	vol.node_size  = be16(hr + 18);
	if (vol.node_size < 512 || vol.node_size > MAX_NODE ||
	    (vol.node_size % 512) != 0)
		return -1;

	vol.label[0] = '\0';
	vol.mounted = 1;
	for_each_leaf(visit_label, NULL);
	if (!vol.label[0]) {
		const char *d = "HFS+ Volume";
		int i = 0; for (; d[i]; i++) vol.label[i] = d[i]; vol.label[i] = '\0';
	}
	return 0;
}

static uint32_t le32(const uint8_t *p)        /* GPT fields are little-endian */
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read `len` bytes from absolute device byte offset using a raw read callback
 * (used during probing, before vol.read/base are set). */
static int raw_read(hfs_read_fn read, uint64_t byteoff, uint8_t *buf, uint32_t len)
{
	uint8_t s[512];
	while (len) {
		uint32_t lba  = (uint32_t)(byteoff / 512);
		uint32_t soff = (uint32_t)(byteoff % 512);
		if (read(lba, s) != 0)
			return -1;
		uint32_t n = 512 - soff;
		if (n > len) n = len;
		kmemcpy(buf, s + soff, n);
		buf += n; byteoff += n; len -= n;
	}
	return 0;
}

/* Try a whole-disk mount, then GPT partitions, then an Apple Partition Map. */
static int probe(hfs_read_fn read)
{
	if (hfs_mount(read, 0) == 0)
		return 0;

	uint8_t sec[512];

	/* GPT: header at LBA 1 ("EFI PART"), entry array described in LE fields. */
	if (read(1, sec) == 0 && sec[0] == 'E' && sec[1] == 'F' &&
	    sec[2] == 'I' && sec[3] == ' ' && sec[4] == 'P') {
		uint64_t ent_lba = (uint64_t)le32(sec + 72) | ((uint64_t)le32(sec + 76) << 32);
		uint32_t ent_cnt = le32(sec + 80);
		uint32_t ent_sz  = le32(sec + 84);
		if (ent_sz < 128) ent_sz = 128;
		if (ent_cnt > 128) ent_cnt = 128;
		uint8_t ent[128];
		for (uint32_t i = 0; i < ent_cnt; i++) {
			if (raw_read(read, ent_lba * 512 + (uint64_t)i * ent_sz, ent, 128) != 0)
				break;
			int empty = 1;                    /* zero type GUID => unused slot */
			for (int b = 0; b < 16; b++) if (ent[b]) { empty = 0; break; }
			if (empty) continue;
			uint64_t first = (uint64_t)le32(ent + 32) | ((uint64_t)le32(ent + 36) << 32);
			if (first && hfs_mount(read, (uint32_t)first) == 0)
				return 0;
		}
	}

	/* Apple Partition Map: block 1 starts with 'PM' (0x504D), big-endian. */
	if (read(1, sec) == 0 && sec[0] == 0x50 && sec[1] == 0x4D) {
		uint32_t mapblks = be32(sec + 4);        /* pmMapBlkCnt */
		if (mapblks > 64) mapblks = 64;
		for (uint32_t i = 1; i <= mapblks; i++) {
			if (read(i, sec) != 0) break;
			if (sec[0] != 0x50 || sec[1] != 0x4D) continue;
			uint32_t pstart = be32(sec + 8);     /* pmPyPartStart */
			if (pstart && hfs_mount(read, pstart) == 0)
				return 0;
		}
	}
	return -1;
}

int hfs_mount_probe(hfs_read_fn read)
{
	if (!read)
		return -1;
	return probe(read);
}

void hfs_unmount(void)
{
	vol.mounted = 0;
	vol.label[0] = '\0';
}

int hfs_mounted(void) { return vol.mounted; }
const char *hfs_label(void) { return vol.mounted ? vol.label : ""; }
const char *hfs_type(void) { return vol.mounted ? "HFS+" : "none"; }

/* ------------------------------------------------------------- public reads */

int hfs_stat(const char *path, int *is_dir, uint32_t *size)
{
	if (!vol.mounted) return -2;
	struct obj o;
	if (resolve(path, &o) != 0) return -1;
	if (is_dir) *is_dir = o.is_dir;
	if (size)   *size = (uint32_t)o.size;
	return 0;
}

/* Directory listing / enumeration ------------------------------------------ */
struct list_ctx {
	uint32_t parent;
	int      want_idx;        /* -1 => print all; else fill *out for that index */
	int      seen;
	char    *name;
	int     *is_dir;
	uint32_t *size;
	int      hit;
};

static int visit_list(const struct catrec *c, void *vp)
{
	struct list_ctx *L = vp;
	if (c->parent != L->parent || (c->type != 1 && c->type != 2))
		return 0;
	if (rec_hidden(c))
		return 0;
	char nm[256];
	uni_ascii(c->name, c->namelen, nm, sizeof nm);

	if (L->want_idx < 0) {                    /* print mode */
		struct obj o; rec_to_obj(c, &o);
		vga_write(o.is_dir ? "  <DIR>  " : "         ");
		if (!o.is_dir) { put_u32((uint32_t)o.size); vga_write("  "); }
		else           { vga_write("  "); }
		vga_write(nm);
		vga_putc('\n');
		return 0;
	}
	/* index mode */
	if (L->seen == L->want_idx) {
		struct obj o; rec_to_obj(c, &o);
		int i = 0; for (; i < 255 && nm[i]; i++) L->name[i] = nm[i];
		L->name[i] = '\0';
		if (L->is_dir) *L->is_dir = o.is_dir;
		if (L->size)   *L->size = (uint32_t)o.size;
		L->hit = 1;
		return 1;
	}
	L->seen++;
	return 0;
}

int hfs_list(const char *path)
{
	if (!vol.mounted) return -2;
	struct obj d;
	if (resolve(path, &d) != 0) return -1;
	if (!d.is_dir) return -1;
	struct list_ctx L = { d.cnid, -1, 0, 0, 0, 0, 0 };
	for_each_leaf(visit_list, &L);
	return 0;
}

int hfs_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
	if (!vol.mounted || idx < 0) return 0;
	struct obj d;
	if (resolve(path, &d) != 0 || !d.is_dir) return 0;
	struct list_ctx L = { d.cnid, idx, 0, name, is_dir, size, 0 };
	for_each_leaf(visit_list, &L);
	return L.hit;
}

int hfs_read_at(const char *path, uint32_t offset, void *buf, uint32_t len)
{
	if (!vol.mounted) return -2;
	struct obj o;
	if (resolve(path, &o) != 0 || o.is_dir) return -1;
	if (offset >= o.size) return 0;
	if (offset + len > o.size) len = (uint32_t)(o.size - offset);

	uint8_t *dst = buf;
	uint32_t got = 0;
	while (got < len) {
		uint32_t cur = offset + got;
		uint64_t pos = 0; int fi = -1; uint64_t within = 0;
		for (int i = 0; i < 8 && o.ext[i].count; i++) {
			uint64_t span = (uint64_t)o.ext[i].count * vol.block_size;
			if (cur < pos + span) { fi = i; within = cur - pos; break; }
			pos += span;
		}
		if (fi < 0) break;                    /* beyond inline extents */
		uint64_t devoff = (uint64_t)o.ext[fi].start * vol.block_size + within;
		uint64_t remain = (uint64_t)o.ext[fi].count * vol.block_size - within;
		uint32_t chunk = len - got;
		if (chunk > remain) chunk = (uint32_t)remain;
		if (dev_read(devoff, dst + got, chunk) != 0)
			return got ? (int)got : -1;
		got += chunk;
	}
	return (int)got;
}

int hfs_read(const char *path, void *buf, uint32_t max)
{
	return hfs_read_at(path, 0, buf, max);
}
