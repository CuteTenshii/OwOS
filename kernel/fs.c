/* fs.c — owofs implementation (see fs.h for the on-disk layout). */

#include <stddef.h>

#include "fs.h"
#include "blk.h"
#include "procfs.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
#include "iso9660.h"
#include "console.h"
#include "rtc.h"

/* A mounted removable volume (FAT via fat.c, HFS+ via hfs.c, ext2/3/4 via ext.c,
 * or ISO 9660 CD/DVD via iso9660.c) is grafted into the owofs namespace at
 * "/media" -- so the ordinary
 * ls/cat/cd reach it with no per-command special-casing and no parallel verbs.
 * media_subpath() recognises a path inside the mount and returns the path *within*
 * the volume: "" for "/media" itself, "/docs/README.TXT" for the nested file. It
 * returns NULL when nothing is mounted or the path is plain owofs. */
#define MEDIA_MOUNT "media"

/* Exactly one removable volume is mounted at a time; these dispatch to whichever
 * driver owns it. Writes go to FAT only -- HFS+, ext and ISO 9660 are read-only.
 * One if-ladder per verb, in the same driver order, keeps the routing legible. */
static int rvol_mounted(void)
{ return fat_mounted() || hfs_mounted() || ext_mounted() || iso_mounted(); }
static int rvol_readonly(void)
{ return hfs_mounted() || ext_mounted() || iso_mounted(); }

static int rvol_list(const char *p)
{
	if (iso_mounted()) return iso_list(p);
	if (ext_mounted()) return ext_list(p);
	if (hfs_mounted()) return hfs_list(p);
	return fat_list(p);
}
static int rvol_stat(const char *p, int *d, uint32_t *s)
{
	if (iso_mounted()) return iso_stat(p, d, s);
	if (ext_mounted()) return ext_stat(p, d, s);
	if (hfs_mounted()) return hfs_stat(p, d, s);
	return fat_stat(p, d, s);
}
static int rvol_read_at(const char *p, uint32_t o, void *b, uint32_t l)
{
	if (iso_mounted()) return iso_read_at(p, o, b, l);
	if (ext_mounted()) return ext_read_at(p, o, b, l);
	if (hfs_mounted()) return hfs_read_at(p, o, b, l);
	return fat_read_at(p, o, b, l);
}
static int rvol_enum(const char *p, int i, char *n, int *d, uint32_t *s)
{
	if (iso_mounted()) return iso_enum_dir(p, i, n, d, s);
	if (ext_mounted()) return ext_enum_dir(p, i, n, d, s);
	if (hfs_mounted()) return hfs_enum_dir(p, i, n, d, s);
	return fat_enum_dir(p, i, n, d, s);
}

static const char *media_subpath(const char *path)
{
	if (!path || !rvol_mounted())
		return NULL;
	while (*path == '/')               /* tolerate absolute or root-relative */
		path++;
	const char *m = MEDIA_MOUNT;
	while (*m && *path == *m) { path++; m++; }
	if (*m != '\0')                    /* didn't match the whole "media"      */
		return NULL;
	if (*path != '\0' && *path != '/') /* "mediafoo" must not match "media"   */
		return NULL;
	return path;                       /* "" or "/sub/path"                  */
}

#define SECTOR_SIZE     512
#define FS_MAGIC        0x4F574F34u    /* "OWO4" (bumped: 32-entry, 2-sector dir) */
#define FS_MAX_FILES    32             /* 32 entries -> a 2-sector directory      */
#define FS_NAME_LEN     18             /* per-component name: up to 17 + NUL    */
#define SUPER_LBA       0
#define DIR_LBA         1
#define DIR_SECTORS     ((FS_MAX_FILES * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE)
#define DATA_LBA        (DIR_LBA + DIR_SECTORS)   /* data starts past the dir   */
#define FS_MAX_SECTORS  32768          /* 16 MiB disk image                   */

#define FS_DIR_FLAG     0x01u          /* dirent.flags: entry is a directory    */
#define FS_ROOT         0xFFu          /* dirent.parent value meaning "in root" */
#define FS_SLOT_ROOT    (-2)           /* find_path() result for "/" itself     */

/* The 16 directory entries form a tree: each names a single path component and
 * points at its containing directory by slot index (FS_ROOT for the top
 * level). The root directory is implicit -- it has no dirent of its own. */
struct dirent {
	char     name[FS_NAME_LEN];    /* 18: up to 17 chars + NUL                */
	uint8_t  flags;                /* FS_DIR_FLAG if this entry is a directory*/
	uint8_t  parent;               /* containing dir slot, or FS_ROOT         */
	uint32_t size;                 /* 0 for directories                       */
	uint32_t start;                /* first data sector (0 for directories)   */
	uint32_t mtime;                /* packed wall-clock time (see pack_now)   */
};                                     /* 32 bytes; 16 fit one sector         */

