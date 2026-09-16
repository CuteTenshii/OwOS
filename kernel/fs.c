/* fs.c — owofs implementation (see fs.h for the on-disk layout). */

#include <stddef.h>

#include "blk.h"
#include "console.h"
#include "fs.h"
#include "fsmount.h"
#include "heap.h"
#include "procfs.h"
#include "rtc.h"
#include "sysfs.h"

/* ----------------------------------------------------------- mount table ---
 *
 * Removable volumes are grafted into the owofs namespace under "/media/<name>"
 * -- so the ordinary ls/cat/cd reach them with no per-command special-casing.
 * Each mounted volume is a slot in mtab[]: a mount point ("/media/usb0"), the
 * /dev name it came from, and the driver's fs_ops + opaque instance. Because no
 * driver keeps global state any more, N volumes (including two of the same
 * type) mount at once.
 *
 * mount_for() does a longest-prefix match of a path against the active mount
 * points and returns the owning mount plus the path *within* the volume ("" for
 * the mount point itself, "/docs/README.TXT" for a nested file), or NULL for a
 * plain-owofs path. The thin rvol_* dispatchers just call through the resolved
 * mount's fs_ops; a mount is writable iff its ops expose create().
 *
 * The synthetic kernel filesystems /dev, /proc and /sys live in this same table
 * as `permanent` mounts (see the pseudo-filesystem section below): they are
 * real fs_ops mounts routed through mount_for() exactly like a removable
 * volume, so every fs op handles them generically -- no per-path
 * special-casing. Being permanent, they never unmount and are hidden from the
 * removable-volume views (fs_mount_enum / `umount`). */
#define MAX_MOUNTS 12

struct mnt {
  int used;
  int permanent;  /* a kernel pseudo-fs: never unmounts  */
  char point[48]; /* "/media/usb0", "/proc"              */
  char dev[16];   /* "usb0", "proc"                      */
  const struct fs_ops *ops;
  void *inst;
};

static struct mnt mtab[MAX_MOUNTS];

/* Length of a NUL-terminated string. */
static int s_len(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

/* Longest-prefix match of `path` against the active mount points. A match needs
 * the whole point string to match and the next char of path to be '\0' or '/'.
 * On a match *sub is the remainder within the volume ("" when path == the mount
 * point, else the "/..." tail). Returns the owning mnt, or NULL. */
static struct mnt *mount_for(const char *path, const char **sub)
{
  if (!path)
    return NULL;
  struct mnt *best = NULL;
  int best_len = -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mtab[i].used)
      continue;
    const char *p = path;
    const char *m = mtab[i].point;
    while (*m && *p == *m) {
      p++;
      m++;
    }
    if (*m != '\0') /* didn't match the whole point        */
      continue;
    if (*p != '\0' && *p != '/') /* "/media/usb0x" must not match       */
      continue;
    int len = s_len(mtab[i].point);
    if (len > best_len) {
      best = &mtab[i];
      best_len = len;
      *sub = p; /* "" or "/sub/path"                   */
    }
  }
  return best;
}

/* A mount is writable iff its driver exposes create(). */
static int rvol_readonly(struct mnt *m) { return m->ops->create == NULL; }

static int rvol_list(struct mnt *m, const char *p) { return m->ops->list(m->inst, p); }
static int rvol_stat(struct mnt *m, const char *p, int *d, uint32_t *s) { return m->ops->stat(m->inst, p, d, s); }
static int rvol_read_at(struct mnt *m, const char *p, uint32_t o, void *b, uint32_t l)
{
  return m->ops->read_at(m->inst, p, o, b, l);
}
static int rvol_enum(struct mnt *m, const char *p, int i, char *n, int *d, uint32_t *s)
{
  return m->ops->enum_dir(m->inst, p, i, n, d, s);
}

/* The leaf name of a mount point ("/media/usb0" -> "usb0"). */
static const char *mnt_leaf(const struct mnt *m)
{
  const char *s = m->point;
  const char *leaf = s;
  for (const char *p = s; *p; p++)
    if (*p == '/')
      leaf = p + 1;
  return leaf;
}

/* True for "/media" or "/media/" -- the synthetic mount-root directory. */
static int is_media_root(const char *path)
{
  if (!path)
    return 0;
  const char *m = "/media";
  while (*m && *path == *m) {
    path++;
    m++;
  }
  if (*m != '\0')
    return 0;
  return *path == '\0' || (*path == '/' && path[1] == '\0');
}

#define SECTOR_SIZE 512
#define FS_MAGIC 0x4F574F38u /* "OWO8" (growable directory; no fixed cap) */
#define FS_NAME_LEN 64       /* per-component name: up to 63 + NUL    */
#define SUPER_LBA 0
#define RESERVED_SECTORS 1   /* only sector 0 (the superblock) is fixed */
#define DIR_INIT_ENTRIES 256 /* directory starts this big, then grows  */
#define FS_MAX_DIR_SECTORS                                                                                             \
  16384u /* 8 MiB directory extent (~100k entries):                                                                    \
                                    bounds the untrusted on-disk dir_sectors                                           \
                                    so the extent byte-size can't overflow */
#define FS_MAX_CLUSTERS                                                                                                \
  262144 /* allocation-bitmap size: a fixed 32 KiB.                                                                    \
                                    The cluster size scales with the disk                                              \
                                    (sectors/cluster = ceil(disk capacity /                                            \
                                    FS_MAX_CLUSTERS)), so a disk of ANY size                                           \
                                    is usable from this bounded bitmap.    */

#define FS_DIR_FLAG 0x01u /* dirent.flags: entry is a directory    */
#define FS_ROOT 0xFFFFu   /* dirent.parent value meaning "in root" */
#define FS_SLOT_ROOT (-2) /* find_path() result for "/" itself     */

/* The directory entries form a tree: each names a single path component and
 * points at its containing directory by slot index (FS_ROOT for the top
 * level). The root directory is implicit -- it has no dirent of its own. */
struct dirent {
  char name[FS_NAME_LEN]; /* 64: up to 63 chars + NUL                */
  uint8_t flags;          /* FS_DIR_FLAG if this entry is a directory*/
  uint8_t reserved;       /* (was alignment pad; keeps the struct 80B)*/
  uint16_t parent;        /* containing dir slot, or FS_ROOT         */
  uint32_t size;          /* 0 for directories                       */
  uint32_t start;         /* first data sector (0 for directories)   */
  uint32_t mtime;         /* packed wall-clock time (see pack_now)   */
}; /* 80 bytes                            */

/* The directory is no longer a fixed region: it is a growable extent allocated
 * from the disk like a file. The superblock records where it lives and how big
 * it is; when it fills, free_slot() relocates it to a larger extent. So the
 * file count is bounded by disk space (and the heap that mirrors the
 * directory), not by a compiled-in constant. */
struct super {
  uint32_t magic;
  uint32_t dir_start;   /* first sector of the directory extent    */
  uint32_t dir_sectors; /* sectors in the directory extent         */
  uint8_t pad[SECTOR_SIZE - 12];
};

static struct super sb;
static struct dirent *dir; /* heap-allocated mirror of the directory  */
static uint32_t dir_cap;   /* entries the current extent/mirror hold  */
static int mounted;

