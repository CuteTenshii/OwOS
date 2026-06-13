/* fat.c — read-only FAT12/16/32 driver (see fat.h).
 *
 * One volume is mounted at a time, described by `vol`. Everything is derived
 * from the BIOS Parameter Block in the boot sector: the reserved region, one or
 * more FATs, a root-directory region (FAT12/16) or root cluster (FAT32), and
 * the cluster-addressed data region. The FAT type is decided the canonical way
 * -- by the count of data clusters -- not by the (advisory) string in the BPB.
 *
 * Sector I/O goes through a one-sector LRU-of-1 cache. Walking a cluster chain
 * re-reads the same FAT sector for every link, and listing a directory re-reads
 * its sectors; caching the last sector turns those into hits and keeps the
 * polled AHCI/NVMe back ends from thrashing.
 */

#include <stddef.h>

#include "fat.h"
#include "endian.h"
#include "console.h"
#include "kstring.h"

/* ------------------------------------------------------------------ helpers */

static void fmemset(void *p, int c, uint32_t n)
{
	uint8_t *d = p;
	while (n--)
		*d++ = (uint8_t)c;
}

static char lc(char c)            /* ASCII lower-case */
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive compare of a NUL-terminated name against a length-bounded
 * one. Returns 1 if equal. */
static int name_eq(const char *a, const char *b, uint32_t blen)
{
	uint32_t i = 0;
	for (; i < blen; i++) {
		if (!a[i] || lc(a[i]) != lc(b[i]))
			return 0;
	}
	return a[i] == '\0';
}


/* ----------------------------------------------------------------- mounting */

enum fat_kind { FAT_NONE, FAT12, FAT16, FAT32 };

static struct {
	fat_read_fn  read;
	fat_write_fn write;     /* NULL => read-only mount                      */
	uint32_t base;          /* absolute LBA of the boot sector              */
	enum fat_kind kind;
	uint32_t bytes_per_sec;
	uint32_t sec_per_clus;
	uint32_t reserved;      /* reserved sectors before the first FAT        */
	uint32_t num_fats;
	uint32_t fat_sectors;   /* sectors per FAT                              */
	uint32_t root_entries;  /* FAT12/16 root-dir entry count (0 on FAT32)   */
	uint32_t root_sectors;  /* FAT12/16 root-dir region length in sectors   */
	uint32_t first_data;    /* volume-relative first data sector            */
	uint32_t root_cluster;  /* FAT32 root cluster (0 on FAT12/16)           */
	uint32_t total_clusters;
	char label[12];
} vol;

/* One-sector cache, indexed by absolute LBA. */
static uint8_t  cache[512];
static uint32_t cache_lba = 0xFFFFFFFFu;
static int      cache_ok;

static int read_abs(uint32_t lba, const uint8_t **out)
{
	if (!(cache_ok && cache_lba == lba)) {
		if (vol.read(lba, cache) != 0) {
			cache_ok = 0;
			return -1;
		}
		cache_lba = lba;
		cache_ok = 1;
	}
	*out = cache;
	return 0;
}

/* Read a volume-relative sector (adds the partition base). */
static int read_sec(uint32_t sec, const uint8_t **out)
{
	return read_abs(vol.base + sec, out);
}

/* Write-through one absolute sector and keep the read cache coherent. There is
 * deliberately no write-back buffering: every change reaches the medium before
 * the call returns, so a removal between operations can never lose data. */
static int write_abs(uint32_t lba, const void *buf)
{
	if (!vol.write)
		return -1;
	if (vol.write(lba, buf) != 0)
		return -1;
	if (cache_ok && cache_lba == lba)
		memcpy(cache, buf, 512);
	return 0;
}

static int write_sec(uint32_t sec, const void *buf)
{
	return write_abs(vol.base + sec, buf);
}

#define BAD_CLUSTER 0x0FFFFFFFu

/* The next cluster after `clus` in the chain, or >= an end marker at the end. */
static uint32_t fat_next(uint32_t clus)
{
	const uint8_t *s;
	uint32_t fat_base = vol.reserved;   /* sectors */

	if (vol.kind == FAT16) {
		uint32_t off = clus * 2;
		if (read_sec(fat_base + off / vol.bytes_per_sec, &s) != 0)
			return BAD_CLUSTER;
		return load_le16(s + off % vol.bytes_per_sec);
	}
	if (vol.kind == FAT32) {
		uint32_t off = clus * 4;
		if (read_sec(fat_base + off / vol.bytes_per_sec, &s) != 0)
			return BAD_CLUSTER;
		return load_le32(s + off % vol.bytes_per_sec) & 0x0FFFFFFFu;
	}
	/* FAT12: 1.5 bytes per entry, possibly straddling a sector boundary. */
	uint32_t off = clus + clus / 2;
	uint32_t sec = fat_base + off / vol.bytes_per_sec;
	uint32_t pos = off % vol.bytes_per_sec;
	uint8_t lo, hi;
	if (read_sec(sec, &s) != 0)
		return BAD_CLUSTER;
	lo = s[pos];
	if (pos + 1 < vol.bytes_per_sec) {
		hi = s[pos + 1];
	} else {
		if (read_sec(sec + 1, &s) != 0)   /* spills into the next sector */
			return BAD_CLUSTER;
		hi = s[0];
	}
	uint16_t v = (uint16_t)(lo | (hi << 8));
	return (clus & 1) ? (uint32_t)(v >> 4) : (uint32_t)(v & 0x0FFF);
}