struct super {
	uint32_t magic;
	uint32_t next_free;            /* bump pointer for data sectors           */
	uint8_t  pad[SECTOR_SIZE - 8];
};

static struct super  sb;
static struct dirent dir[FS_MAX_FILES];
static int           mounted;

/* The directory table spans DIR_SECTORS consecutive sectors from DIR_LBA. */
static void dir_load(void)
{
	for (int i = 0; i < DIR_SECTORS; i++)
		blk_read_sector(DIR_LBA + i, (uint8_t *)dir + i * SECTOR_SIZE);
}

static int dir_store(void)
{
	for (int i = 0; i < DIR_SECTORS; i++)
		if (blk_write_sector(DIR_LBA + i, (uint8_t *)dir + i * SECTOR_SIZE) != 0)
			return -1;
	return 0;
}

static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < FS_NAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

static void name_copy(char *dst, const char *src)
{
	int i = 0;
	for (; i < FS_NAME_LEN - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* Wall-clock time packed into 32 bits for a dirent (no calendar math needed to
 * unpack): [year-2000:6][month:4][day:5][hour:5][min:6][sec:6]. Good to 2063. */
static uint32_t pack_now(void)
{
	struct rtc_time t;
	rtc_now(&t);
	uint32_t y = (t.year >= 2000) ? (uint32_t)(t.year - 2000) : 0;
	return ((y & 0x3F) << 26) | (((uint32_t)t.month & 0xF) << 22) |
	       (((uint32_t)t.day & 0x1F) << 17) | (((uint32_t)t.hour & 0x1F) << 12) |
	       (((uint32_t)t.min & 0x3F) << 6) | ((uint32_t)t.sec & 0x3F);
}

/* Compare two NUL-terminated strings for equality (for "."/".." components). */
static int s_eq(const char *a, const char *b)
{
	int i = 0;
	for (; a[i] && b[i]; i++)
		if (a[i] != b[i])
			return 0;
	return a[i] == b[i];
}

/* The first unused directory slot, or -1 when all 16 are taken. */
static int free_slot(void)
{
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (!dir[i].name[0])
			return i;
	return -1;
}

/* The slot of the entry named `leaf` directly inside directory `parent`
 * (FS_ROOT for the top level), or -1 if there is none. */
static int find_in(int parent, const char *leaf)
{
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (dir[i].name[0] && dir[i].parent == (uint8_t)parent &&
		    name_eq(dir[i].name, leaf))
			return i;
	return -1;
}

/* Walk `path` to its containing directory: resolve every component but the
 * last, which is returned in leaf[] (>= FS_NAME_LEN bytes). *parent receives
 * the slot of the directory that should hold leaf (FS_ROOT for the top level).
 * Leading/duplicated slashes and "."/".." are honored; a trailing slash (or a
 * path naming a directory, e.g. "/" or "..") yields an empty leaf. Returns 0,
 * or -1 if a component is too long or an intermediate name isn't a directory. */
static int resolve_path(const char *path, int *parent, char *leaf)
{
	int cur = FS_ROOT;
	const char *p = path;
	char comp[FS_NAME_LEN];

	for (;;) {
		while (*p == '/')
			p++;
		if (*p == '\0') {                  /* nothing left -> name a directory */
			leaf[0] = '\0';
			*parent = cur;
			return 0;
		}
		int n = 0;
		while (p[n] && p[n] != '/')
			n++;
		if (n >= FS_NAME_LEN)
			return -1;                 /* component too long               */
		for (int i = 0; i < n; i++)
			comp[i] = p[i];
		comp[n] = '\0';
		p += n;
		while (*p == '/')
			p++;
		int last = (*p == '\0');

		if (s_eq(comp, ".")) {
			if (last) { leaf[0] = '\0'; *parent = cur; return 0; }
			continue;
		}
		if (s_eq(comp, "..")) {
			if (cur != FS_ROOT)
				cur = dir[cur].parent;
			if (last) { leaf[0] = '\0'; *parent = cur; return 0; }
			continue;
		}
		if (last) {
			name_copy(leaf, comp);
			*parent = cur;
			return 0;
		}
		int s = find_in(cur, comp);        /* descend into a subdirectory      */
		if (s < 0 || !(dir[s].flags & FS_DIR_FLAG))
			return -1;
		cur = s;
	}
}

/* Resolve `path` to an existing entry's slot, FS_SLOT_ROOT for the root
 * directory, or -1 if it does not exist. */
static int find_path(const char *path)
{
	int parent;
	char leaf[FS_NAME_LEN];
	if (resolve_path(path, &parent, leaf) != 0)
		return -1;
	if (leaf[0] == '\0')
		return (parent == FS_ROOT) ? FS_SLOT_ROOT : parent;
	return find_in(parent, leaf);
}

/* Resolve `path` to the slot of an existing *file* (not a directory), or -1. */
static int find_file(const char *path)
{
	int s = find_path(path);
	if (s < 0 || (dir[s].flags & FS_DIR_FLAG))
		return -1;
	return s;
}

void fs_format(void)
{
	sb.magic = FS_MAGIC;
	sb.next_free = DATA_LBA;
	for (unsigned i = 0; i < sizeof(sb.pad); i++)
		sb.pad[i] = 0;
	for (int i = 0; i < FS_MAX_FILES; i++) {
		dir[i].name[0] = '\0';
		dir[i].flags = 0;
		dir[i].parent = FS_ROOT;
		dir[i].size = 0;
		dir[i].start = 0;
		dir[i].mtime = 0;
	}
	blk_write_sector(SUPER_LBA, &sb);
	dir_store();
	mounted = 1;
}

void fs_init(void)
{
	int have = (blk_read_sector(SUPER_LBA, &sb) == 0);

	if (have && sb.magic == FS_MAGIC) {        /* a real owofs disk -- mount it  */
		dir_load();
		mounted = 1;
		return;
	}
	if (have && blk_is_ide()) {                /* blank IDE dev disk -- make owofs*/
		con_write("fs: unformatted IDE disk -- creating owofs.\n");
		fs_format();
		return;
	}

	/* No usable persistent disk: either no controller at all, or an AHCI/NVMe
	 * disk that is the user's OS drive and must NOT be auto-formatted (it has no
	 * owofs magic because it holds a real partition table). Fall back to a
	 * volatile RAM-backed filesystem so the system still has a working fs; the
	 * real disk is only ever read (sector 0 above), never written. */
	if (blk_use_ram() == 0) {
		con_write("fs: no owofs disk -- using RAM (tmpfs; not persistent).\n");
		fs_format();                       /* formats + mounts the RAM disk   */
		return;
	}
	con_write("fs: no disk and RAM fs unavailable.\n");
	mounted = 0;
}

/* Print a zero-padded decimal of the given width (1..) */
static void write_pad(uint32_t v, int width)
{
	uint32_t div = 1;
	for (int i = 1; i < width; i++)
		div *= 10;
	for (; div > 0; div /= 10)
		con_putc((char)('0' + (v / div) % 10));
}

/* Print a packed mtime as "YYYY-MM-DD HH:MM:SS" (see pack_now). */
static void write_mtime(uint32_t m)
{
	if (m == 0) {                          /* never stamped                    */
		con_write("       -          ");
		return;
	}
	write_pad(2000 + (m >> 26), 4);    con_putc('-');
	write_pad((m >> 22) & 0xF, 2);     con_putc('-');
	write_pad((m >> 17) & 0x1F, 2);    con_putc(' ');
	write_pad((m >> 12) & 0x1F, 2);    con_putc(':');
	write_pad((m >> 6) & 0x3F, 2);     con_putc(':');
	write_pad(m & 0x3F, 2);
}

/* Print one directory row: mtime, size (or "<DIR>"), and the name. */
static void list_row(int i)
{
	write_mtime(dir[i].mtime);
	con_write("  ");
	if (dir[i].flags & FS_DIR_FLAG)
		con_write("   <DIR>");            /* same width as the size column     */
	else
		write_pad(dir[i].size, 8);        /* right-aligned size column         */
	con_write("  ");
	con_write(dir[i].name);
	if (dir[i].flags & FS_DIR_FLAG)
		con_putc('/');
	con_putc('\n');
}

void fs_list(const char *path)
{
	const char *proc = procfs_subpath(path);
	if (proc) {
		int k = procfs_kind(proc);
		if (k < 0) { con_write("ls: no such file or directory\n"); return; }
		char nm[20];
		if (k == 0) {                  /* names one /proc file -> list just it  */
			const char *nf = proc[0] == '/' ? proc + 1 : proc;
			write_mtime(0); con_write("  "); write_pad((uint32_t)procfs_size(nf), 8);
			con_write("  "); con_write(nf); con_putc('\n');
			return;
		}
		for (int i = 0; procfs_list(i, nm, sizeof nm); i++) {   /* the dir     */
			write_mtime(0); con_write("  "); write_pad((uint32_t)procfs_size(nm), 8);
			con_write("  "); con_write(nm); con_putc('\n');
		}
		return;
	}
	if (!mounted) {
		con_write("fs: not mounted\n");
		return;
	}
	const char *sub = media_subpath(path);
	if (sub) {                             /* under the FAT mount -> fat.c      */
		if (rvol_list(sub) < 0)
			con_write("ls: no such file or directory\n");
		return;
	}
	int d = (path && path[0]) ? find_path(path) : FS_SLOT_ROOT;
	if (d == -1) {
		con_write("ls: no such file or directory\n");
		return;
	}
	if (d != FS_SLOT_ROOT && !(dir[d].flags & FS_DIR_FLAG)) {
		list_row(d);                       /* path names a file -> list just it */
		return;
	}
	int parent = (d == FS_SLOT_ROOT) ? (int)FS_ROOT : d;
	int n = 0;
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (dir[i].name[0] && dir[i].parent == (uint8_t)parent) {
			list_row(i);
			n++;
		}
	if (d == FS_SLOT_ROOT && rvol_mounted()) {  /* surface the mount point      */
		con_write("       -            <DIR>  " MEDIA_MOUNT "/\n");
		n++;
	}
	if (!n)
		con_write("(empty)\n");
}

int fs_read(const char *name, void *buf, uint32_t maxlen)
{
	const char *proc = procfs_subpath(name);   /* /proc entries generated on read */
	if (proc)
		return procfs_read(proc, (char *)buf, (int)maxlen);
	if (!mounted)
		return -1;
	const char *sub = media_subpath(name);
	if (sub) {
		int r = rvol_read_at(sub, 0, buf, maxlen);
		return r < 0 ? -1 : r;
	}
	int i = find_file(name);
	if (i < 0)
		return -1;

	uint32_t want = dir[i].size < maxlen ? dir[i].size : maxlen;
	uint32_t sectors = (want + SECTOR_SIZE - 1) / SECTOR_SIZE;
	uint8_t  tmp[SECTOR_SIZE];
	uint32_t copied = 0;

	for (uint32_t s = 0; s < sectors; s++) {
		if (blk_read_sector(dir[i].start + s, tmp) != 0)
			return -1;
		uint32_t chunk = (want - copied) < SECTOR_SIZE
		                 ? (want - copied) : SECTOR_SIZE;
		for (uint32_t k = 0; k < chunk; k++)
			((uint8_t *)buf)[copied + k] = tmp[k];
		copied += chunk;
	}
	return (int)want;
}

/* Size of a file in bytes, or -1 if it does not exist (or names a directory). */
int fs_size(const char *name)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(name);
	if (sub) {
		int is_dir; uint32_t sz;
		if (rvol_stat(sub, &is_dir, &sz) != 0 || is_dir)
			return -1;
		return (int)sz;
	}
	int i = find_file(name);
	return (i < 0) ? -1 : (int)dir[i].size;
}

/* Read up to len bytes starting at byte offset within a file -- lets a large
 * file be streamed in bounded chunks (owofs stores files contiguously, so this
 * is just sector math). Returns bytes read (0 at/after EOF), or -1 if absent. */
int fs_read_at(const char *name, uint32_t offset, void *buf, uint32_t len)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(name);
	if (sub) {
		int r = rvol_read_at(sub, offset, buf, len);
		return r < 0 ? -1 : r;
	}
	int i = find_file(name);
	if (i < 0)
		return -1;
	if (offset >= dir[i].size)
		return 0;
	uint32_t avail = dir[i].size - offset;
	uint32_t want  = avail < len ? avail : len;
	uint8_t  tmp[SECTOR_SIZE];
	uint32_t copied = 0;

	while (copied < want) {
		uint32_t pos   = offset + copied;
		uint32_t sec   = dir[i].start + pos / SECTOR_SIZE;
		uint32_t soff  = pos % SECTOR_SIZE;
		uint32_t left  = want - copied;

		/* Whole sectors landing on a sector boundary go straight into the
		 * caller's buffer in one batched read — no per-sector bounce through
		 * tmp. A streaming reader (4 KiB-aligned, e.g. imgview) hits this for
		 * all but a possible tail, turning a megabyte into a handful of
		 * READ SECTORS commands. */
		if (soff == 0 && left >= SECTOR_SIZE) {
			uint32_t nsec = left / SECTOR_SIZE;
			if (blk_read(sec, (uint8_t *)buf + copied, nsec) != 0)
				return (int)copied;   /* partial read on I/O error */
			copied += nsec * SECTOR_SIZE;
			continue;
		}

		/* Partial leading/trailing sector: read one and copy the slice. */
		if (blk_read_sector(sec, tmp) != 0)
			return (int)copied;       /* partial read on I/O error */
		uint32_t chunk = SECTOR_SIZE - soff;
		if (chunk > left)
			chunk = left;
		for (uint32_t k = 0; k < chunk; k++)
			((uint8_t *)buf)[copied + k] = tmp[soff + k];
		copied += chunk;
	}
	return (int)copied;
}