/* ---- data-sector allocation (in-RAM bitmap, rebuilt from the directory) -----
 *
 * owofs files are contiguous -- each is a {start, size} extent -- so allocation
 * hands out a contiguous run of free space and freeing returns a file's run to
 * the pool; that is how deleting or overwriting a file reclaims its space (the
 * old allocator was a one-way bump pointer that never reused a freed sector).
 *
 * To make a disk of ANY size usable from a fixed, bounded amount of RAM, the
 * bitmap tracks *clusters*, not single sectors. The cluster size is chosen at
 * mount so the whole disk fits in FS_MAX_CLUSTERS bits (a 32 KiB bitmap): a
 * 64 MiB disk gets 1-sector clusters (512-byte granularity), a 256 GiB disk
 * gets ~1 MiB clusters, and so on -- the bitmap stays 32 KiB either way. Files
 * are still addressed in sectors; allocation just rounds up to whole clusters
 * and returns a cluster-aligned start, so no two files ever share a cluster
 * (which keeps freeing a file safe). The on-disk directory is the only
 * persistent truth: this bitmap is derived from it at mount, never stored. */
static uint8_t clus_used[FS_MAX_CLUSTERS / 8]; /* 1 bit per allocation cluster  */
static uint32_t fs_total_sectors;              /* full capacity of the disk     */
static uint32_t clus_sectors;                  /* sectors per cluster (>= 1)    */
static uint32_t clus_count;                    /* whole clusters on this disk   */