/* 1 if `clus` is an end-of-chain marker for the mounted FAT type. */
static int is_eoc(uint32_t clus)
{
	switch (vol.kind) {
	case FAT12: return clus >= 0x0FF8u;
	case FAT16: return clus >= 0xFFF8u;
	case FAT32: return clus >= 0x0FFFFFF8u;
	default:    return 1;
	}
}

static uint32_t cluster_first_sector(uint32_t clus)
{
	return vol.first_data + (clus - 2) * vol.sec_per_clus;
}

/* Validate a candidate boot sector as a FAT BPB and, if good, populate `vol`.
 * Returns 0 on success. */
static int parse_bpb(const uint8_t *bs)
{
	if (load_le16(bs + 510) != 0xAA55)
		return -1;

	uint32_t bps  = load_le16(bs + 11);
	uint32_t spc  = bs[13];
	uint32_t rsvd = load_le16(bs + 14);
	uint32_t nfat = bs[16];
	uint32_t rootent = load_le16(bs + 17);
	uint32_t totsec16 = load_le16(bs + 19);
	uint32_t fatsz16  = load_le16(bs + 22);
	uint32_t totsec32 = load_le32(bs + 32);
	uint32_t fatsz32  = load_le32(bs + 36);

	/* Our block back ends speak 512-byte sectors, and the cache is one such
	 * sector, so only 512-byte-logical-sector volumes are supported (the
	 * overwhelming common case). This doubles as the FAT signature check for a
	 * whole-disk volume with no partition table. */
	if (bps != 512)
		return -1;
	if (spc == 0 || (spc & (spc - 1)) != 0)
		return -1;
	if (nfat == 0 || nfat > 2 || rsvd == 0)
		return -1;

	uint32_t fatsz  = fatsz16 ? fatsz16 : fatsz32;
	uint32_t totsec = totsec16 ? totsec16 : totsec32;
	if (fatsz == 0 || totsec == 0)
		return -1;

	uint32_t root_sectors = ((rootent * 32) + (bps - 1)) / bps;
	uint32_t data_start   = rsvd + nfat * fatsz + root_sectors;
	if (data_start >= totsec)
		return -1;
	uint32_t data_sectors = totsec - data_start;
	uint32_t clusters     = data_sectors / spc;

	vol.bytes_per_sec  = bps;
	vol.sec_per_clus   = spc;
	vol.reserved       = rsvd;
	vol.num_fats       = nfat;
	vol.fat_sectors    = fatsz;
	vol.root_entries   = rootent;
	vol.root_sectors   = root_sectors;
	vol.first_data     = data_start;
	vol.total_clusters = clusters;

	if (clusters < 4085)
		vol.kind = FAT12;
	else if (clusters < 65525)
		vol.kind = FAT16;
	else
		vol.kind = FAT32;

	/* Root location and volume label live in different places per type. */
	const uint8_t *lbl = NULL;
	if (vol.kind == FAT32) {
		vol.root_cluster = load_le32(bs + 44);
		if (vol.root_cluster < 2)
			return -1;
		if (bs[66] == 0x29)        /* extended boot signature */
			lbl = bs + 71;
	} else {
		vol.root_cluster = 0;
		if (bs[38] == 0x29)
			lbl = bs + 43;
	}

	vol.label[0] = '\0';
	if (lbl) {
		int n = 11;
		while (n > 0 && lbl[n - 1] == ' ')   /* trim trailing pad */
			n--;
		for (int i = 0; i < n; i++)
			vol.label[i] = (char)lbl[i];
		vol.label[n] = '\0';
	}
	return 0;
}

int fat_mount(fat_read_fn read, fat_write_fn write, uint32_t base_lba)
{
	uint8_t bs[512];
	vol.kind = FAT_NONE;
	cache_ok = 0;
	cache_lba = 0xFFFFFFFFu;

	if (!read || read(base_lba, bs) != 0)
		return -1;

	vol.read  = read;
	vol.write = write;
	vol.base  = base_lba;
	if (parse_bpb(bs) != 0) {
		vol.kind = FAT_NONE;
		return -1;
	}
	return 0;
}

void fat_unmount(void)
{
	vol.kind  = FAT_NONE;
	vol.write = NULL;
	cache_ok  = 0;
	cache_lba = 0xFFFFFFFFu;
}

int fat_writable(void) { return vol.kind != FAT_NONE && vol.write != NULL; }

/* --------------------------------------------------------- directory walking */

/* A resolved directory entry. The loc[] arrays record where the raw on-disk
 * records live -- the run of LFN slots followed by the 8.3 entry -- so a delete
 * can wipe every record that makes up the name, not just the 8.3 one. */