int fs_create(const char *name, const void *data, uint32_t size)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(name);
	if (sub) {
		if (rvol_readonly()) return -1;        /* HFS+ is read-only */
		return fat_create(sub, data, size) == 0 ? 0 : -1;
	}

	int parent;
	char leaf[FS_NAME_LEN];
	if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
		return -1;                                 /* bad path / names a dir   */

	int slot = find_in(parent, leaf);              /* overwrite if it exists   */
	if (slot >= 0 && (dir[slot].flags & FS_DIR_FLAG))
		return -1;                                 /* can't clobber a directory*/
	if (slot < 0)
		slot = free_slot();
	if (slot < 0)
		return -1;                                 /* directory full           */

	uint32_t sectors = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (sectors == 0)
		sectors = 1;
	if (sb.next_free + sectors > FS_MAX_SECTORS)
		return -1;                                 /* disk full                */

	uint32_t start = sb.next_free;
	sb.next_free += sectors;

	uint8_t  tmp[SECTOR_SIZE];
	uint32_t written = 0;
	for (uint32_t s = 0; s < sectors; s++) {
		for (int k = 0; k < SECTOR_SIZE; k++)
			tmp[k] = 0;
		uint32_t chunk = (size - written) < SECTOR_SIZE
		                 ? (size - written) : SECTOR_SIZE;
		for (uint32_t k = 0; k < chunk; k++)
			tmp[k] = ((const uint8_t *)data)[written + k];
		if (blk_write_sector(start + s, tmp) != 0)
			return -1;
		written += chunk;
	}

	name_copy(dir[slot].name, leaf);
	dir[slot].flags = 0;
	dir[slot].parent = (uint8_t)parent;
	dir[slot].size = size;
	dir[slot].start = start;
	dir[slot].mtime = pack_now();

	if (blk_write_sector(SUPER_LBA, &sb) != 0)
		return -1;
	if (dir_store() != 0)
		return -1;
	return 0;
}