static int clus_test(uint32_t c) { return (clus_used[c >> 3] >> (c & 7)) & 1; }
static void clus_set(uint32_t c) { clus_used[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static void clus_clr(uint32_t c) { clus_used[c >> 3] &= (uint8_t)~(1u << (c & 7)); }

/* Sectors a file of `size` bytes occupies (always >= 1, matching fs_create). */
static uint32_t size_to_sectors(uint32_t size)
{
  uint32_t n = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  return n ? n : 1;
}

/* Mark every cluster touched by sector range [start, start+nsec) used (used=1)
 * or free (used=0). Callers pass cluster-aligned extents (a whole file, or a
 * tail rounded up to a cluster boundary), so freeing never clears a cluster
 * another file is still using. */
static void run_mark(uint32_t start, uint32_t nsec, int used)
{
  if (nsec == 0)
    return;
  uint32_t c0 = start / clus_sectors;
  uint32_t c1 = (start + nsec - 1) / clus_sectors;
  for (uint32_t c = c0; c <= c1 && c < clus_count; c++) {
    if (used)
      clus_set(c);
    else
      clus_clr(c);
  }
}

/* Free a file's extent. Guards the reserved superblock sector and the start==0
 * sentinel an empty file carries, so neither is ever cleared. */
static void free_extent(uint32_t start, uint32_t size)
{
  if (start >= (uint32_t)RESERVED_SECTORS)
    run_mark(start, size_to_sectors(size), 0);
}

/* Directory entries that fit in `sectors` sectors (entries are packed; the few
 * slack bytes at the end of the extent are unused). */
static uint32_t entries_for(uint32_t sectors) { return sectors * SECTOR_SIZE / (uint32_t)sizeof(struct dirent); }

/* Pick the cluster size so the whole backing disk fits in FS_MAX_CLUSTERS bits,
 * then count the whole clusters (a partial final cluster is left unused so
 * every allocation stays within the disk). No size cap -- a disk of any size is
 * usable; the bitmap just gets coarser on a larger disk. */
static void set_capacity(void)
{
  fs_total_sectors = blk_sectors();
  if (fs_total_sectors == 0)
    fs_total_sectors = 1;
  clus_sectors = (fs_total_sectors + FS_MAX_CLUSTERS - 1) / FS_MAX_CLUSTERS;
  if (clus_sectors == 0)
    clus_sectors = 1;
  clus_count = fs_total_sectors / clus_sectors; /* whole clusters only */
}

/* Clear the bitmap and reserve only the superblock sector. */
static void reset_alloc_map(void)
{
  for (uint32_t i = 0; i < sizeof clus_used; i++)
    clus_used[i] = 0;
  run_mark(0, (uint32_t)RESERVED_SECTORS, 1); /* the superblock */
}

/* Rebuild the allocation bitmap from the on-disk directory: reserve the
 * superblock and the directory extent, then mark every live file's extent. */
static void rebuild_alloc_map(void)
{
  reset_alloc_map();
  run_mark(sb.dir_start, sb.dir_sectors, 1); /* the directory extent itself */
  for (uint32_t i = 0; i < dir_cap; i++) {
    if (!dir[i].name[0] || (dir[i].flags & FS_DIR_FLAG) || dir[i].start == 0)
      continue; /* dirs + empty files own no data sectors */
    run_mark(dir[i].start, size_to_sectors(dir[i].size), 1);
  }
}

/* First-fit search for enough contiguous free clusters to hold `nsec` sectors;
 * marks them used and returns the cluster-aligned start LBA, or 0 (an invalid
 * data LBA -- allocation starts past the reserved superblock) when no run is
 * large enough -- the "disk full" signal. */
static uint32_t alloc_run(uint32_t nsec)
{
  if (nsec == 0)
    nsec = 1;
  uint32_t need = (nsec + clus_sectors - 1) / clus_sectors;
  if (need == 0)
    need = 1;
  uint32_t first = ((uint32_t)RESERVED_SECTORS + clus_sectors - 1) / clus_sectors;
  uint32_t run = 0, start_c = 0;
  for (uint32_t c = first; c < clus_count; c++) {
    if (clus_test(c)) {
      run = 0;
      continue;
    }
    if (run == 0)
      start_c = c;
    if (++run == need) {
      for (uint32_t i = 0; i < need; i++)
        clus_set(start_c + i);
      return start_c * clus_sectors;
    }
  }
  return 0;
}

/* The largest contiguous free run, for a streaming write whose final size is
 * unknown: returns its length in sectors and stores its cluster-aligned start
 * sector in *start. */
static uint32_t largest_free_run(uint32_t *start)
{
  uint32_t first = ((uint32_t)RESERVED_SECTORS + clus_sectors - 1) / clus_sectors;
  uint32_t best = 0, best_c = 0, run = 0, rc = 0;
  for (uint32_t c = first; c < clus_count; c++) {
    if (clus_test(c)) {
      run = 0;
      continue;
    }
    if (run == 0)
      rc = c;
    if (++run > best) {
      best = run;
      best_c = rc;
    }
  }
  *start = best_c * clus_sectors;
  return best * clus_sectors;
}

/* Load the directory extent (sb.dir_start, sb.dir_sectors) into a fresh heap
 * mirror. The mirror is sized to the whole extent so a full-sector read can't
 * overrun it. Returns 0, or -1 if the allocation failed. */
static int dir_load(void)
{
  /* sb.dir_sectors is untrusted (from the on-disk superblock); a crafted value
   * would overflow the byte computation and undersize the mirror while the read
   * loop writes a full extent. Reject anything that overflows, exceeds the
   * design ceiling, or doesn't fit the backing device. */
  if (sb.dir_sectors == 0 || sb.dir_sectors > FS_MAX_DIR_SECTORS || sb.dir_start >= fs_total_sectors ||
      sb.dir_sectors > fs_total_sectors - sb.dir_start)
    return -1;
  uint32_t bytes = sb.dir_sectors * SECTOR_SIZE;
  dir = kmalloc(bytes);
  if (!dir)
    return -1;
  dir_cap = entries_for(sb.dir_sectors);
  for (uint32_t s = 0; s < sb.dir_sectors; s++)
    blk_read_sector(sb.dir_start + s, (uint8_t *)dir + s * SECTOR_SIZE);
  return 0;
}

/* Write the whole directory extent back to disk. */
static int dir_store(void)
{
  for (uint32_t s = 0; s < sb.dir_sectors; s++)
    if (blk_write_sector(sb.dir_start + s, (uint8_t *)dir + s * SECTOR_SIZE) != 0)
      return -1;
  return 0;
}

/* Grow the directory by relocating it to a larger contiguous extent: allocate
 * double the sectors, copy the entries across, persist the new location in the
 * superblock, then free the old extent. Returns 0, or -1 if the disk or heap is
 * exhausted (the only thing now bounding the file count). */
static int grow_dir(void)
{
  uint32_t new_sectors = sb.dir_sectors ? sb.dir_sectors * 2 : 1;
  uint32_t new_start = alloc_run(new_sectors);
  if (new_start == 0)
    return -1; /* disk full */
  uint32_t new_bytes = new_sectors * SECTOR_SIZE;
  struct dirent *nd = kmalloc(new_bytes);
  if (!nd) {
    run_mark(new_start, new_sectors, 0);
    return -1;
  } /* heap full */

  uint8_t *d = (uint8_t *)nd, *s = (uint8_t *)dir;
  uint32_t old_bytes = dir_cap * (uint32_t)sizeof(struct dirent);
  for (uint32_t i = 0; i < new_bytes; i++)
    d[i] = (i < old_bytes) ? s[i] : 0;

  for (uint32_t k = 0; k < new_sectors; k++)
    if (blk_write_sector(new_start + k, d + k * SECTOR_SIZE) != 0) {
      kfree(nd);
      run_mark(new_start, new_sectors, 0);
      return -1;
    }

  uint32_t old_start = sb.dir_start, old_sectors = sb.dir_sectors;
  kfree(dir);
  dir = nd;
  sb.dir_start = new_start;
  sb.dir_sectors = new_sectors;
  dir_cap = entries_for(new_sectors);
  if (blk_write_sector(SUPER_LBA, &sb) != 0)
    return -1;                                       /* new location persisted */
  free_extent(old_start, old_sectors * SECTOR_SIZE); /* reclaim old extent */
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
 * unpack): [year-2000:6][month:4][day:5][hour:5][min:6][sec:6]. Good to 2063.
 */
static uint32_t pack_now(void)
{
  struct rtc_time t;
  rtc_now(&t);
  uint32_t y = (t.year >= 2000) ? (uint32_t)(t.year - 2000) : 0;
  return ((y & 0x3F) << 26) | (((uint32_t)t.month & 0xF) << 22) | (((uint32_t)t.day & 0x1F) << 17) |
         (((uint32_t)t.hour & 0x1F) << 12) | (((uint32_t)t.min & 0x3F) << 6) | ((uint32_t)t.sec & 0x3F);
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

/* The first unused directory slot, growing the directory if every slot is
 * taken. Returns -1 only when the disk or heap can't hold a bigger directory.
 */
static int free_slot(void)
{
  for (uint32_t i = 0; i < dir_cap; i++)
    if (!dir[i].name[0])
      return (int)i;
  if (grow_dir() != 0)
    return -1;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (!dir[i].name[0])
      return (int)i;
  return -1;
}

/* The slot of the entry named `leaf` directly inside directory `parent`
 * (FS_ROOT for the top level), or -1 if there is none. */
static int find_in(int parent, const char *leaf)
{
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && dir[i].parent == (uint16_t)parent && name_eq(dir[i].name, leaf))
      return (int)i;
  return -1;
}

/* Walk `path` to its containing directory: resolve every component but the
 * last, which is returned in leaf[] (>= FS_NAME_LEN bytes). *parent receives
 * the slot of the directory that should hold leaf (FS_ROOT for the top level).
 * Leading/duplicated slashes and "."/".." are honored; a trailing slash (or a
 * path naming a directory, e.g. "/" or "..") yields an empty leaf. Returns 0,
 * or -1 if a component is too long or an intermediate name isn't a directory.
 */
static int resolve_path(const char *path, int *parent, char *leaf)
{
  int cur = FS_ROOT;
  const char *p = path;
  char comp[FS_NAME_LEN];

  for (;;) {
    while (*p == '/')
      p++;
    if (*p == '\0') { /* nothing left -> name a directory */
      leaf[0] = '\0';
      *parent = cur;
      return 0;
    }
    int n = 0;
    while (p[n] && p[n] != '/')
      n++;
    if (n >= FS_NAME_LEN)
      return -1; /* component too long               */
    for (int i = 0; i < n; i++)
      comp[i] = p[i];
    comp[n] = '\0';
    p += n;
    while (*p == '/')
      p++;
    int last = (*p == '\0');

    if (s_eq(comp, ".")) {
      if (last) {
        leaf[0] = '\0';
        *parent = cur;
        return 0;
      }
      continue;
    }
    if (s_eq(comp, "..")) {
      if (cur != FS_ROOT)
        cur = (dir[cur].parent == FS_ROOT || dir[cur].parent < dir_cap) ? dir[cur].parent : FS_ROOT;
      if (last) {
        leaf[0] = '\0';
        *parent = cur;
        return 0;
      }
      continue;
    }
    if (last) {
      name_copy(leaf, comp);
      *parent = cur;
      return 0;
    }
    int s = find_in(cur, comp); /* descend into a subdirectory      */
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

/* ------------------------------------------------- pseudo-filesystem mounts --
 *
 * /dev, /proc and /sys are not special-cased inside each fs op: each is a real
 * entry in the mount table backed by a struct fs_ops, exactly like a removable
 * /media volume. mount_for() routes a path to the owning pseudo-fs and every fs
 * operation dispatches through the same rvol_* path, so they list, stat, read
 * and enumerate as ordinary folders and files -- in the Files app and the shell
 * alike. The ops are stateless singletons (inst == NULL): the content is
 * generated on demand from live kernel state by blk.c / procfs.c / sysfs.c. */

/* These reuse the listing/format helpers and str_copy defined further down. */
static void write_mtime(uint32_t m);
static void write_pad(uint32_t v, int width);
static void str_copy(char *dst, const char *src, int cap);

/* "" or "/" -- the mount point itself, i.e. the pseudo-fs root directory. */
static int sub_is_root(const char *p) { return !p[0] || (p[0] == '/' && !p[1]); }

/* ---- /dev: the present block devices, each a regular (non-byte-readable) file
 */
static const char *devfs_label(void *i)
{
  (void)i;
  return "";
}
static const char *devfs_type(void *i)
{
  (void)i;
  return "devfs";
}

static int devfs_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  (void)inst;
  if (sub_is_root(path)) { /* /dev itself is a directory       */
    if (is_dir)
      *is_dir = 1;
    if (size)
      *size = 0;
    return 0;
  }
  const char *nm = (path[0] == '/') ? path + 1 : path;
  struct blk_devinfo devs[16];
  int nd = blk_dev_list(devs, 16);
  for (int i = 0; i < nd; i++)
    if (s_eq(devs[i].dev, nm)) { /* a named device is a regular file */
      if (is_dir)
        *is_dir = 0;
      if (size)
        *size = 0;
      return 0;
    }
  return -1;
}

static int devfs_enum(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  (void)inst;
  if (!sub_is_root(path)) /* /dev is flat                     */
    return 0;
  struct blk_devinfo devs[16];
  int nd = blk_dev_list(devs, 16);
  if (idx < 0 || idx >= nd)
    return 0;
  str_copy(name, devs[idx].dev, FS_NAME_LEN);
  if (is_dir)
    *is_dir = 0;
  if (size)
    *size = 0;
  return 1;
}

static int devfs_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{ /* block-device nodes aren't byte-readable through
     the file API */
  (void)inst;
  (void)path;
  (void)off;
  (void)buf;
  (void)len;
  return -1;
}

static int devfs_list(void *inst, const char *path)
{
  (void)inst;
  if (!sub_is_root(path))
    return -1; /* ls of a device name -> no such   */
  struct blk_devinfo devs[16];
  int nd = blk_dev_list(devs, 16);
  for (int i = 0; i < nd; i++) {
    write_mtime(0);
    con_write("  ");
    write_pad(0, 8);
    con_write("  ");
    con_write(devs[i].dev);
    con_putc('\n');
  }
  if (!nd)
    con_write("(empty)\n");
  return 0;
}

static const struct fs_ops devfs_ops = {
    .fsname = "devfs",
    .probe = NULL,
    .close = NULL,
    .label = devfs_label,
    .type = devfs_type,
    .list = devfs_list,
    .stat = devfs_stat,
    .read_at = devfs_read_at,
    .enum_dir = devfs_enum,
    .create = NULL,
    .del = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
};

/* ---- a generated pseudo-file reader supporting an arbitrary offset ----------
 * procfs/sysfs files are produced whole, from the start. A plain whole-file
 * read (offset 0) generates straight into the caller's buffer; an offset read
 * regenerates once into a scratch buffer and slices, so /proc and /sys files
 * stream in bounded chunks like any other file. */
typedef int (*gen_read_fn)(const char *sub, char *buf, int max);
static int gen_read_at(gen_read_fn gen, const char *sub, uint32_t off, void *buf, uint32_t len)
{
  if (off == 0)
    return gen(sub, (char *)buf, (int)len);
  static char scratch[8192];
  int total = gen(sub, scratch, (int)sizeof scratch);
  if (total < 0)
    return -1;
  if (off >= (uint32_t)total)
    return 0;
  uint32_t avail = (uint32_t)total - off;
  uint32_t want = avail < len ? avail : len;
  for (uint32_t i = 0; i < want; i++)
    ((uint8_t *)buf)[i] = (uint8_t)scratch[off + i];
  return (int)want;
}

/* ---- /proc: flat, one generated file per fact (see procfs.c) ---------------
 */
static const char *procfs_label(void *i)
{
  (void)i;
  return "";
}
static const char *procfs_type(void *i)
{
  (void)i;
  return "proc";
}

static int procfs_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  (void)inst;
  int k = procfs_kind(path); /* 1 dir / 0 file / -1 unknown      */
  if (k < 0)
    return -1;
  if (is_dir)
    *is_dir = (k == 1);
  if (size)
    *size = (k == 0) ? (uint32_t)procfs_size(path) : 0;
  return 0;
}

static int procfs_enum(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  (void)inst;
  if (!sub_is_root(path)) /* /proc is flat: only the root     */
    return 0;
  if (idx < 0 || !procfs_list(idx, name, FS_NAME_LEN))
    return 0;
  if (is_dir)
    *is_dir = 0;
  if (size)
    *size = (uint32_t)procfs_size(name);
  return 1;
}

static int procfs_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  (void)inst;
  if (procfs_kind(path) != 0) /* only files are readable          */
    return -1;
  return gen_read_at(procfs_read, path, off, buf, len);
}

static int procfs_list_op(void *inst, const char *path)
{
  (void)inst;
  int k = procfs_kind(path);
  if (k < 0)
    return -1;
  if (k == 0) { /* names a single file              */
    const char *nf = (path[0] == '/') ? path + 1 : path;
    write_mtime(0);
    con_write("  ");
    write_pad((uint32_t)procfs_size(nf), 8);
    con_write("  ");
    con_write(nf);
    con_putc('\n');
    return 0;
  }
  char nm[24];
  for (int i = 0; procfs_list(i, nm, sizeof nm); i++) {
    write_mtime(0);
    con_write("  ");
    write_pad((uint32_t)procfs_size(nm), 8);
    con_write("  ");
    con_write(nm);
    con_putc('\n');
  }
  return 0;
}

static const struct fs_ops procfs_ops = {
    .fsname = "proc",
    .probe = NULL,
    .close = NULL,
    .label = procfs_label,
    .type = procfs_type,
    .list = procfs_list_op,
    .stat = procfs_stat,
    .read_at = procfs_read_at,
    .enum_dir = procfs_enum,
    .create = NULL,
    .del = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
};

/* ---- /sys: a nested tree of device tunables (see sysfs.c) ------------------
 */
static const char *sysfs_label(void *i)
{
  (void)i;
  return "";
}
static const char *sysfs_type(void *i)
{
  (void)i;
  return "sysfs";
}

/* Join a /sys directory sub path and a child name into "dir/child". */
static void sysfs_child(char *out, int cap, const char *dirsub, const char *child)
{
  int j = 0;
  for (const char *q = dirsub; *q && j < cap - 2; q++)
    out[j++] = *q;
  out[j++] = '/';
  for (int i = 0; child[i] && j < cap - 1; i++)
    out[j++] = child[i];
  out[j] = '\0';
}

static int sysfs_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  (void)inst;
  int k = sysfs_kind(path); /* 1 dir / 0 file / -1 unknown      */
  if (k < 0)
    return -1;
  if (is_dir)
    *is_dir = (k == 1);
  if (size)
    *size = (k == 0) ? (uint32_t)sysfs_size(path) : 0;
  return 0;
}