#define MAX_RECS 21           /* up to 20 LFN slots + 1 short entry */
struct dirent {
	char     name[256];   /* long name if present, else 8.3                  */
	uint32_t cluster;     /* first cluster (0 = empty file)                  */
	uint32_t size;        /* bytes (0 for directories)                       */
	int      is_dir;
	uint32_t loc_lba[MAX_RECS];
	uint32_t loc_off[MAX_RECS];
	int      loc_n;
};

/* Build the displayable 8.3 name from a raw directory record into out[13]. */
static void short_name(const uint8_t *e, char *out)
{
	int o = 0;
	for (int i = 0; i < 8; i++) {
		if (e[i] == ' ')
			break;
		out[o++] = (char)e[i];
	}
	if (e[8] != ' ') {
		out[o++] = '.';
		for (int i = 8; i < 11; i++) {
			if (e[i] == ' ')
				break;
			out[o++] = (char)e[i];
		}
	}
	out[o] = '\0';
	/* A leading 0x05 stands in for a real 0xE5 first byte (KANJI escape). */
	if ((uint8_t)out[0] == 0x05)
		out[0] = (char)0xE5;
}

/* Copy the 13 UCS-2 chars of one LFN slot (offsets 1,3,5,7,9,14..24,28,30) as
 * ASCII into dst[13], stopping at a NUL/0xFFFF terminator. Returns chars
 * written (<=13) and sets *term if the name ended in this slot. */