/* Wipe a directory slot (the entry's data sectors, if any, leak). */
static void slot_clear(int i)
{
	dir[i].name[0] = '\0';
	dir[i].flags = 0;
	dir[i].parent = FS_ROOT;
	dir[i].size = 0;
	dir[i].start = 0;
	dir[i].mtime = 0;
}

int fs_delete(const char *name)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(name);
	if (sub) {
		if (rvol_readonly()) return -1;        /* HFS+ is read-only */
		int r = fat_delete(sub);
		if (r == 0)  return 0;
		if (r == -3) return -2;          /* it's a directory -> fs_rmdir */
		return -1;
	}
	int i = find_path(name);
	if (i < 0 || i == FS_SLOT_ROOT)
		return -1;                                 /* no such file                    */
	if (dir[i].flags & FS_DIR_FLAG)
		return -2;                                 /* it's a directory -> fs_rmdir    */
	slot_clear(i);
	return (dir_store() == 0) ? 0 : -1;
}

int fs_rename(const char *old, const char *neu)
{
	if (!mounted)
		return -1;

	int src = find_path(old);
	if (src < 0 || src == FS_SLOT_ROOT)
		return -1;                                 /* no such source                  */

	int dparent;
	char dleaf[FS_NAME_LEN];
	if (resolve_path(neu, &dparent, dleaf) != 0 || dleaf[0] == '\0')
		return -1;                                 /* bad/empty target                */

	if (dir[src].flags & FS_DIR_FLAG) {            /* don't move a dir into itself    */
		int c = dparent;
		while (c != (int)FS_ROOT) {
			if (c == src)
				return -1;
			c = dir[c].parent;
		}
	}

	int ex = find_in(dparent, dleaf);              /* overwrite an existing target    */
	if (ex >= 0 && ex != src) {
		if (dir[ex].flags & FS_DIR_FLAG)
			return -1;                         /* refuse to clobber a directory   */
		slot_clear(ex);                            /* its sectors leak                */
	}

	name_copy(dir[src].name, dleaf);
	dir[src].parent = (uint8_t)dparent;
	dir[src].mtime = pack_now();
	return (dir_store() == 0) ? 0 : -1;
}