static int sysfs_enum_op(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  (void)inst;
  int d = 0;
  if (idx < 0 || !sysfs_enum(path, idx, name, FS_NAME_LEN, &d))
    return 0;
  if (is_dir)
    *is_dir = d;
  if (size) {
    if (d)
      *size = 0;
    else { /* file child: its full sub path    */
      char child[160];
      sysfs_child(child, (int)sizeof child, path, name);
      *size = (uint32_t)sysfs_size(child);
    }
  }
  return 1;
}

static int sysfs_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  (void)inst;
  if (sysfs_kind(path) != 0)
    return -1;
  return gen_read_at(sysfs_read, path, off, buf, len);
}

static int sysfs_list_op(void *inst, const char *path)
{
  (void)inst;
  int k = sysfs_kind(path);
  if (k < 0)
    return -1;
  char nm[32];
  int isd;
  for (int i = 0; sysfs_enum(path, i, nm, sizeof nm, &isd); i++) {
    char child[160];
    sysfs_child(child, (int)sizeof child, path, nm);
    write_mtime(0);
    con_write("  ");
    write_pad(isd ? 0 : (uint32_t)sysfs_size(child), 8);
    con_write("  ");
    con_write(nm);
    if (isd)
      con_putc('/');
    con_putc('\n');
  }
  return 0;
}

static const struct fs_ops sysfs_ops = {
    .fsname = "sysfs",
    .probe = NULL,
    .close = NULL,
    .label = sysfs_label,
    .type = sysfs_type,
    .list = sysfs_list_op,
    .stat = sysfs_stat,
    .read_at = sysfs_read_at,
    .enum_dir = sysfs_enum_op,
    .create = NULL,
    .del = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
};

/* Graft a permanent pseudo-filesystem into the mount table at `point`. */
static void mount_pseudo(const char *point, const char *dev, const struct fs_ops *ops)
{
  for (int i = 0; i < MAX_MOUNTS; i++)
    if (mtab[i].used && s_eq(mtab[i].point, point))
      return; /* already mounted                  */
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used)
      continue;
    mtab[i].used = 1;
    mtab[i].permanent = 1;
    str_copy(mtab[i].point, point, (int)sizeof mtab[i].point);
    str_copy(mtab[i].dev, dev, (int)sizeof mtab[i].dev);
    mtab[i].ops = ops;
    mtab[i].inst = NULL; /* stateless singleton              */
    return;
  }
}

/* Mount the three permanent pseudo-filesystems. Independent of owofs, so they
 * are present even on a no-disk / tmpfs boot. */
static void fs_mount_pseudo(void)
{
  mount_pseudo("/dev", "dev", &devfs_ops);
  mount_pseudo("/proc", "proc", &procfs_ops);
  mount_pseudo("/sys", "sys", &sysfs_ops);
}