static int lfn_chars(const uint8_t *e, char *dst, int *term)
{
	static const int off[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
	int n = 0;
	*term = 0;
	for (int i = 0; i < 13; i++) {
		uint16_t ch = load_le16(e + off[i]);
		if (ch == 0x0000 || ch == 0xFFFF) {
			*term = 1;
			break;
		}
		dst[n++] = (ch < 0x80) ? (char)ch : '?';
	}
	return n;
}

/* Iterator over a directory's 32-byte records. FAT12/16 root is a fixed sector
 * region; everything else is a cluster chain. `start_cluster` is 0 for the
 * FAT12/16 fixed root. */
struct dir_iter {
	uint32_t cluster;       /* current cluster (0 => fixed root region)      */
	uint32_t sector_in;     /* sector index within the cluster/root region   */
	uint32_t ent_in;        /* entry index within the current sector         */
	uint32_t root_left;     /* remaining root sectors (fixed-root mode)      */
	const uint8_t *sec;     /* cached current sector                         */
	uint32_t sec_lba;       /* absolute LBA of `sec` (for in-place edits)    */
	uint32_t cur_lba;       /* LBA of the record just returned by diter_next */
	uint32_t cur_off;       /* byte offset of that record within its sector  */
	int done;
};

static void diter_init(struct dir_iter *it, uint32_t start_cluster)
{
	fmemset(it, 0, sizeof(*it));
	it->cluster = start_cluster;
	if (start_cluster == 0)
		it->root_left = vol.root_sectors;
}

/* Fetch the next raw 32-byte record, or NULL at end-of-directory. */
static const uint8_t *diter_next(struct dir_iter *it)
{
	uint32_t per_sec = vol.bytes_per_sec / 32;
	for (;;) {
		if (it->done)
			return NULL;
		if (it->ent_in == 0) {
			/* Need to (re)load the current sector. */
			uint32_t lba;
			if (it->cluster == 0) {            /* fixed FAT12/16 root */
				if (it->root_left == 0) { it->done = 1; return NULL; }
				lba = vol.reserved + vol.num_fats * vol.fat_sectors +
				      it->sector_in;
			} else {
				if (it->sector_in == vol.sec_per_clus) {
					uint32_t nxt = fat_next(it->cluster);
					if (nxt < 2 || is_eoc(nxt)) { it->done = 1; return NULL; }
					it->cluster = nxt;
					it->sector_in = 0;
				}
				lba = cluster_first_sector(it->cluster) + it->sector_in;
			}
			if (read_sec(lba, &it->sec) != 0) { it->done = 1; return NULL; }
			it->sec_lba = lba;
		}

		uint32_t rec_off = it->ent_in * 32;
		const uint8_t *e = it->sec + rec_off;
		it->cur_lba = it->sec_lba;       /* remember where this record lives */
		it->cur_off = rec_off;
		/* advance the cursor for next time */
		if (++it->ent_in == per_sec) {
			it->ent_in = 0;
			it->sector_in++;
			if (it->cluster == 0 && it->root_left)
				it->root_left--;
		}

		if (e[0] == 0x00) { it->done = 1; return NULL; }  /* end of dir */
		if (e[0] == 0xE5)                                  /* deleted */
			continue;
		return e;
	}
}

/* Read the next *logical* entry (folding any preceding LFN slots into the
 * following 8.3 record) into `out`. Returns 1 on a yielded entry, 0 at end. */
static int dir_read(struct dir_iter *it, struct dirent *out)
{
	char lfn[256];
	int  have_lfn = 0;
	uint32_t run_lba[MAX_RECS], run_off[MAX_RECS];
	int      run_n = 0;              /* raw records seen for this logical entry */

	for (;;) {
		const uint8_t *e = diter_next(it);
		if (!e)
			return 0;

		uint8_t attr = e[11];
		if (attr == 0x0F) {                  /* LFN slot */
			if (run_n < MAX_RECS) {
				run_lba[run_n] = it->cur_lba;
				run_off[run_n] = it->cur_off;
				run_n++;
			}
			int idx = (e[0] & 0x3F) - 1;     /* 1-based sequence */
			if (idx < 0 || idx >= 20)        /* 20*13 = 260 cap */
				{ have_lfn = 0; continue; }
			char part[13];
			int term;
			int n = lfn_chars(e, part, &term);
			int base = idx * 13;
			if (base + n < 256) {
				for (int i = 0; i < n; i++)
					lfn[base + i] = part[i];
				if (e[0] & 0x40)             /* last (highest) slot */
					lfn[base + n] = '\0';
				else if (n < 13)
					lfn[base + n] = '\0';
				have_lfn = 1;
			}
			continue;
		}
		if (attr & 0x08) {                   /* volume label / not a file */
			have_lfn = 0;
			run_n = 0;
			continue;
		}

		fmemset(out, 0, sizeof(*out));
		if (have_lfn && lfn[0]) {
			uint32_t k = 0;
			while (k < 255 && lfn[k]) { out->name[k] = lfn[k]; k++; }
			out->name[k] = '\0';
		} else {
			short_name(e, out->name);
			run_n = 0;               /* no LFN slots belong to this entry */
		}
		out->is_dir  = (attr & 0x10) ? 1 : 0;
		out->cluster = ((uint32_t)load_le16(e + 20) << 16) | load_le16(e + 26);
		out->size    = out->is_dir ? 0 : load_le32(e + 28);

		for (int i = 0; i < run_n; i++) {     /* LFN slots, then the 8.3 entry */
			out->loc_lba[i] = run_lba[i];
			out->loc_off[i] = run_off[i];
		}
		out->loc_lba[run_n] = it->cur_lba;
		out->loc_off[run_n] = it->cur_off;
		out->loc_n = run_n + 1;
		return 1;
	}
}

/* Find the child named [comp, comp+len) in the directory rooted at
 * `dir_cluster` (0 = FAT12/16 root). Returns 1 and fills *out on a hit. */
static int dir_find(uint32_t dir_cluster, const char *comp, uint32_t len,
                    struct dirent *out)
{
	struct dir_iter it;
	struct dirent   d;
	diter_init(&it, dir_cluster);
	while (dir_read(&it, &d)) {
		if (name_eq(d.name, comp, len)) {
			memcpy(out, &d, sizeof(*out));
			return 1;
		}
	}
	return 0;
}

/* Resolve `path` to a directory entry. On the root, returns is_dir with
 * cluster = root (0 for FAT12/16). Returns 0 on success, -1 if absent. */
static int resolve(const char *path, struct dirent *out)
{
	/* Start at the root. */
	fmemset(out, 0, sizeof(*out));
	out->is_dir  = 1;
	out->cluster = (vol.kind == FAT32) ? vol.root_cluster : 0;
	out->size    = 0;

	if (!path)
		return 0;
	while (*path == '/')
		path++;

	while (*path) {
		const char *comp = path;
		uint32_t len = 0;
		while (path[len] && path[len] != '/')
			len++;
		path += len;
		while (*path == '/')
			path++;

		if (len == 0)
			continue;
		if (len == 1 && comp[0] == '.')
			continue;

		if (!out->is_dir)            /* can't descend into a file */
			return -1;

		struct dirent child;
		if (!dir_find(out->cluster, comp, len, &child))
			return -1;
		memcpy(out, &child, sizeof(*out));
	}
	return 0;
}

/* ------------------------------------------------------------- public reads */

int fat_mounted(void) { return vol.kind != FAT_NONE; }

const char *fat_type(void)
{
	switch (vol.kind) {
	case FAT12: return "FAT12";
	case FAT16: return "FAT16";
	case FAT32: return "FAT32";
	default:    return "none";
	}
}

const char *fat_label(void) { return vol.label; }

int fat_stat(const char *path, int *is_dir, uint32_t *size)
{
	if (vol.kind == FAT_NONE)
		return -2;
	struct dirent d;
	if (resolve(path, &d) != 0)
		return -1;
	if (is_dir)
		*is_dir = d.is_dir;
	if (size)
		*size = d.size;
	return 0;
}

int fat_list(const char *path)
{
	if (vol.kind == FAT_NONE)
		return -2;
	struct dirent d;
	if (resolve(path, &d) != 0)
		return -1;
	if (!d.is_dir)
		return -1;

	struct dir_iter it;
	struct dirent   e;
	diter_init(&it, d.cluster);
	while (dir_read(&it, &e)) {
		if (e.name[0] == '.' &&
		    (e.name[1] == '\0' || (e.name[1] == '.' && e.name[2] == '\0')))
			continue;                 /* hide "." and ".." */
		con_write(e.is_dir ? "  <DIR>  " : "         ");
		if (!e.is_dir) {
			con_write_dec(e.size);
			con_write("  ");
		} else {
			con_write("  ");
		}
		con_write(e.name);
		con_putc('\n');
	}
	return 0;
}

int fat_read_at(const char *path, uint32_t offset, void *buf, uint32_t len)
{
	if (vol.kind == FAT_NONE)
		return -2;
	struct dirent d;
	if (resolve(path, &d) != 0 || d.is_dir)
		return -1;

	if (offset >= d.size)
		return 0;
	if (offset + len > d.size)
		len = d.size - offset;

	uint32_t cbytes = vol.sec_per_clus * vol.bytes_per_sec;
	uint32_t clus   = d.cluster;

	/* Walk the chain to the cluster holding `offset`. */
	uint32_t skip = offset / cbytes;
	while (skip--) {
		if (clus < 2 || is_eoc(clus))
			return 0;
		clus = fat_next(clus);
	}

	uint8_t *dst = buf;
	uint32_t got = 0;
	uint32_t in_clus = offset % cbytes;     /* byte offset into first cluster */

	while (got < len) {
		if (clus < 2 || is_eoc(clus))
			break;
		uint32_t first = cluster_first_sector(clus);
		uint32_t sec   = in_clus / vol.bytes_per_sec;
		uint32_t soff  = in_clus % vol.bytes_per_sec;

		for (; sec < vol.sec_per_clus && got < len; sec++, soff = 0) {
			const uint8_t *s;
			if (read_sec(first + sec, &s) != 0)
				return got ? (int)got : -1;
			uint32_t n = vol.bytes_per_sec - soff;
			if (n > len - got)
				n = len - got;
			memcpy(dst + got, s + soff, n);
			got += n;
		}
		in_clus = 0;
		clus = fat_next(clus);
	}
	return (int)got;
}

int fat_read(const char *path, void *buf, uint32_t max)
{
	return fat_read_at(path, 0, buf, max);
}

int fat_enum_dir(const char *path, int idx, char *name, int *is_dir,
                 uint32_t *size)
{
	if (vol.kind == FAT_NONE || idx < 0)
		return 0;
	struct dirent d;
	if (resolve(path, &d) != 0 || !d.is_dir)
		return 0;

	struct dir_iter it;
	struct dirent   e;
	int count = 0;
	diter_init(&it, d.cluster);
	while (dir_read(&it, &e)) {
		if (e.name[0] == '.' &&
		    (e.name[1] == '\0' || (e.name[1] == '.' && e.name[2] == '\0')))
			continue;
		if (count == idx) {
			uint32_t k = 0;
			while (k < 255 && e.name[k]) { name[k] = e.name[k]; k++; }
			name[k] = '\0';
			if (is_dir) *is_dir = e.is_dir;
			if (size)   *size = e.size;
			return 1;
		}
		count++;
	}
	return 0;
}

/* ----------------------------------------------------------------- writes
 *
 * Write-through: every helper below reaches the medium before returning, and
 * keeps the read cache coherent, so there is never buffered state to lose on an
 * eject. Names are written as 8.3 short names (LFN generation is not done yet),
 * so a created file may surface under a generated short alias. */

/* Read a (volume-relative) sector into a mutable buffer for a read-modify-write. */
static int read_secbuf(uint32_t sec, uint8_t *buf)
{
	const uint8_t *s;
	if (read_sec(sec, &s) != 0)
		return -1;
	memcpy(buf, s, 512);
	return 0;
}

static uint32_t eoc_value(void)
{
	return vol.kind == FAT12 ? 0x0FFFu
	     : vol.kind == FAT16 ? 0xFFFFu : 0x0FFFFFFFu;
}

/* Set cluster `clus`'s FAT entry to `val`, in every FAT copy. */
static int fat_set(uint32_t clus, uint32_t val)
{
	for (uint32_t f = 0; f < vol.num_fats; f++) {
		uint32_t fbase = vol.reserved + f * vol.fat_sectors;
		uint8_t b[512];

		if (vol.kind == FAT16) {
			uint32_t off = clus * 2;
			uint32_t sec = fbase + off / 512, pos = off % 512;
			if (read_secbuf(sec, b) != 0) return -1;
			b[pos] = val & 0xFF; b[pos + 1] = (val >> 8) & 0xFF;
			if (write_sec(sec, b) != 0) return -1;
		} else if (vol.kind == FAT32) {
			uint32_t off = clus * 4;
			uint32_t sec = fbase + off / 512, pos = off % 512;
			if (read_secbuf(sec, b) != 0) return -1;
			uint32_t nv = (load_le32(b + pos) & 0xF0000000u) | (val & 0x0FFFFFFFu);
			b[pos] = nv & 0xFF; b[pos + 1] = (nv >> 8) & 0xFF;
			b[pos + 2] = (nv >> 16) & 0xFF; b[pos + 3] = (nv >> 24) & 0xFF;
			if (write_sec(sec, b) != 0) return -1;
		} else {                       /* FAT12: 1.5 bytes, may straddle */
			uint32_t off = clus + clus / 2;
			uint32_t sec = fbase + off / 512, pos = off % 512;
			if (read_secbuf(sec, b) != 0) return -1;
			int straddle = (pos + 1 >= 512);
			uint8_t b1[512];
			uint8_t lo = b[pos], hi;
			if (straddle) {
				if (read_secbuf(sec + 1, b1) != 0) return -1;
				hi = b1[0];
			} else {
				hi = b[pos + 1];
			}
			uint16_t cur = (uint16_t)(lo | (hi << 8));
			uint16_t nv = (clus & 1) ? (uint16_t)((cur & 0x000F) | ((val & 0x0FFF) << 4))
			                         : (uint16_t)((cur & 0xF000) | (val & 0x0FFF));
			b[pos] = nv & 0xFF;
			if (straddle) {
				b1[0] = (nv >> 8) & 0xFF;
				if (write_sec(sec, b) != 0)     return -1;
				if (write_sec(sec + 1, b1) != 0) return -1;
			} else {
				b[pos + 1] = (nv >> 8) & 0xFF;
				if (write_sec(sec, b) != 0) return -1;
			}
		}
	}
	return 0;
}

/* Reserve a free cluster (marks it end-of-chain). Returns 0 if the disk is full. */
static uint32_t fat_alloc(void)
{
	for (uint32_t c = 2; c < vol.total_clusters + 2; c++) {
		if (fat_next(c) == 0) {            /* 0 == free */
			if (fat_set(c, eoc_value()) != 0)
				return 0;
			return c;
		}
	}
	return 0;
}

/* Free an entire cluster chain. */
static void fat_free_chain(uint32_t c)
{
	uint32_t guard = 0;
	while (c >= 2 && c != BAD_CLUSTER && !is_eoc(c) && guard++ < vol.total_clusters + 2) {
		uint32_t next = fat_next(c);
		fat_set(c, 0);
		c = next;
	}
}

static int zero_cluster(uint32_t c)
{
	uint8_t z[512];
	fmemset(z, 0, 512);
	uint32_t first = cluster_first_sector(c);
	for (uint32_t s = 0; s < vol.sec_per_clus; s++)
		if (write_sec(first + s, z) != 0)
			return -1;
	return 0;
}

/* ---- directory-entry writing ---- */

static char up_clean(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - 32);
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
	for (const char *ok = "!#$%&'()-@^_`{}~"; *ok; ok++)
		if (*ok == c) return c;
	return '_';                          /* map anything else to underscore */
}