int fs_mkdir(const char *path)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(path);
	if (sub) {
		if (rvol_readonly()) return -1;        /* HFS+ is read-only */
		return fat_mkdir(sub) == 0 ? 0 : -1;
	}
	int parent;
	char leaf[FS_NAME_LEN];
	if (resolve_path(path, &parent, leaf) != 0 || leaf[0] == '\0')
		return -1;                                 /* bad path / "/" itself           */
	if (find_in(parent, leaf) >= 0)
		return -1;                                 /* name already taken              */
	int slot = free_slot();
	if (slot < 0)
		return -1;                                 /* directory full                  */
	name_copy(dir[slot].name, leaf);
	dir[slot].flags = FS_DIR_FLAG;
	dir[slot].parent = (uint8_t)parent;
	dir[slot].size = 0;
	dir[slot].start = 0;
	dir[slot].mtime = pack_now();
	return (dir_store() == 0) ? 0 : -1;
}

int fs_rmdir(const char *path)
{
	if (!mounted)
		return -1;
	const char *sub = media_subpath(path);
	if (sub) {
		if (rvol_readonly()) return -1;        /* HFS+ is read-only */
		int r = fat_rmdir(sub);
		return (r == 0) ? 0 : (r == -2) ? -2 : (r == -3) ? -3 : -1;
	}
	int s = find_path(path);
	if (s < 0 || s == FS_SLOT_ROOT)
		return -1;                                 /* not found / can't remove root   */
	if (!(dir[s].flags & FS_DIR_FLAG))
		return -2;                                 /* not a directory                 */
	for (int i = 0; i < FS_MAX_FILES; i++)         /* must be empty                   */
		if (dir[i].name[0] && dir[i].parent == (uint8_t)s)
			return -3;
	slot_clear(s);
	return (dir_store() == 0) ? 0 : -1;
}