void fs_format(void)
{
  set_capacity();
  reset_alloc_map(); /* clear bitmap; reserve the superblock */

  /* Allocate the initial directory extent from the freshly-cleared pool. */
  uint32_t init_sectors = (DIR_INIT_ENTRIES * (uint32_t)sizeof(struct dirent) + SECTOR_SIZE - 1) / SECTOR_SIZE;
  uint32_t start = alloc_run(init_sectors);
  if (start == 0) { /* disk too small for a directory */
    mounted = 0;
    return;
  }
  sb.magic = FS_MAGIC;
  sb.dir_start = start;
  sb.dir_sectors = init_sectors;
  for (unsigned i = 0; i < sizeof(sb.pad); i++)
    sb.pad[i] = 0;

  uint32_t bytes = init_sectors * SECTOR_SIZE;
  if (dir)
    kfree(dir);
  dir = kmalloc(bytes);
  if (!dir) {
    mounted = 0;
    return;
  }
  for (uint32_t i = 0; i < bytes; i++)
    ((uint8_t *)dir)[i] = 0; /* all slots free (name[0] == 0) */
  dir_cap = entries_for(init_sectors);

  blk_write_sector(SUPER_LBA, &sb);
  dir_store();
  mounted = 1;
}

void fs_init(void)
{
  fs_mount_pseudo(); /* /dev, /proc, /sys -- always present, owofs-independent
                      */

  int have = (blk_read_sector(SUPER_LBA, &sb) == 0);

  if (have && sb.magic == FS_MAGIC) { /* a real owofs disk -- mount it  */
    set_capacity();
    if (dir_load() != 0) { /* out of heap -- can't mount      */
      mounted = 0;
      return;
    }
    rebuild_alloc_map(); /* reconstruct free space from the dir */
    mounted = 1;
    return;
  }
  if (have && blk_is_ide()) { /* blank IDE dev disk -- make owofs*/
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
    fs_format(); /* formats + mounts the RAM disk   */
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
  if (m == 0) { /* never stamped                    */
    con_write("       -          ");
    return;
  }
  write_pad(2000 + (m >> 26), 4);
  con_putc('-');
  write_pad((m >> 22) & 0xF, 2);
  con_putc('-');
  write_pad((m >> 17) & 0x1F, 2);
  con_putc(' ');
  write_pad((m >> 12) & 0x1F, 2);
  con_putc(':');
  write_pad((m >> 6) & 0x3F, 2);
  con_putc(':');
  write_pad(m & 0x3F, 2);
}

/* Print one directory row: mtime, size (or "<DIR>"), and the name. */
static void list_row(int i)
{
  write_mtime(dir[i].mtime);
  con_write("  ");
  if (dir[i].flags & FS_DIR_FLAG)
    con_write("   <DIR>"); /* same width as the size column     */
  else
    write_pad(dir[i].size, 8); /* right-aligned size column         */
  con_write("  ");
  con_write(dir[i].name);
  if (dir[i].flags & FS_DIR_FLAG)
    con_putc('/');
  con_putc('\n');
}

void fs_list(const char *path)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) { /* /dev /proc /sys, or a /media volume */
    if (rvol_list(mv, sub) < 0)
      con_write("ls: no such file or directory\n");
    return;
  }
  /* /media -- synthetic directory of the active removable mounts.           */
  if (is_media_root(path)) {
    int n = 0;
    for (int i = 0; i < MAX_MOUNTS; i++)
      if (mtab[i].used && !mtab[i].permanent) {
        write_mtime(0);
        con_write("  ");
        con_write("   <DIR>");
        con_write("  ");
        con_write(mnt_leaf(&mtab[i]));
        con_putc('/');
        con_putc('\n');
        n++;
      }
    if (!n)
      con_write("(empty)\n");
    return;
  }
  if (!mounted) {
    con_write("fs: not mounted\n");
    return;
  }
  int d = (path && path[0]) ? find_path(path) : FS_SLOT_ROOT;
  if (d == -1) {
    con_write("ls: no such file or directory\n");
    return;
  }
  if (d != FS_SLOT_ROOT && !(dir[d].flags & FS_DIR_FLAG)) {
    list_row(d); /* path names a file -> list just it */
    return;
  }
  int parent = (d == FS_SLOT_ROOT) ? (int)FS_ROOT : d;
  int n = 0;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && dir[i].parent == (uint16_t)parent) {
      list_row(i);
      n++;
    }
  if (d == FS_SLOT_ROOT) { /* /media + the permanent pseudo-fs mounts */
    con_write("       -            <DIR>  media/\n");
    n++;
    for (int i = 0; i < MAX_MOUNTS; i++)
      if (mtab[i].used && mtab[i].permanent) {
        con_write("       -            <DIR>  ");
        con_write(mnt_leaf(&mtab[i]));
        con_write("/\n");
        n++;
      }
  }
  if (!n)
    con_write("(empty)\n");
}

int fs_read(const char *name, void *buf, uint32_t maxlen)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) { /* /proc /sys file, /dev (-> -1), or a volume */
    int r = rvol_read_at(mv, sub, 0, buf, maxlen);
    return r < 0 ? -1 : r;
  }
  if (!mounted)
    return -1;
  int i = find_file(name);
  if (i < 0)
    return -1;

  uint32_t want = dir[i].size < maxlen ? dir[i].size : maxlen;
  uint32_t sectors = (want + SECTOR_SIZE - 1) / SECTOR_SIZE;
  uint8_t tmp[SECTOR_SIZE];
  uint32_t copied = 0;

  for (uint32_t s = 0; s < sectors; s++) {
    if (blk_read_sector(dir[i].start + s, tmp) != 0)
      return -1;
    uint32_t chunk = (want - copied) < SECTOR_SIZE ? (want - copied) : SECTOR_SIZE;
    for (uint32_t k = 0; k < chunk; k++)
      ((uint8_t *)buf)[copied + k] = tmp[k];
    copied += chunk;
  }
  return (int)want;
}

/* Size of a file in bytes, or -1 if it does not exist (or names a directory).
 */
int fs_size(const char *name)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    int is_dir;
    uint32_t sz;
    if (rvol_stat(mv, sub, &is_dir, &sz) != 0 || is_dir)
      return -1;
    return (int)sz;
  }
  if (!mounted)
    return -1;
  int i = find_file(name);
  return (i < 0) ? -1 : (int)dir[i].size;
}

/* Read up to len bytes starting at byte offset within a file -- lets a large
 * file be streamed in bounded chunks (owofs stores files contiguously, so this
 * is just sector math). Returns bytes read (0 at/after EOF), or -1 if absent.
 */