/* Build a space-padded 11-byte 8.3 name from a path component. */
static void make_83(const char *leaf, uint8_t out[11])
{
	for (int i = 0; i < 11; i++) out[i] = ' ';
	int len = 0; while (leaf[len]) len++;
	int dot = -1;
	for (int i = len - 1; i > 0; i--)
		if (leaf[i] == '.') { dot = i; break; }
	int blen = (dot < 0) ? len : dot;
	int o = 0;
	for (int i = 0; i < blen && o < 8; i++) {
		if (leaf[i] == '.' || leaf[i] == ' ') continue;
		out[o++] = up_clean(leaf[i]);
	}
	if (dot >= 0) {
		int e = 0;
		for (int i = dot + 1; leaf[i] && e < 3; i++)
			out[8 + e++] = up_clean(leaf[i]);
	}
	if (out[0] == ' ') out[0] = '_';     /* never an empty base */
}

static int rec83_eq(const uint8_t *e, const uint8_t name[11])
{
	for (int i = 0; i < 11; i++)
		if (e[i] != name[i]) return 0;
	return 1;
}

/* 1 if a record with this exact 8.3 name already exists in the directory. */
static int dir_has_83(uint32_t dir_cluster, const uint8_t name[11])
{
	struct dir_iter it;
	const uint8_t *e;
	diter_init(&it, dir_cluster);
	while ((e = diter_next(&it))) {
		uint8_t a = e[11];
		if (a == 0x0F || (a & 0x08)) continue;
		if (rec83_eq(e, name)) return 1;
	}
	return 0;
}