int fs_isdir(const char *path)
{
	const char *sub = media_subpath(path);
	if (sub) {
		int is_dir;
		if (rvol_stat(sub, &is_dir, NULL) != 0)
			return -1;
		return is_dir ? 1 : 0;
	}
	int s = find_path(path);
	if (s == FS_SLOT_ROOT)
		return 1;
	if (s < 0)
		return -1;
	return (dir[s].flags & FS_DIR_FLAG) ? 1 : 0;
}

int fs_prealloc(const char *name, uint32_t size)
{
	if (!mounted)
		return -1;

	int parent;
	char leaf[FS_NAME_LEN];
	if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
		return -1;

	int slot = find_in(parent, leaf);
	if (slot >= 0 && (dir[slot].flags & FS_DIR_FLAG))
		return -1;                                 /* can't clobber a directory       */
	if (slot < 0)
		slot = free_slot();
	if (slot < 0)
		return -1;                                 /* directory full           */

	uint32_t sectors = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (sectors == 0)
		sectors = 1;
	if (sb.next_free + sectors > FS_MAX_SECTORS)
		return -1;                                 /* disk full                */

	uint32_t start = sb.next_free;

	uint8_t zero[SECTOR_SIZE];
	for (int k = 0; k < SECTOR_SIZE; k++)
		zero[k] = 0;
	for (uint32_t s = 0; s < sectors; s++)
		if (blk_write_sector(start + s, zero) != 0)
			return -1;                         /* leave next_free untouched */

	sb.next_free += sectors;                           /* commit the reservation */
	name_copy(dir[slot].name, leaf);
	dir[slot].flags = 0;
	dir[slot].parent = (uint8_t)parent;
	dir[slot].size  = size;
	dir[slot].start = start;
	dir[slot].mtime = pack_now();

	if (blk_write_sector(SUPER_LBA, &sb) != 0)
		return -1;
	if (dir_store() != 0)
		return -1;
	return 0;
}

int fs_write_at(const char *name, uint32_t offset, const void *data, uint32_t len)
{
	if (!mounted)
		return -1;
	int i = find_file(name);
	if (i >= 0) {
		if (offset > dir[i].size || len > dir[i].size - offset)
			return -1;                         /* would write past EOF (no grow) */

		const uint8_t *p = data;
		uint8_t  tmp[SECTOR_SIZE];
		uint32_t written = 0;
		while (written < len) {
			uint32_t pos   = offset + written;
			uint32_t sec   = dir[i].start + pos / SECTOR_SIZE;
			uint32_t soff  = pos % SECTOR_SIZE;
			uint32_t chunk = SECTOR_SIZE - soff;
			if (chunk > len - written)
				chunk = len - written;

			if (soff != 0 || chunk != SECTOR_SIZE) {  /* partial: read-modify-write */
				if (blk_read_sector(sec, tmp) != 0)
					return -1;
			}
			for (uint32_t k = 0; k < chunk; k++)
				tmp[soff + k] = p[written + k];
			if (blk_write_sector(sec, tmp) != 0)
				return -1;
			written += chunk;
		}
		return (int)written;
	}
	return -1;                                         /* no such file             */
}

/* Streaming creation, for data whose total size isn't known until the last
 * byte arrives (e.g. an HTTP download). begin -> write* -> end. Bytes are
 * staged a sector at a time so the whole file never has to live in RAM. Not
 * reentrant: one stream at a time, which is all the single-threaded shell
 * needs. The data-sector bump pointer is only committed in fs_stream_end, so a
 * download that dies partway just abandons its sectors -- nothing is leaked
 * into the directory and the next allocation reuses them. */
static int      strm_active;
static char     strm_name[FS_NAME_LEN]; /* final path component (leaf)          */
static int      strm_parent;           /* slot of the directory to create it in */
static uint32_t strm_start;            /* first data sector (reserved at begin) */
static uint32_t strm_sectors;          /* full sectors flushed so far           */
static uint32_t strm_size;             /* bytes accepted so far                 */
static uint8_t  strm_buf[SECTOR_SIZE]; /* the sector currently filling          */
static uint32_t strm_fill;             /* bytes pending in strm_buf             */

int fs_stream_begin(const char *name)
{
	if (!mounted || strm_active)
		return -1;
	if (resolve_path(name, &strm_parent, strm_name) != 0 || strm_name[0] == '\0')
		return -1;                             /* bad path / names a directory     */
	strm_start = sb.next_free;
	strm_sectors = 0;
	strm_size = 0;
	strm_fill = 0;
	strm_active = 1;
	return 0;
}