int fs_read_at(const char *name, uint32_t offset, void *buf, uint32_t len)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) { /* /proc /sys (offset slice), /dev (-> -1), or a volume */
    int r = rvol_read_at(mv, sub, offset, buf, len);
    return r < 0 ? -1 : r;
  }
  if (!mounted)
    return -1;
  int i = find_file(name);
  if (i < 0)
    return -1;
  if (offset >= dir[i].size)
    return 0;
  uint32_t avail = dir[i].size - offset;
  uint32_t want = avail < len ? avail : len;
  uint8_t tmp[SECTOR_SIZE];
  uint32_t copied = 0;

  while (copied < want) {
    uint32_t pos = offset + copied;
    uint32_t sec = dir[i].start + pos / SECTOR_SIZE;
    uint32_t soff = pos % SECTOR_SIZE;
    uint32_t left = want - copied;

    /* Whole sectors landing on a sector boundary go straight into the
     * caller's buffer in one batched read — no per-sector bounce through
     * tmp. A streaming reader (4 KiB-aligned, e.g. imgview) hits this for
     * all but a possible tail, turning a megabyte into a handful of
     * READ SECTORS commands. */
    if (soff == 0 && left >= SECTOR_SIZE) {
      uint32_t nsec = left / SECTOR_SIZE;
      if (blk_read(sec, (uint8_t *)buf + copied, nsec) != 0)
        return (int)copied; /* partial read on I/O error */
      copied += nsec * SECTOR_SIZE;
      continue;
    }

    /* Partial leading/trailing sector: read one and copy the slice. */
    if (blk_read_sector(sec, tmp) != 0)
      return (int)copied; /* partial read on I/O error */
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
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1; /* read-only mount */
    return mv->ops->create(mv->inst, sub, data, size) == 0 ? 0 : -1;
  }

  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1; /* bad path / names a dir   */

  int slot = find_in(parent, leaf); /* overwrite if it exists   */
  int overwrite = 0;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG)
      return -1; /* can't clobber a directory*/
    overwrite = 1;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0)
      return -1; /* directory full           */
  }

  /* Allocate the new extent before freeing any old one, so a failed write
   * leaves the existing file intact (the old extent is reclaimed only after
   * the new data is safely committed below). */
  uint32_t sectors = size_to_sectors(size);
  uint32_t start = alloc_run(sectors);
  if (start == 0)
    return -1; /* disk full                */

  uint8_t tmp[SECTOR_SIZE];
  uint32_t written = 0;
  for (uint32_t s = 0; s < sectors; s++) {
    for (int k = 0; k < SECTOR_SIZE; k++)
      tmp[k] = 0;
    uint32_t chunk = (size - written) < SECTOR_SIZE ? (size - written) : SECTOR_SIZE;
    for (uint32_t k = 0; k < chunk; k++)
      tmp[k] = ((const uint8_t *)data)[written + k];
    if (blk_write_sector(start + s, tmp) != 0) {
      run_mark(start, sectors, 0); /* roll back the reservation */
      return -1;
    }
    written += chunk;
  }

  name_copy(dir[slot].name, leaf);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = size;
  dir[slot].start = start;
  dir[slot].mtime = pack_now();

  if (overwrite)
    free_extent(old_start, old_size); /* reclaim the old extent   */
  if (dir_store() != 0)
    return -1;
  return 0;
}

/* Wipe a directory slot. The caller is responsible for reclaiming the entry's
 * data extent first (free_extent) -- slot_clear only clears the dirent. */
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
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1; /* read-only mount */
    return mv->ops->del(mv->inst, sub) == 0 ? 0 : -1;
  }
  int i = find_path(name);
  if (i < 0 || i == FS_SLOT_ROOT)
    return -1; /* no such file                    */
  if (dir[i].flags & FS_DIR_FLAG)
    return -2;                            /* it's a directory -> fs_rmdir    */
  free_extent(dir[i].start, dir[i].size); /* reclaim its data sectors        */
  slot_clear(i);
  return (dir_store() == 0) ? 0 : -1;
}

int fs_rename(const char *old, const char *neu)
{
  if (!mounted)
    return -1;

  int src = find_path(old);
  if (src < 0 || src == FS_SLOT_ROOT)
    return -1; /* no such source                  */

  int dparent;
  char dleaf[FS_NAME_LEN];
  if (resolve_path(neu, &dparent, dleaf) != 0 || dleaf[0] == '\0')
    return -1; /* bad/empty target                */

  if (dir[src].flags & FS_DIR_FLAG) { /* don't move a dir into itself    */
    int c = dparent;
    while (c != (int)FS_ROOT) {
      if (c == src)
        return -1;
      c = dir[c].parent;
    }
  }

  int ex = find_in(dparent, dleaf); /* overwrite an existing target    */
  if (ex >= 0 && ex != src) {
    if (dir[ex].flags & FS_DIR_FLAG)
      return -1;                              /* refuse to clobber a directory   */
    free_extent(dir[ex].start, dir[ex].size); /* reclaim the clobbered file */
    slot_clear(ex);
  }

  name_copy(dir[src].name, dleaf);
  dir[src].parent = (uint16_t)dparent;
  dir[src].mtime = pack_now();
  return (dir_store() == 0) ? 0 : -1;
}

int fs_mkdir(const char *path)
{
  if (!mounted)
    return -1;
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1; /* read-only mount */
    return mv->ops->mkdir(mv->inst, sub) == 0 ? 0 : -1;
  }
  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(path, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1; /* bad path / "/" itself           */
  if (find_in(parent, leaf) >= 0)
    return -1; /* name already taken              */
  int slot = free_slot();
  if (slot < 0)
    return -1; /* directory full                  */
  name_copy(dir[slot].name, leaf);
  dir[slot].flags = FS_DIR_FLAG;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = 0;
  dir[slot].start = 0;
  dir[slot].mtime = pack_now();
  return (dir_store() == 0) ? 0 : -1;
}

int fs_rmdir(const char *path)
{
  if (!mounted)
    return -1;
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1; /* read-only mount */
    return mv->ops->rmdir(mv->inst, sub) == 0 ? 0 : -1;
  }
  int s = find_path(path);
  if (s < 0 || s == FS_SLOT_ROOT)
    return -1; /* not found / can't remove root   */
  if (!(dir[s].flags & FS_DIR_FLAG))
    return -2;                           /* not a directory                 */
  for (uint32_t i = 0; i < dir_cap; i++) /* must be empty                   */
    if (dir[i].name[0] && dir[i].parent == (uint16_t)s)
      return -3;
  slot_clear(s);
  return (dir_store() == 0) ? 0 : -1;
}

int fs_isdir(const char *path)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) { /* /dev /proc /sys, or a /media volume */
    int is_dir;
    if (rvol_stat(mv, sub, &is_dir, NULL) != 0)
      return -1;
    return is_dir ? 1 : 0;
  }
  if (is_media_root(path))
    return 1;
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
  { /* prealloc/write_at have no fs_ops counterpart -- not on a mount */
    const char *sub;
    if (mount_for(name, &sub))
      return -1;
  }

  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1;

  int slot = find_in(parent, leaf);
  int overwrite = 0;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG)
      return -1; /* can't clobber a directory       */
    overwrite = 1;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0)
      return -1; /* directory full           */
  }

  uint32_t sectors = size_to_sectors(size);
  uint32_t start = alloc_run(sectors);
  if (start == 0)
    return -1; /* disk full                */

  uint8_t zero[SECTOR_SIZE];
  for (int k = 0; k < SECTOR_SIZE; k++)
    zero[k] = 0;
  for (uint32_t s = 0; s < sectors; s++)
    if (blk_write_sector(start + s, zero) != 0) {
      run_mark(start, sectors, 0); /* roll back the reservation */
      return -1;
    }

  name_copy(dir[slot].name, leaf);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = size;
  dir[slot].start = start;
  dir[slot].mtime = pack_now();

  if (overwrite)
    free_extent(old_start, old_size); /* reclaim the old extent   */
  if (dir_store() != 0)
    return -1;
  return 0;
}