/* Make `name` unique within the directory by appending ~1..~9 if it collides. */
static void unique_83(uint32_t dir_cluster, uint8_t name[11])
{
	if (!dir_has_83(dir_cluster, name))
		return;
	int base = 0;
	while (base < 8 && name[base] != ' ') base++;
	int pos = base > 6 ? 6 : base;       /* leave room for "~N" */
	for (int n = 1; n <= 9; n++) {
		uint8_t cand[11];
		for (int i = 0; i < 11; i++) cand[i] = name[i];
		cand[pos] = '~'; cand[pos + 1] = (uint8_t)('0' + n);
		for (int i = pos + 2; i < 8; i++) cand[i] = ' ';
		if (!dir_has_83(dir_cluster, cand)) {
			for (int i = 0; i < 11; i++) name[i] = cand[i];
			return;
		}
	}
}

/* Find a free directory slot, extending a cluster-chain directory if full.
 * Returns the (volume-relative) sector and byte offset of the slot. */
static int dir_alloc_slot(uint32_t dir_cluster, uint32_t *out_sec, uint32_t *out_off)
{
	const uint32_t per_sec = 512 / 32;

	if (dir_cluster == 0) {              /* fixed FAT12/16 root */
		uint32_t base = vol.reserved + vol.num_fats * vol.fat_sectors;
		for (uint32_t s = 0; s < vol.root_sectors; s++) {
			const uint8_t *sec;
			if (read_sec(base + s, &sec) != 0) return -1;
			for (uint32_t i = 0; i < per_sec; i++)
				if (sec[i * 32] == 0x00 || sec[i * 32] == 0xE5) {
					*out_sec = base + s; *out_off = i * 32; return 0;
				}
		}
		return -1;                       /* root directory full */
	}

	uint32_t c = dir_cluster, prev = 0;
	while (c >= 2 && c != BAD_CLUSTER && !is_eoc(c)) {
		uint32_t first = cluster_first_sector(c);
		for (uint32_t s = 0; s < vol.sec_per_clus; s++) {
			const uint8_t *sec;
			if (read_sec(first + s, &sec) != 0) return -1;
			for (uint32_t i = 0; i < per_sec; i++)
				if (sec[i * 32] == 0x00 || sec[i * 32] == 0xE5) {
					*out_sec = first + s; *out_off = i * 32; return 0;
				}
		}
		prev = c;
		c = fat_next(c);
	}
	/* No free slot anywhere: append a fresh, zeroed cluster to the chain. */
	uint32_t nc = fat_alloc();
	if (!nc) return -1;
	if (zero_cluster(nc) != 0 || fat_set(prev, nc) != 0) { fat_free_chain(nc); return -1; }
	*out_sec = cluster_first_sector(nc); *out_off = 0;
	return 0;
}