int fs_stream_write(const void *data, uint32_t size)
{
	if (!strm_active)
		return -1;
	const uint8_t *p = data;
	for (uint32_t i = 0; i < size; i++) {
		strm_buf[strm_fill++] = p[i];
		strm_size++;
		if (strm_fill == SECTOR_SIZE) {
			if (strm_start + strm_sectors >= FS_MAX_SECTORS ||
			    blk_write_sector(strm_start + strm_sectors, strm_buf) != 0) {
				strm_active = 0;       /* disk full / I/O error: bail out */
				return -1;
			}
			strm_sectors++;
			strm_fill = 0;
		}
	}
	return 0;
}

int fs_stream_end(void)
{
	if (!strm_active)
		return -1;
	strm_active = 0;

	if (strm_fill > 0) {                   /* flush the partial final sector   */
		for (uint32_t k = strm_fill; k < SECTOR_SIZE; k++)
			strm_buf[k] = 0;
		if (strm_start + strm_sectors >= FS_MAX_SECTORS ||
		    blk_write_sector(strm_start + strm_sectors, strm_buf) != 0)
			return -1;
		strm_sectors++;
		strm_fill = 0;
	}

	int slot = find_in(strm_parent, strm_name);    /* overwrite if it exists   */
	if (slot >= 0 && (dir[slot].flags & FS_DIR_FLAG))
		return -1;                                 /* can't clobber a directory*/
	if (slot < 0)
		slot = free_slot();
	if (slot < 0)
		return -1;                                 /* directory full           */

	sb.next_free = strm_start + strm_sectors;      /* commit the bump pointer  */
	name_copy(dir[slot].name, strm_name);
	dir[slot].flags = 0;
	dir[slot].parent = (uint8_t)strm_parent;
	dir[slot].size = strm_size;
	dir[slot].start = strm_start;
	dir[slot].mtime = pack_now();

	if (blk_write_sector(SUPER_LBA, &sb) != 0)
		return -1;
	if (dir_store() != 0)
		return -1;
	return (int)strm_size;
}

int fs_file_count(void)
{
	int n = 0;
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (dir[i].name[0] && !(dir[i].flags & FS_DIR_FLAG))
			n++;
	return n;
}

uint32_t fs_used_bytes(void)
{
	uint32_t total = 0;
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (dir[i].name[0] && !(dir[i].flags & FS_DIR_FLAG))
			total += dir[i].size;
	return total;
}

/* --------------------------------------------------------------- self-test */

int fs_selftest(void)
{
	const char *fn = "fstest.tmp";
	const uint32_t SZ = 1000;              /* spans two sectors: 512 + 488     */
	uint8_t buf[1024];
	int ok = 1;

	if (fs_prealloc(fn, SZ) != 0) {
		con_write("fs selftest: prealloc FAILED\n");
		return -1;
	}

	/* A freshly pre-allocated file reads back as all zeros. */
	int zeros_ok = (fs_read(fn, buf, SZ) == (int)SZ);
	for (uint32_t i = 0; i < SZ && zeros_ok; i++)
		if (buf[i] != 0) zeros_ok = 0;
	con_write("  prealloc+zero "); con_write(zeros_ok ? "PASS\n" : "FAIL\n");

	/* Out-of-order writes: end, start, then across the sector boundary. */
	int w_ok = 1;
	w_ok &= fs_write_at(fn, 996, "1234", 4) == 4;   /* last 4 bytes           */
	w_ok &= fs_write_at(fn, 0,   "ABCD", 4) == 4;   /* partial, sector start  */
	w_ok &= fs_write_at(fn, 510, "WXYZ", 4) == 4;   /* crosses 512 boundary   */
	con_write("  write_at      "); con_write(w_ok ? "PASS\n" : "FAIL\n");

	/* A write that would run past EOF must be rejected, not grow the file. */
	int bounds_ok = (fs_write_at(fn, 998, "XXXX", 4) == -1) &&
	                (fs_write_at("nope.tmp", 0, "X", 1) == -1);
	con_write("  bounds        "); con_write(bounds_ok ? "PASS\n" : "FAIL\n");

	/* Read the whole file back and check the patches landed and nothing else
	 * changed. */
	int rb_ok = (fs_read(fn, buf, SZ) == (int)SZ);
	for (uint32_t i = 0; i < SZ && rb_ok; i++) {
		uint8_t want = 0;
		if (i < 4)              want = (uint8_t)"ABCD"[i];
		else if (i >= 510 && i < 514) want = (uint8_t)"WXYZ"[i - 510];
		else if (i >= 996)      want = (uint8_t)"1234"[i - 996];
		if (buf[i] != want) rb_ok = 0;
	}
	con_write("  readback      "); con_write(rb_ok ? "PASS\n" : "FAIL\n");

	ok = zeros_ok && w_ok && bounds_ok && rb_ok;
	con_write(ok ? "fs selftest: PASS\n" : "fs selftest: FAIL\n");
	return ok ? 0 : -1;
}