int fs_write_at(const char *name, uint32_t offset, const void *data, uint32_t len)
{
  if (!mounted)
    return -1;
  { /* random-access write has no fs_ops counterpart -- not on a mount */
    const char *sub;
    if (mount_for(name, &sub))
      return -1;
  }
  int i = find_file(name);
  if (i >= 0) {
    if (offset > dir[i].size || len > dir[i].size - offset)
      return -1; /* would write past EOF (no grow) */

    const uint8_t *p = data;
    uint8_t tmp[SECTOR_SIZE];
    uint32_t written = 0;
    while (written < len) {
      uint32_t pos = offset + written;
      uint32_t sec = dir[i].start + pos / SECTOR_SIZE;
      uint32_t soff = pos % SECTOR_SIZE;
      uint32_t chunk = SECTOR_SIZE - soff;
      if (chunk > len - written)
        chunk = len - written;

      if (soff != 0 || chunk != SECTOR_SIZE) { /* partial: read-modify-write */
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
  return -1; /* no such file             */
}

/* Streaming creation, for data whose total size isn't known until the last
 * byte arrives (e.g. an HTTP download). begin -> write* -> end. Bytes are
 * staged a sector at a time so the whole file never has to live in RAM. Not
 * reentrant: one stream at a time, which is all the single-threaded shell
 * needs. Because the final size is unknown, begin reserves the *largest*
 * contiguous free run on the disk and streams into it; end releases the unused
 * tail (and reclaims an overwritten file's old extent), so a download is
 * bounded only by the biggest free gap, and a stream that dies partway frees
 * its whole reservation -- nothing is leaked. */
static int strm_active;
static char strm_name[FS_NAME_LEN];   /* final path component (leaf)          */
static int strm_parent;               /* slot of the directory to create it in */
static uint32_t strm_start;           /* first data sector (reserved at begin) */
static uint32_t strm_reserved;        /* sectors reserved for the stream       */
static uint32_t strm_sectors;         /* full sectors flushed so far           */
static uint32_t strm_size;            /* bytes accepted so far                 */
static uint8_t strm_buf[SECTOR_SIZE]; /* the sector currently filling */
static uint32_t strm_fill;            /* bytes pending in strm_buf             */

int fs_stream_begin(const char *name)
{
  if (!mounted || strm_active)
    return -1;
  { /* streaming creation has no fs_ops counterpart -- not on a mount */
    const char *sub;
    if (mount_for(name, &sub))
      return -1;
  }
  if (resolve_path(name, &strm_parent, strm_name) != 0 || strm_name[0] == '\0')
    return -1; /* bad path / names a directory     */
  strm_reserved = largest_free_run(&strm_start);
  if (strm_reserved == 0)
    return -1;                            /* disk full                        */
  run_mark(strm_start, strm_reserved, 1); /* hold the gap for the whole stream */
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
      if (strm_sectors >= strm_reserved || blk_write_sector(strm_start + strm_sectors, strm_buf) != 0) {
        run_mark(strm_start, strm_reserved, 0); /* release the gap */
        strm_active = 0;                        /* overran the reservation / I/O error */
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

  if (strm_fill > 0) { /* flush the partial final sector   */
    for (uint32_t k = strm_fill; k < SECTOR_SIZE; k++)
      strm_buf[k] = 0;
    if (strm_sectors >= strm_reserved || blk_write_sector(strm_start + strm_sectors, strm_buf) != 0) {
      run_mark(strm_start, strm_reserved, 0); /* release the gap */
      return -1;
    }
    strm_sectors++;
    strm_fill = 0;
  }

  int slot = find_in(strm_parent, strm_name); /* overwrite if it exists   */
  int overwrite = 0;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG) {
      run_mark(strm_start, strm_reserved, 0); /* release; can't clobber dir */
      return -1;
    }
    overwrite = 1;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0) {
      run_mark(strm_start, strm_reserved, 0); /* release; directory full */
      return -1;
    }
  }

  /* Hand back the unused tail of the reservation. Round the boundary up past
   * the last sector we actually used so its cluster is kept, then free the
   * whole clusters beyond it (the reservation end is already cluster-aligned).
   */
  uint32_t used_end = strm_start + strm_sectors;
  uint32_t tail = (used_end + clus_sectors - 1) / clus_sectors * clus_sectors;
  if (strm_start + strm_reserved > tail)
    run_mark(tail, (strm_start + strm_reserved) - tail, 0);

  name_copy(dir[slot].name, strm_name);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)strm_parent;
  dir[slot].size = strm_size;
  dir[slot].start = (strm_sectors > 0) ? strm_start : 0; /* empty => no extent */
  dir[slot].mtime = pack_now();

  if (overwrite)
    free_extent(old_start, old_size); /* reclaim the old extent   */
  if (dir_store() != 0)
    return -1;
  return (int)strm_size;
}

int fs_file_count(void)
{
  int n = 0;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && !(dir[i].flags & FS_DIR_FLAG))
      n++;
  return n;
}

uint32_t fs_used_bytes(void)
{
  uint32_t total = 0;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && !(dir[i].flags & FS_DIR_FLAG))
      total += dir[i].size;
  return total;
}

/* --------------------------------------------------------------- self-test */

int fs_selftest(void)
{
  const char *fn = "fstest.tmp";
  const uint32_t SZ = 1000; /* spans two sectors: 512 + 488     */
  uint8_t buf[1024];
  int ok = 1;

  if (fs_prealloc(fn, SZ) != 0) {
    con_write("fs selftest: prealloc FAILED\n");
    return -1;
  }

  /* A freshly pre-allocated file reads back as all zeros. */
  int zeros_ok = (fs_read(fn, buf, SZ) == (int)SZ);
  for (uint32_t i = 0; i < SZ && zeros_ok; i++)
    if (buf[i] != 0)
      zeros_ok = 0;
  con_write("  prealloc+zero ");
  con_write(zeros_ok ? "PASS\n" : "FAIL\n");

  /* Out-of-order writes: end, start, then across the sector boundary. */
  int w_ok = 1;
  w_ok &= fs_write_at(fn, 996, "1234", 4) == 4; /* last 4 bytes           */
  w_ok &= fs_write_at(fn, 0, "ABCD", 4) == 4;   /* partial, sector start  */
  w_ok &= fs_write_at(fn, 510, "WXYZ", 4) == 4; /* crosses 512 boundary   */
  con_write("  write_at      ");
  con_write(w_ok ? "PASS\n" : "FAIL\n");

  /* A write that would run past EOF must be rejected, not grow the file. */
  int bounds_ok = (fs_write_at(fn, 998, "XXXX", 4) == -1) && (fs_write_at("nope.tmp", 0, "X", 1) == -1);
  con_write("  bounds        ");
  con_write(bounds_ok ? "PASS\n" : "FAIL\n");

  /* Read the whole file back and check the patches landed and nothing else
   * changed. */
  int rb_ok = (fs_read(fn, buf, SZ) == (int)SZ);
  for (uint32_t i = 0; i < SZ && rb_ok; i++) {
    uint8_t want = 0;
    if (i < 4)
      want = (uint8_t)"ABCD"[i];
    else if (i >= 510 && i < 514)
      want = (uint8_t)"WXYZ"[i - 510];
    else if (i >= 996)
      want = (uint8_t)"1234"[i - 996];
    if (buf[i] != want)
      rb_ok = 0;
  }
  con_write("  readback      ");
  con_write(rb_ok ? "PASS\n" : "FAIL\n");

  ok = zeros_ok && w_ok && bounds_ok && rb_ok;
  con_write(ok ? "fs selftest: PASS\n" : "fs selftest: FAIL\n");
  return ok ? 0 : -1;
}

int fs_enum(int idx, char *name, uint32_t *size, uint32_t *mtime)
{
  if (!mounted || idx < 0 || (uint32_t)idx >= dir_cap || !dir[idx].name[0])
    return 0;
  if ((dir[idx].flags & FS_DIR_FLAG) || dir[idx].parent != FS_ROOT)
    return 0; /* SMB sees a flat share of root-level files */
  int i = 0;
  for (; i < FS_NAME_LEN && dir[idx].name[i]; i++)
    name[i] = dir[idx].name[i];
  name[i] = '\0';
  if (size)
    *size = dir[idx].size;
  if (mtime)
    *mtime = dir[idx].mtime;
  return 1;
}