static void fill_entry(uint8_t *e, const uint8_t name[11], uint8_t attr,
                       uint32_t cluster, uint32_t size)
{
	for (int i = 0; i < 11; i++) e[i] = name[i];
	e[11] = attr;
	for (int i = 12; i < 32; i++) e[i] = 0;
	e[20] = (cluster >> 16) & 0xFF; e[21] = (cluster >> 24) & 0xFF;  /* hi word */
	e[26] = cluster & 0xFF;         e[27] = (cluster >> 8) & 0xFF;   /* lo word */
	e[28] = size & 0xFF;       e[29] = (size >> 8) & 0xFF;
	e[30] = (size >> 16) & 0xFF; e[31] = (size >> 24) & 0xFF;
}

static int dir_write_entry(uint32_t sec, uint32_t off, const uint8_t name[11],
                           uint8_t attr, uint32_t cluster, uint32_t size)
{
	uint8_t b[512];
	if (read_secbuf(sec, b) != 0) return -1;
	fill_entry(b + off, name, attr, cluster, size);
	return write_sec(sec, b);
}

/* Stamp a record's first byte 0xE5 (deleted). */
static int wipe_record(uint32_t sec, uint32_t off)
{
	uint8_t b[512];
	if (read_secbuf(sec, b) != 0) return -1;
	b[off] = 0xE5;
	return write_sec(sec, b);
}

/* Split `path` into its parent directory (resolved to a cluster; *is_root set
 * if the parent is the volume root) and the final component `leaf`. */
static int split_parent(const char *path, uint32_t *pcluster, int *is_root,
                        char *leaf, int cap)
{
	while (*path == '/') path++;
	if (!*path) return -1;

	const char *last = path, *p = path;
	while (*p) { if (*p == '/' && p[1] && p[1] != '/') last = p + 1; p++; }

	int n = 0;
	for (const char *q = last; *q; q++) {
		if (*q == '/') return -1;        /* trailing slash -> not a leaf */
		if (n >= cap - 1) return -1;
		leaf[n++] = *q;
	}
	leaf[n] = '\0';
	if (n == 0) return -1;

	char pbuf[256];
	int pl = 0;
	for (const char *q = path; q < last && pl < 255; q++) pbuf[pl++] = *q;
	pbuf[pl] = '\0';
	while (pl > 0 && pbuf[pl - 1] == '/') pbuf[--pl] = '\0';

	struct dirent pd;
	if (resolve(pbuf, &pd) != 0 || !pd.is_dir) return -1;
	*pcluster = pd.cluster;
	*is_root  = (pbuf[0] == '\0');
	return 0;
}

int fat_create(const char *path, const void *data, uint32_t size)
{
	if (vol.kind == FAT_NONE || !vol.write) return -2;

	struct dirent ex;                    /* overwrite an existing file */
	if (resolve(path, &ex) == 0) {
		if (ex.is_dir) return -1;
		if (fat_delete(path) != 0) return -1;
	}

	uint32_t pcluster; int is_root; char leaf[256];
	if (split_parent(path, &pcluster, &is_root, leaf, sizeof leaf) != 0)
		return -1;

	/* Allocate and fill the data cluster chain. */
	uint32_t first = 0, prev = 0, written = 0;
	const uint8_t *src = data;
	while (written < size) {
		uint32_t c = fat_alloc();
		if (!c) { fat_free_chain(first); return -1; }
		if (first == 0) first = c;
		else if (fat_set(prev, c) != 0) { fat_free_chain(first); return -1; }

		uint32_t fsec = cluster_first_sector(c);
		for (uint32_t s = 0; s < vol.sec_per_clus && written < size; s++) {
			uint8_t b[512];
			fmemset(b, 0, 512);
			uint32_t chunk = size - written < 512 ? size - written : 512;
			memcpy(b, src + written, chunk);
			if (write_sec(fsec + s, b) != 0) { fat_free_chain(first); return -1; }
			written += chunk;
		}
		prev = c;
	}

	uint8_t name[11];
	make_83(leaf, name);
	unique_83(pcluster, name);

	uint32_t sec, off;
	if (dir_alloc_slot(pcluster, &sec, &off) != 0) { fat_free_chain(first); return -1; }
	if (dir_write_entry(sec, off, name, 0x20, first, size) != 0) {
		fat_free_chain(first); return -1;
	}
	return 0;
}