int fs_enum(int idx, char *name, uint32_t *size, uint32_t *mtime)
{
	if (!mounted || idx < 0 || idx >= FS_MAX_FILES || !dir[idx].name[0])
		return 0;
	if ((dir[idx].flags & FS_DIR_FLAG) || dir[idx].parent != FS_ROOT)
		return 0;                  /* SMB sees a flat share of root-level files */
	int i = 0;
	for (; i < FS_NAME_LEN && dir[idx].name[i]; i++)
		name[i] = dir[idx].name[i];
	name[i] = '\0';
	if (size)  *size = dir[idx].size;
	if (mtime) *mtime = dir[idx].mtime;
	return 1;
}

int fs_stat(const char *path, int *is_dir, uint32_t *size, uint32_t *mtime)
{
	const char *proc = procfs_subpath(path);
	if (proc) {
		int k = procfs_kind(proc);     /* 1 dir (/proc) / 0 file / -1 unknown  */
		if (k < 0) return -1;
		if (is_dir) *is_dir = (k == 1);
		if (size)   *size = (k == 0) ? (uint32_t)procfs_size(proc) : 0;
		if (mtime)  *mtime = 0;
		return 0;
	}
	if (!mounted)
		return -1;
	const char *sub = media_subpath(path);
	if (sub) {
		int d; uint32_t sz;
		if (rvol_stat(sub, &d, &sz) != 0)
			return -1;
		if (is_dir) *is_dir = d;
		if (size)   *size = sz;
		if (mtime)  *mtime = 0;        /* FAT mtimes not surfaced yet */
		return 0;
	}
	int slot = find_path((path && path[0]) ? path : "/");
	if (slot == FS_SLOT_ROOT) {        /* the root directory itself */
		if (is_dir) *is_dir = 1;
		if (size)   *size = 0;
		if (mtime)  *mtime = 0;
		return 0;
	}
	if (slot < 0)
		return -1;
	if (is_dir) *is_dir = (dir[slot].flags & FS_DIR_FLAG) ? 1 : 0;
	if (size)   *size = dir[slot].size;
	if (mtime)  *mtime = dir[slot].mtime;
	return 0;
}

int fs_enum_dir(const char *path, int idx, char *name, int *is_dir,
                uint32_t *size, uint32_t *mtime)
{
	const char *proc = procfs_subpath(path);
	if (proc && !proc[0]) {            /* listing /proc itself                 */
		if (idx < 0 || !procfs_list(idx, name, 20))
			return 0;
		if (is_dir) *is_dir = 0;
		if (size)   *size = (uint32_t)procfs_size(name);
		if (mtime)  *mtime = 0;
		return 1;
	}
	if (!mounted || idx < 0)
		return 0;
	const char *sub = media_subpath(path);
	if (sub) {                         /* browsing inside the FAT mount      */
		int d; uint32_t sz;
		if (!rvol_enum(sub, idx, name, &d, &sz))
			return 0;
		if (is_dir) *is_dir = d;
		if (size)   *size = sz;
		if (mtime)  *mtime = 0;
		return 1;
	}
	int slot = find_path((path && path[0]) ? path : "/");
	uint8_t parent;
	if (slot == FS_SLOT_ROOT)
		parent = FS_ROOT;
	else if (slot >= 0 && (dir[slot].flags & FS_DIR_FLAG))
		parent = (uint8_t)slot;
	else
		return 0;                  /* not a directory */

	int count = 0;
	for (int i = 0; i < FS_MAX_FILES; i++) {
		if (!dir[i].name[0] || dir[i].parent != parent)
			continue;
		if (count == idx) {
			int j = 0;
			for (; j < FS_NAME_LEN && dir[i].name[j]; j++)
				name[j] = dir[i].name[j];
			name[j] = '\0';
			if (is_dir) *is_dir = (dir[i].flags & FS_DIR_FLAG) ? 1 : 0;
			if (size)   *size = dir[i].size;
			if (mtime)  *mtime = dir[i].mtime;
			return 1;
		}
		count++;
	}
	/* The FAT volume appears as a synthetic "fat" directory in the root. */
	if (parent == FS_ROOT && rvol_mounted()) {
		if (count == idx) {
			int j = 0;
			for (; MEDIA_MOUNT[j] && j < FS_NAME_LEN - 1; j++)
				name[j] = MEDIA_MOUNT[j];
			name[j] = '\0';
			if (is_dir) *is_dir = 1;
			if (size)   *size = 0;
			if (mtime)  *mtime = 0;
			return 1;
		}
		count++;
	}
	return 0;
}