int fs_stat(const char *path, int *is_dir, uint32_t *size, uint32_t *mtime)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) { /* /dev /proc /sys, or a /media volume */
    int d;
    uint32_t sz;
    if (rvol_stat(mv, sub, &d, &sz) != 0)
      return -1;
    if (is_dir)
      *is_dir = d;
    if (size)
      *size = sz;
    if (mtime)
      *mtime = 0; /* generated/removable mtimes not surfaced */
    return 0;
  }
  /* /media -- the synthetic mount root is a directory, even with no mounts. */
  if (is_media_root(path)) {
    if (is_dir)
      *is_dir = 1;
    if (size)
      *size = 0;
    if (mtime)
      *mtime = 0;
    return 0;
  }
  if (!mounted)
    return -1;
  int slot = find_path((path && path[0]) ? path : "/");
  if (slot == FS_SLOT_ROOT) { /* the root directory itself */
    if (is_dir)
      *is_dir = 1;
    if (size)
      *size = 0;
    if (mtime)
      *mtime = 0;
    return 0;
  }
  if (slot < 0)
    return -1;
  if (is_dir)
    *is_dir = (dir[slot].flags & FS_DIR_FLAG) ? 1 : 0;
  if (size)
    *size = dir[slot].size;
  if (mtime)
    *mtime = dir[slot].mtime;
  return 0;
}

int fs_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size, uint32_t *mtime)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) { /* /dev /proc /sys, or a mounted volume */
    int d;
    uint32_t sz;
    if (!rvol_enum(mv, sub, idx, name, &d, &sz))
      return 0;
    if (is_dir)
      *is_dir = d;
    if (size)
      *size = sz;
    if (mtime)
      *mtime = 0;
    return 1;
  }
  if (idx < 0)
    return 0;
  /* /media -- the idx-th active removable mount's leaf name (a directory).  */
  if (is_media_root(path)) {
    int count = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (!mtab[i].used || mtab[i].permanent)
        continue;
      if (count == idx) {
        const char *leaf = mnt_leaf(&mtab[i]);
        int j = 0;
        for (; leaf[j] && j < 17; j++)
          name[j] = leaf[j];
        name[j] = '\0';
        if (is_dir)
          *is_dir = 1;
        if (size)
          *size = 0;
        if (mtime)
          *mtime = 0;
        return 1;
      }
      count++;
    }
    return 0;
  }
  if (!mounted)
    return 0;
  int slot = find_path((path && path[0]) ? path : "/");
  uint16_t parent;
  if (slot == FS_SLOT_ROOT)
    parent = FS_ROOT;
  else if (slot >= 0 && (dir[slot].flags & FS_DIR_FLAG))
    parent = (uint16_t)slot;
  else
    return 0; /* not a directory */

  int count = 0;
  for (uint32_t i = 0; i < dir_cap; i++) {
    if (!dir[i].name[0] || dir[i].parent != parent)
      continue;
    if (count == idx) {
      int j = 0;
      for (; j < FS_NAME_LEN && dir[i].name[j]; j++)
        name[j] = dir[i].name[j];
      name[j] = '\0';
      if (is_dir)
        *is_dir = (dir[i].flags & FS_DIR_FLAG) ? 1 : 0;
      if (size)
        *size = dir[i].size;
      if (mtime)
        *mtime = dir[i].mtime;
      return 1;
    }
    count++;
  }
  /* Synthetic top-level dirs: the /media container, then each permanently
   * mounted pseudo-filesystem (/dev, /proc, /sys), enumerated from the mount
   * table -- no hardcoded names. */
  if (parent == FS_ROOT) {
    if (count == idx) {
      str_copy(name, "media", FS_NAME_LEN);
      if (is_dir)
        *is_dir = 1;
      if (size)
        *size = 0;
      if (mtime)
        *mtime = 0;
      return 1;
    }
    count++;
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (!mtab[i].used || !mtab[i].permanent)
        continue;
      if (count == idx) {
        str_copy(name, mnt_leaf(&mtab[i]), FS_NAME_LEN);
        if (is_dir)
          *is_dir = 1;
        if (size)
          *size = 0;
        if (mtime)
          *mtime = 0;
        return 1;
      }
      count++;
    }
  }
  return 0;
}

/* ----------------------------------------------------------- mount API -----
 */

/* Validate that `point` is "/media/<name>" with exactly one path component
 * after "/media/" (no nested slashes). Returns 0 on success, -1 otherwise. */
static int valid_media_point(const char *point)
{
  const char *m = "/media/";
  const char *p = point;
  while (*m && *p == *m) {
    p++;
    m++;
  }
  if (*m != '\0')
    return -1; /* doesn't start with "/media/"        */
  if (*p == '\0')
    return -1; /* "/media/" with no name              */
  for (; *p; p++)
    if (*p == '/')
      return -1; /* nested slash -> not one component   */
  return 0;
}

static void str_copy(char *dst, const char *src, int cap)
{
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

int fs_mount(const char *dev, const char *point)
{
  if (!dev || !point)
    return -1;
  if (valid_media_point(point) != 0)
    return -1;

  /* Reject a duplicate mount point and find a free slot. */
  int slot = -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used) {
      if (s_eq(mtab[i].point, point))
        return -1; /* already mounted there               */
    } else if (slot < 0) {
      slot = i;
    }
  }
  if (slot < 0)
    return -1; /* mount table full                    */

  blk_read_fn r = NULL;
  blk_write_fn w = NULL;
  int iso = 0;
  if (blk_dev_io(dev, &r, &w, &iso) != 0)
    return -1; /* no such /dev device                 */

  const struct fs_ops *ops = NULL;
  void *inst = NULL;
  if (iso) {
    ops = iso_ops();
    inst = ops ? ops->probe(r, w) : NULL;
    if (!inst)
      return -1;
  } else {
    const struct fs_ops *cand[] = {
        fat_ops(),
        ext_ops(),
        ntfs_ops(),
        hfs_ops(),
    };
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
      if (!cand[i])
        continue;
      inst = cand[i]->probe(r, w);
      if (inst) {
        ops = cand[i];
        break;
      }
    }
    if (!inst)
      return -1; /* no driver recognised the medium     */
  }

  mtab[slot].used = 1;
  mtab[slot].permanent = 0;
  str_copy(mtab[slot].point, point, (int)sizeof mtab[slot].point);
  str_copy(mtab[slot].dev, dev, (int)sizeof mtab[slot].dev);
  mtab[slot].ops = ops;
  mtab[slot].inst = inst;
  return 0;
}

int fs_unmount(const char *point)
{
  if (!point)
    return -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used && s_eq(mtab[i].point, point)) {
      if (mtab[i].permanent)
        return -1; /* a kernel pseudo-fs is not unmountable */
      if (mtab[i].ops->close)
        mtab[i].ops->close(mtab[i].inst);
      mtab[i].used = 0;
      mtab[i].point[0] = '\0';
      mtab[i].dev[0] = '\0';
      mtab[i].ops = NULL;
      mtab[i].inst = NULL;
      return 0;
    }
  }
  return -1; /* no such mount                       */
}

int fs_mount_enum(int idx, struct fs_mountinfo *out)
{
  if (idx < 0 || !out)
    return 0;
  int count = 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mtab[i].used || mtab[i].permanent) /* hide the pseudo-fs mounts */
      continue;
    if (count == idx) {
      str_copy(out->point, mtab[i].point, (int)sizeof out->point);
      str_copy(out->dev, mtab[i].dev, (int)sizeof out->dev);
      out->type = mtab[i].ops->type ? mtab[i].ops->type(mtab[i].inst) : "";
      out->label = mtab[i].ops->label ? mtab[i].ops->label(mtab[i].inst) : "";
      return 1;
    }
    count++;
  }
  return 0; /* past the last mount                 */
}