int fat_delete(const char *path)
{
	if (vol.kind == FAT_NONE || !vol.write) return -2;

	uint32_t pcluster; int is_root; char leaf[256];
	if (split_parent(path, &pcluster, &is_root, leaf, sizeof leaf) != 0)
		return -1;

	uint32_t ll = 0; while (leaf[ll]) ll++;
	struct dir_iter it; struct dirent d;
	diter_init(&it, pcluster);
	int found = 0;
	while (dir_read(&it, &d))
		if (name_eq(d.name, leaf, ll)) { found = 1; break; }
	if (!found) return -1;
	if (d.is_dir) return -3;             /* a directory -> fat_rmdir */

	if (d.cluster >= 2) fat_free_chain(d.cluster);
	for (int i = 0; i < d.loc_n; i++)
		wipe_record(d.loc_lba[i], d.loc_off[i]);
	return 0;
}

int fat_mkdir(const char *path)
{
	if (vol.kind == FAT_NONE || !vol.write) return -2;

	struct dirent ex;
	if (resolve(path, &ex) == 0) return -1;   /* already exists */

	uint32_t pcluster; int is_root; char leaf[256];
	if (split_parent(path, &pcluster, &is_root, leaf, sizeof leaf) != 0)
		return -1;

	uint32_t dc = fat_alloc();
	if (!dc) return -1;
	if (zero_cluster(dc) != 0) { fat_free_chain(dc); return -1; }

	/* "." -> self, ".." -> parent (0 when the parent is the root). */
	uint8_t dot[11], dotdot[11];
	for (int i = 0; i < 11; i++) { dot[i] = ' '; dotdot[i] = ' '; }
	dot[0] = '.'; dotdot[0] = '.'; dotdot[1] = '.';
	uint8_t b[512];
	fmemset(b, 0, 512);
	fill_entry(b + 0,  dot,    0x10, dc, 0);
	fill_entry(b + 32, dotdot, 0x10, is_root ? 0 : pcluster, 0);
	if (write_sec(cluster_first_sector(dc), b) != 0) { fat_free_chain(dc); return -1; }

	uint8_t name[11];
	make_83(leaf, name);
	unique_83(pcluster, name);

	uint32_t sec, off;
	if (dir_alloc_slot(pcluster, &sec, &off) != 0) { fat_free_chain(dc); return -1; }
	if (dir_write_entry(sec, off, name, 0x10, dc, 0) != 0) { fat_free_chain(dc); return -1; }
	return 0;
}

int fat_rmdir(const char *path)
{
	if (vol.kind == FAT_NONE || !vol.write) return -2;

	struct dirent d;
	if (resolve(path, &d) != 0) return -1;
	if (!d.is_dir) return -2;

	struct dir_iter it; struct dirent e;
	diter_init(&it, d.cluster);
	while (dir_read(&it, &e)) {           /* must be empty but for "." / ".." */
		if (e.name[0] == '.' &&
		    (e.name[1] == '\0' || (e.name[1] == '.' && e.name[2] == '\0')))
			continue;
		return -3;                        /* not empty */
	}

	uint32_t pcluster; int is_root; char leaf[256];
	if (split_parent(path, &pcluster, &is_root, leaf, sizeof leaf) != 0)
		return -1;
	uint32_t ll = 0; while (leaf[ll]) ll++;
	struct dir_iter pit; struct dirent pd;
	diter_init(&pit, pcluster);
	int found = 0;
	while (dir_read(&pit, &pd))
		if (name_eq(pd.name, leaf, ll)) { found = 1; break; }
	if (!found) return -1;

	if (pd.cluster >= 2) fat_free_chain(pd.cluster);
	for (int i = 0; i < pd.loc_n; i++)
		wipe_record(pd.loc_lba[i], pd.loc_off[i]);
	return 0;
}

/* ----------------------------------------------------------------- auto-mount */

int fat_mount_probe(fat_read_fn read, fat_write_fn write)
{
	if (fat_mount(read, write, 0) == 0)    /* whole disk: BPB at LBA 0 */
		return 0;

	/* Otherwise look for an MBR partition table and try each FAT partition. */
	uint8_t mbr[512];
	if (read(0, mbr) != 0 || load_le16(mbr + 510) != 0xAA55)
		return -1;
	for (int i = 0; i < 4; i++) {
		const uint8_t *p = mbr + 446 + i * 16;
		uint8_t type = p[4];
		uint32_t lba = load_le32(p + 8);
		if (type == 0x00 || lba == 0)
			continue;
		/* 0x01 FAT12, 0x04/0x06 FAT16, 0x0B/0x0C FAT32, 0x0E FAT16 LBA */
		if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B ||
		    type == 0x0C || type == 0x0E) {
			if (fat_mount(read, write, lba) == 0)
				return 0;
		}
	}
	return -1;
}
