/* fs.c: owofs implementation (see fs.h for the on-disk layout). */

#include <stddef.h>

#include "blk.h"
#include "console.h"
#include "fs.h"
#include "fsmigrate.h"
#include "fsmount.h"
#include "heap.h"
#include "kstring.h"
#include "perm.h"
#include "procfs.h"
#include "rtc.h"
#include "sched.h"
#include "sysfs.h"
#include "tmpfs.h"

/* Mount table: removable volumes appear under "/media/<name>"; /dev, /proc, /sys and /tmp are permanent
 * mounts hidden from fs_mount_enum/umount. mount_for() longest-prefix matches a path and returns the
 * owning mount plus the path within the volume, or NULL for plain owofs. Writable iff ops expose create(). */
#define MAX_MOUNTS 12

struct mnt {
  int used;
  bool permanent; /* a kernel pseudo-fs: never unmounts  */
  char point[48]; /* "/media/usb0", "/proc"              */
  char dev[16];   /* "usb0", "proc"                      */
  const struct fs_ops *ops;
  void *inst;
  uint16_t dir_mode; /* mount-wide mode for volumes without permissions */
  uint16_t file_mode;
};

static struct mnt mtab[MAX_MOUNTS];

static int s_len(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

/* Longest-prefix match of path against active mount points (next char must be '\0' or '/').
 * *sub gets the remainder within the volume ("" for the point itself). NULL if none. */
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
    if (*m != '\0')
      continue;
    if (*p != '\0' && *p != '/') /* "/media/usb0x" must not match       */
      continue;
    int len = s_len(mtab[i].point);
    if (len > best_len) {
      best = &mtab[i];
      best_len = len;
      *sub = p;
    }
  }
  return best;
}

static bool rvol_readonly(struct mnt *m) { return m->ops->create == NULL; }

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

/* True for "/media" or "/media/", the synthetic mount-root directory. */
static bool is_media_root(const char *path)
{
  if (!path)
    return false;
  const char *m = "/media";
  while (*m && *path == *m) {
    path++;
    m++;
  }
  if (*m != '\0')
    return false;
  return *path == '\0' || (*path == '/' && path[1] == '\0');
}

#define SECTOR_SIZE 512
#define FS_MAGIC FSM_MAGIC_V9
#define FS_NAME_LEN FSM_NAME_LEN
#define SUPER_LBA 0
#define RESERVED_SECTORS 1
#define DIR_INIT_ENTRIES 256
#define FS_MAX_DIR_SECTORS                                                                                             \
  16384u /* 8 MiB extent (~100k entries); bounds untrusted dir_sectors so the byte size can't overflow */
#define FS_MAX_CLUSTERS                                                                                                \
  262144 /* fixed 32 KiB allocation bitmap; cluster size scales with disk capacity so any size fits */

#define FS_DIR_FLAG FSM_DIR_FLAG /* dirent.flags: entry is a directory    */
#define FS_ROOT FSM_ROOT         /* dirent.parent value meaning "in root" */
#define FS_SLOT_ROOT (-2)

/* Directory entries form a tree: each names one component and points at its parent slot (FS_ROOT at top;
 * the root has no dirent). */

/* The directory is a growable extent allocated like a file; free_slot() relocates it when full. */
struct super {
  uint32_t magic;
  uint32_t dir_start;   /* first sector of the directory extent    */
  uint32_t dir_sectors; /* sectors in the directory extent         */
  uint8_t pad[SECTOR_SIZE - 12];
};

static struct super sb;
static struct owofs_dirent *dir; /* heap-allocated mirror of the directory  */
static uint32_t dir_cap;         /* entries the current extent/mirror hold  */
static bool mounted;

/* Data-sector allocation: in-RAM bitmap of clusters, rebuilt from the directory at mount, never stored.
 * Files are contiguous {start, size} extents. The cluster size is chosen so the disk fits in
 * FS_MAX_CLUSTERS bits; allocation rounds up to whole clusters so no two files share one. */
static uint8_t clus_used[FS_MAX_CLUSTERS / 8]; /* 1 bit per allocation cluster  */
static uint32_t fs_total_sectors;              /* full capacity of the disk     */
static uint32_t clus_sectors;                  /* sectors per cluster (>= 1)    */
static uint32_t clus_count;                    /* whole clusters on this disk   */

static bool clus_test(uint32_t c) { return (clus_used[c >> 3] >> (c & 7)) & 1; }
static void clus_set(uint32_t c) { clus_used[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static void clus_clr(uint32_t c) { clus_used[c >> 3] &= (uint8_t)~(1u << (c & 7)); }

/* Sectors a file of `size` bytes occupies (always >= 1, matching fs_create). */
static uint32_t size_to_sectors(uint32_t size)
{
  uint32_t n = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  return n ? n : 1;
}

/* Mark every cluster in sector range [start, start+nsec) used or free; extents must be cluster-aligned. */
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

/* Free a file's extent, guarding the superblock sector and the start==0 sentinel of an empty file. */
static void free_extent(uint32_t start, uint32_t size)
{
  if (start >= (uint32_t)RESERVED_SECTORS)
    run_mark(start, size_to_sectors(size), 0);
}

/* Directory entries that fit in the given sectors. */
static uint32_t entries_for(uint32_t sectors) { return sectors * SECTOR_SIZE / (uint32_t)sizeof(struct owofs_dirent); }

/* Pick the cluster size so the disk fits in FS_MAX_CLUSTERS bits; a partial final cluster is unused. */
static void set_capacity(void)
{
  fs_total_sectors = blk_sectors();
  if (fs_total_sectors == 0)
    fs_total_sectors = 1;
  clus_sectors = (fs_total_sectors + FS_MAX_CLUSTERS - 1) / FS_MAX_CLUSTERS;
  if (clus_sectors == 0)
    clus_sectors = 1;
  clus_count = fs_total_sectors / clus_sectors;
}

/* Clear the bitmap and reserve only the superblock sector. */
static void reset_alloc_map(void)
{
  for (uint32_t i = 0; i < sizeof clus_used; i++)
    clus_used[i] = 0;
  run_mark(0, (uint32_t)RESERVED_SECTORS, 1);
}

/* Rebuild the bitmap from the on-disk directory: reserve the superblock and directory, mark live extents. */
static void rebuild_alloc_map(void)
{
  reset_alloc_map();
  run_mark(sb.dir_start, sb.dir_sectors, 1);
  for (uint32_t i = 0; i < dir_cap; i++) {
    if (!dir[i].name[0] || (dir[i].flags & FS_DIR_FLAG) || dir[i].start == 0)
      continue; /* dirs + empty files own no data sectors */
    run_mark(dir[i].start, size_to_sectors(dir[i].size), 1);
  }
}

/* First-fit search for contiguous free clusters holding nsec sectors; marks them used and returns the
 * start LBA, or 0 (never a valid data LBA) when full. */
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
    if (!(c & 7) && need - run >= 8 && clus_used[c >> 3] == 0xFF) {
      run = 0;
      c += 7;
      continue;
    }
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

/* Largest contiguous free run for a streaming write of unknown size: returns its sectors, start in *start. */
static uint32_t largest_free_run(uint32_t *start)
{
  uint32_t first = ((uint32_t)RESERVED_SECTORS + clus_sectors - 1) / clus_sectors;
  uint32_t best = 0, best_c = 0, run = 0, rc = 0;
  for (uint32_t c = first; c < clus_count; c++) {
    if (!(c & 7) && clus_used[c >> 3] == 0xFF) {
      run = 0;
      c += 7;
      continue;
    }
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

/* Load the directory extent into a fresh heap mirror sized to the whole extent. 0, or -1 on alloc failure. */
static int dir_load(void)
{
  /* sb.dir_sectors is untrusted: reject values that overflow, exceed the ceiling or the device. */
  if (sb.dir_sectors == 0 || sb.dir_sectors > FS_MAX_DIR_SECTORS || sb.dir_start >= fs_total_sectors ||
      sb.dir_sectors > fs_total_sectors - sb.dir_start)
    return -1;
  uint32_t bytes = sb.dir_sectors * SECTOR_SIZE;
  dir = kmalloc(bytes);
  if (!dir)
    return -1;
  dir_cap = entries_for(sb.dir_sectors);
  return blk_read(sb.dir_start, dir, sb.dir_sectors);
}

/* Write the whole directory extent back to disk. */
static int dir_store(void) { return blk_write(sb.dir_start, dir, sb.dir_sectors); }

/* Grow the directory by relocating it to a double-size extent, persisting it in the superblock.
 * Returns 0, or -1 if the disk or heap is exhausted. */
static int grow_dir(void)
{
  uint32_t new_sectors = sb.dir_sectors ? sb.dir_sectors * 2 : 1;
  if (new_sectors > FS_MAX_DIR_SECTORS) /* dir_load would refuse the disk at the next mount */
    return -1;
  uint32_t new_start = alloc_run(new_sectors);
  if (new_start == 0)
    return -1;
  uint32_t new_bytes = new_sectors * SECTOR_SIZE;
  struct owofs_dirent *nd = kmalloc(new_bytes);
  if (!nd) {
    run_mark(new_start, new_sectors, 0);
    return -1;
  }

  uint8_t *d = (uint8_t *)nd, *s = (uint8_t *)dir;
  uint32_t old_bytes = dir_cap * (uint32_t)sizeof(struct owofs_dirent);
  memset(d, 0, new_bytes);
  memcpy(d, s, old_bytes);

  if (blk_write(new_start, d, new_sectors) != 0) {
    kfree(nd);
    run_mark(new_start, new_sectors, 0);
    return -1;
  }

  /* Point the on-disk superblock at the new extent before switching to it in memory. */
  uint32_t old_start = sb.dir_start, old_sectors = sb.dir_sectors;
  sb.dir_start = new_start;
  sb.dir_sectors = new_sectors;
  if (blk_write_sector(SUPER_LBA, &sb) != 0) {
    sb.dir_start = old_start;
    sb.dir_sectors = old_sectors;
    kfree(nd);
    run_mark(new_start, new_sectors, 0);
    return -1;
  }
  kfree(dir);
  dir = nd;
  dir_cap = entries_for(new_sectors);
  free_extent(old_start, old_sectors * SECTOR_SIZE);
  return 0;
}

static bool name_eq(const char *a, const char *b)
{
  for (int i = 0; i < FS_NAME_LEN; i++) {
    if (a[i] != b[i])
      return false;
    if (a[i] == '\0')
      return true;
  }
  return true;
}

static void name_copy(char *dst, const char *src)
{
  int i = 0;
  for (; i < FS_NAME_LEN - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

/* Wall-clock packed into 32 bits: [year-2000:6][month:4][day:5][hour:5][min:6][sec:6]. Good to 2063. */
static uint32_t pack_now(void)
{
  struct rtc_time t;
  rtc_now(&t);
  uint32_t y = (t.year >= 2000) ? (uint32_t)(t.year - 2000) : 0;
  return ((y & 0x3F) << 26) | (((uint32_t)t.month & 0xF) << 22) | (((uint32_t)t.day & 0x1F) << 17) |
         (((uint32_t)t.hour & 0x1F) << 12) | (((uint32_t)t.min & 0x3F) << 6) | ((uint32_t)t.sec & 0x3F);
}

/* Unix seconds (proleptic Gregorian, UTC) to the packed form above. */
static uint32_t pack_unix(uint32_t t)
{
  uint32_t secs = t % 86400u;
  uint32_t z = t / 86400u + 719468u;
  uint32_t era = z / 146097u, doe = z - era * 146097u;
  uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  uint32_t mp = (5u * doy + 2u) / 153u;
  uint32_t day = doy - (153u * mp + 2u) / 5u + 1u;
  uint32_t mon = mp < 10u ? mp + 3u : mp - 9u;
  uint32_t year = yoe + era * 400u + (mon <= 2u ? 1u : 0u);
  uint32_t y = year >= 2000u ? year - 2000u : 0u;
  return ((y & 0x3F) << 26) | ((mon & 0xF) << 22) | ((day & 0x1F) << 17) | (((secs / 3600u) & 0x1F) << 12) |
         (((secs / 60u % 60u) & 0x3F) << 6) | ((secs % 60u) & 0x3F);
}

/* Packed mtime of a path on a mounted volume, 0 when the driver has none. */
static uint32_t mnt_mtime(struct mnt *m, const char *path)
{
  uint32_t t = 0;
  if (!m->ops->mtime || m->ops->mtime(m->inst, path, &t) != 0 || t == 0)
    return 0;
  return pack_unix(t);
}

static bool s_eq(const char *a, const char *b)
{
  int i = 0;
  for (; a[i] && b[i]; i++)
    if (a[i] != b[i])
      return false;
  return a[i] == b[i];
}

/* Slots a parent field can name: 16 bits, with FS_ROOT itself reserved for "in root". */
static uint32_t usable_slots(void) { return dir_cap < FS_ROOT ? dir_cap : FS_ROOT; }

/* First unused directory slot, growing the directory if full; -1 if it can't grow. */
static int free_slot(void)
{
  for (uint32_t i = 0; i < usable_slots(); i++)
    if (!dir[i].name[0])
      return (int)i;
  if (dir_cap >= FS_ROOT || grow_dir() != 0)
    return -1;
  for (uint32_t i = 0; i < usable_slots(); i++)
    if (!dir[i].name[0])
      return (int)i;
  return -1;
}

/* Slot of the entry named leaf directly inside parent (FS_ROOT for top level), or -1. */
static int find_in(int parent, const char *leaf)
{
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && dir[i].parent == (uint16_t)parent && name_eq(dir[i].name, leaf))
      return (int)i;
  return -1;
}

/* Walk path to its containing directory; the last component goes to leaf[] (>= FS_NAME_LEN bytes),
 * the holder slot to *parent. "." and ".." are honored; a trailing slash or directory path gives an empty leaf.
 * Returns 0, or -1 if a component is too long or an intermediate is not a directory. */
static int resolve_path(const char *path, int *parent, char *leaf)
{
  int cur = FS_ROOT;
  const char *p = path;
  char comp[FS_NAME_LEN];

  for (;;) {
    while (*p == '/')
      p++;
    if (*p == '\0') {
      leaf[0] = '\0';
      *parent = cur;
      return 0;
    }
    int n = 0;
    while (p[n] && p[n] != '/')
      n++;
    if (n >= FS_NAME_LEN)
      return -1;
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
    int s = find_in(cur, comp);
    if (s < 0 || !(dir[s].flags & FS_DIR_FLAG))
      return -1;
    cur = s;
  }
}

/* Resolve path to a slot, FS_SLOT_ROOT for the root, or -1 if absent. */
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

/* Pseudo-filesystem mounts: /dev, /proc and /sys are real mount-table entries backed by stateless fs_ops
 * (inst == NULL), generated on demand by blk.c / procfs.c / sysfs.c, and dispatched via rvol_*. */

/* These reuse the listing/format helpers and str_copy defined further down. */
static void write_mtime(uint32_t m);
static void write_pad(uint32_t v, int width);
static void str_copy(char *dst, const char *src, int cap);

/* "" or "/" -- the mount point itself, i.e. the pseudo-fs root directory. */
static bool sub_is_root(const char *p) { return !p[0] || (p[0] == '/' && !p[1]); }

/* /dev: present block devices as regular, non-byte-readable files. */
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
  if (sub_is_root(path)) {
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
    if (s_eq(devs[i].dev, nm)) {
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
  if (!sub_is_root(path))
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
{
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
    return -1;
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

/* Generated pseudo-file reader with arbitrary offset: offset 0 generates into the caller's buffer, else
 * regenerates into a scratch buffer and slices. */
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

/* /proc: flat, one generated file per fact (see procfs.c). */
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
  int k = procfs_kind(path);
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
  if (!sub_is_root(path))
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
  if (procfs_kind(path) != 0)
    return -1;
  return gen_read_at(procfs_read, path, off, buf, len);
}

static int procfs_list_op(void *inst, const char *path)
{
  (void)inst;
  int k = procfs_kind(path);
  if (k < 0)
    return -1;
  if (k == 0) {
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

/* /sys: nested tree of device tunables (see sysfs.c). */
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
  int k = sysfs_kind(path);
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
    else {
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
  int bin = sysfs_read_binary(path, off, buf, len);
  return bin != SYSFS_NOT_BINARY ? bin : gen_read_at(sysfs_read, path, off, buf, len);
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

/* Mode and owner of `sub` inside mount m: fixed for the pseudo-fs, the real inode for ext, else the mount default. */
static void mnt_attr(struct mnt *m, const char *sub, bool is_dir, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
  *uid = *gid = 0;
  const char *leaf = (sub[0] == '/') ? sub + 1 : sub;
  if (m->ops == &devfs_ops)
    *mode = is_dir ? 0555 : 0644;
  else if (m->ops == &procfs_ops)
    *mode = is_dir ? 0555 : s_eq(leaf, "login") || s_eq(leaf, "sudo") ? 0666 : s_eq(leaf, "hostclip") ? 0644 : 0444;
  else if (m->ops == &sysfs_ops)
    *mode = is_dir ? 0555 : sysfs_writable(sub) ? 0644 : 0444;
  else if (!m->ops->getattr || m->ops->getattr(m->inst, sub, mode, uid, gid) != 0)
    *mode = is_dir ? m->dir_mode : m->file_mode;
}

/* Graft a permanent filesystem into the mount table at `point`. 1 when mounted, 0 when the point is already
 * mounted, -1 when the table is full. */
static int mount_pseudo(const char *point, const char *dev, const struct fs_ops *ops, void *inst)
{
  for (int i = 0; i < MAX_MOUNTS; i++)
    if (mtab[i].used && s_eq(mtab[i].point, point))
      return 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used)
      continue;
    mtab[i].used = 1;
    mtab[i].permanent = true;
    str_copy(mtab[i].point, point, (int)sizeof mtab[i].point);
    str_copy(mtab[i].dev, dev, (int)sizeof mtab[i].dev);
    mtab[i].ops = ops;
    mtab[i].inst = inst;
    return 1;
  }
  return -1;
}

/* Mount the permanent filesystems; independent of owofs, so present on a no-disk boot. */
static void fs_mount_pseudo(void)
{
  mount_pseudo("/dev", "dev", &devfs_ops, NULL);
  mount_pseudo("/proc", "proc", &procfs_ops, NULL);
  mount_pseudo("/sys", "sys", &sysfs_ops, NULL);
  void *tmp = tmpfs_new();
  int rc = tmp ? mount_pseudo("/tmp", "tmp", tmpfs_ops(), tmp) : -1;
  if (rc != 1 && tmp)
    tmpfs_ops()->close(tmp);
  if (rc < 0)
    con_write("fs: cannot mount /tmp\n");
}

/* An owofs root entry named like a permanent mount (the on-disk /tmp older kernels made) is hidden behind it. */
static bool shadowed_by_mount(uint32_t i)
{
  if (dir[i].parent != FS_ROOT)
    return false;
  for (int k = 0; k < MAX_MOUNTS; k++)
    if (mtab[k].used && mtab[k].permanent && s_eq(mnt_leaf(&mtab[k]), dir[i].name))
      return true;
  return false;
}

void fs_format(void)
{
  set_capacity();
  reset_alloc_map();

  uint32_t init_sectors = (DIR_INIT_ENTRIES * (uint32_t)sizeof(struct owofs_dirent) + SECTOR_SIZE - 1) / SECTOR_SIZE;
  uint32_t start = alloc_run(init_sectors);
  if (start == 0) {
    mounted = false;
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
    mounted = false;
    return;
  }
  for (uint32_t i = 0; i < bytes; i++)
    ((uint8_t *)dir)[i] = 0;
  dir_cap = entries_for(init_sectors);

  blk_write_sector(SUPER_LBA, &sb);
  dir_store();
  mounted = true;
}

/* Convert a v8 disk: write a v9 directory to a fresh extent, then flip the superblock; the old extent is
 * only released once the new superblock is on disk, so a failure leaves the v8 disk untouched. */
static int migrate_v8(void)
{
  uint32_t old_start = sb.dir_start, old_sectors = sb.dir_sectors;
  if (old_sectors == 0 || old_sectors > FS_MAX_DIR_SECTORS || old_start >= fs_total_sectors ||
      old_sectors > fs_total_sectors - old_start)
    return -1;
  uint32_t new_sectors = fsm_v9_sectors(old_sectors);
  if (new_sectors > FS_MAX_DIR_SECTORS)
    return -1;
  struct owofs_dirent_v8 *old = kmalloc(old_sectors * SECTOR_SIZE);
  struct owofs_dirent *nd = kmalloc(new_sectors * SECTOR_SIZE);
  int rc = -1;
  if (old && nd && blk_read(old_start, old, old_sectors) == 0) {
    uint32_t n_old = fsm_entries_v8(old_sectors);
    reset_alloc_map();
    run_mark(old_start, old_sectors, 1);
    for (uint32_t i = 0; i < n_old; i++)
      if (old[i].name[0] && !(old[i].flags & FS_DIR_FLAG) && old[i].start != 0)
        run_mark(old[i].start, size_to_sectors(old[i].size), 1);
    uint32_t new_start = alloc_run(new_sectors);
    if (new_start) {
      fsm_convert(old, n_old, nd, fsm_entries_v9(new_sectors));
      struct super next = sb;
      next.magic = FS_MAGIC;
      next.dir_start = new_start;
      next.dir_sectors = new_sectors;
      if (blk_write(new_start, nd, new_sectors) == 0 && blk_write_sector(SUPER_LBA, &next) == 0) {
        sb = next;
        dir = nd;
        nd = NULL;
        dir_cap = entries_for(new_sectors);
        rebuild_alloc_map();
        rc = 0;
      }
    }
  }
  kfree(old);
  kfree(nd);
  return rc;
}

void fs_init(void)
{
  fs_mount_pseudo();

  int have = (blk_read_sector(SUPER_LBA, &sb) == 0);

  if (have && sb.magic == FSM_MAGIC_V8) {
    con_write("fs: migrating owofs v8 to v9 (owner and mode).\n");
    set_capacity();
    mounted = (migrate_v8() == 0);
    if (!mounted)
      con_write("fs: migration failed; disk left untouched.\n");
    return;
  }

  if (have && sb.magic == FS_MAGIC) {
    set_capacity();
    if (dir_load() != 0) {
      mounted = false;
      return;
    }
    rebuild_alloc_map();
    mounted = true;
    return;
  }
  if (have && blk_is_ide()) {
    con_write("fs: unformatted IDE disk -- creating owofs.\n");
    fs_format();
    return;
  }

  /* No usable persistent disk (none, or an AHCI/NVMe OS drive that must not be auto-formatted): fall back
   * to a volatile RAM filesystem; the real disk is only ever read. */
  if (blk_use_ram() == 0) {
    con_write("fs: no owofs disk -- using RAM (tmpfs; not persistent).\n");
    fs_format();
    return;
  }
  con_write("fs: no disk and RAM fs unavailable.\n");
  mounted = false;
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
  if (m == 0) {
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
  char mode[11];
  perm_mode_str(dir[i].mode, (dir[i].flags & FS_DIR_FLAG) != 0, mode);
  con_write(mode);
  con_write("  ");
  write_mtime(dir[i].mtime);
  con_write("  ");
  if (dir[i].flags & FS_DIR_FLAG)
    con_write("   <DIR>");
  else
    write_pad(dir[i].size, 8);
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
  if (mv) {
    if (rvol_list(mv, sub) < 0)
      con_write("ls: no such file or directory\n");
    return;
  }
  /* /media: synthetic directory of the active removable mounts. */
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
    list_row(d);
    return;
  }
  int parent = (d == FS_SLOT_ROOT) ? (int)FS_ROOT : d;
  int n = 0;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && dir[i].parent == (uint16_t)parent && !shadowed_by_mount(i)) {
      list_row(i);
      n++;
    }
  if (d == FS_SLOT_ROOT) {
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

int fs_read(const char *name, void *buf, uint32_t maxlen) { return fs_read_at(name, 0, buf, maxlen); }

/* Size of a file in bytes, or -1 if absent or a directory. */
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

/* Read up to len bytes at byte offset within a file (contiguous storage, so just sector math).
 * Returns bytes read (0 at/after EOF), or -1 if absent. */
int fs_read_at(const char *name, uint32_t offset, void *buf, uint32_t len)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
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

    /* Whole aligned sectors go straight into the caller's buffer in one batched read, no bounce. */
    if (soff == 0 && left >= SECTOR_SIZE) {
      uint32_t nsec = left / SECTOR_SIZE;
      if (blk_read(sec, (uint8_t *)buf + copied, nsec) != 0)
        return (int)copied;
      copied += nsec * SECTOR_SIZE;
      continue;
    }

    if (blk_read_sector(sec, tmp) != 0)
      return (int)copied;
    uint32_t chunk = SECTOR_SIZE - soff;
    if (chunk > left)
      chunk = left;
    memcpy((uint8_t *)buf + copied, tmp + soff, chunk);
    copied += chunk;
  }
  return (int)copied;
}

/* Stamp a newly created entry with the caller's identity and umask-reduced mode. */
static void entry_set_owner(int slot, uint32_t base_mode)
{
  dir[slot].uid = (uint16_t)sched_uid();
  dir[slot].gid = (uint16_t)sched_gid();
  dir[slot].mode = (uint16_t)perm_apply_umask(base_mode, sched_umask());
}

int fs_create(const char *name, const void *data, uint32_t size)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1;
    return mv->ops->create(mv->inst, sub, data, size) == 0 ? 0 : -1;
  }
  if (!mounted)
    return -1;

  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1;

  int slot = find_in(parent, leaf);
  bool overwrite = false;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG)
      return -1;
    overwrite = true;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0)
      return -1;
  }

  /* Allocate the new extent before freeing the old, so a failed write leaves the file intact. */
  uint32_t sectors = size_to_sectors(size);
  uint32_t start = alloc_run(sectors);
  if (start == 0)
    return -1;

  uint8_t tmp[SECTOR_SIZE];
  uint32_t full = size / SECTOR_SIZE;
  if (full && blk_write(start, data, full) != 0) {
    run_mark(start, sectors, 0);
    return -1;
  }
  uint32_t written = full * SECTOR_SIZE;
  if (written < size) {
    memset(tmp, 0, sizeof tmp);
    uint32_t chunk = (size - written) < SECTOR_SIZE ? (size - written) : SECTOR_SIZE;
    memcpy(tmp, (const uint8_t *)data + written, chunk);
    if (blk_write_sector(start + full, tmp) != 0) {
      run_mark(start, sectors, 0);
      return -1;
    }
  }

  name_copy(dir[slot].name, leaf);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = size;
  dir[slot].start = start;
  dir[slot].mtime = pack_now();
  if (!overwrite)
    entry_set_owner(slot, 0666);

  if (overwrite)
    free_extent(old_start, old_size);
  if (dir_store() != 0)
    return -1;
  return 0;
}

/* Wipe a directory slot; the caller must free_extent the data first. */
static void slot_clear(int i)
{
  dir[i].name[0] = '\0';
  dir[i].flags = 0;
  dir[i].parent = FS_ROOT;
  dir[i].size = 0;
  dir[i].start = 0;
  dir[i].mtime = 0;
  dir[i].mode = 0;
  dir[i].uid = 0;
  dir[i].gid = 0;
}

int fs_delete(const char *name)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1;
    return mv->ops->del(mv->inst, sub) == 0 ? 0 : -1;
  }
  if (!mounted)
    return -1;
  int i = find_path(name);
  if (i < 0 || i == FS_SLOT_ROOT)
    return -1;
  if (dir[i].flags & FS_DIR_FLAG)
    return -2;
  free_extent(dir[i].start, dir[i].size);
  slot_clear(i);
  return (dir_store() == 0) ? 0 : -1;
}

int fs_rename(const char *old, const char *neu)
{
  const char *osub, *nsub;
  struct mnt *om = mount_for(old, &osub), *nm = mount_for(neu, &nsub);
  if (om || nm) /* a rename can't cross mounts */
    return (om == nm && om->ops->rename && om->ops->rename(om->inst, osub, nsub) == 0) ? 0 : -1;
  if (!mounted)
    return -1;

  int src = find_path(old);
  if (src < 0 || src == FS_SLOT_ROOT)
    return -1;

  int dparent;
  char dleaf[FS_NAME_LEN];
  if (resolve_path(neu, &dparent, dleaf) != 0 || dleaf[0] == '\0')
    return -1;

  if (dir[src].flags & FS_DIR_FLAG) {
    int c = dparent;
    while (c != (int)FS_ROOT) {
      if (c == src)
        return -1;
      c = dir[c].parent;
    }
  }

  int ex = find_in(dparent, dleaf);
  if (ex >= 0 && ex != src) {
    if (dir[ex].flags & FS_DIR_FLAG)
      return -1;
    free_extent(dir[ex].start, dir[ex].size);
    slot_clear(ex);
  }

  name_copy(dir[src].name, dleaf);
  dir[src].parent = (uint16_t)dparent;
  dir[src].mtime = pack_now();
  return (dir_store() == 0) ? 0 : -1;
}

int fs_mkdir(const char *path)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1;
    return mv->ops->mkdir(mv->inst, sub) == 0 ? 0 : -1;
  }
  if (!mounted)
    return -1;
  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(path, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1;
  if (find_in(parent, leaf) >= 0)
    return -1;
  int slot = free_slot();
  if (slot < 0)
    return -1;
  name_copy(dir[slot].name, leaf);
  dir[slot].flags = FS_DIR_FLAG;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = 0;
  dir[slot].start = 0;
  dir[slot].mtime = pack_now();
  entry_set_owner(slot, 0777);
  return (dir_store() == 0) ? 0 : -1;
}

static bool stream_targets(int slot);

int fs_rmdir(const char *path)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    if (rvol_readonly(mv))
      return -1;
    return mv->ops->rmdir(mv->inst, sub) == 0 ? 0 : -1;
  }
  if (!mounted)
    return -1;
  int s = find_path(path);
  if (s < 0 || s == FS_SLOT_ROOT)
    return -1;
  if (!(dir[s].flags & FS_DIR_FLAG))
    return -2;
  for (uint32_t i = 0; i < dir_cap; i++)
    if (dir[i].name[0] && dir[i].parent == (uint16_t)s)
      return -3;
  if (stream_targets(s)) /* the stream remembers the slot: a reused one would put its file in another directory */
    return -3;
  slot_clear(s);
  return (dir_store() == 0) ? 0 : -1;
}

int fs_isdir(const char *path)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
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
  {
    const char *sub;
    if (mount_for(name, &sub))
      return -1;
  }

  int parent;
  char leaf[FS_NAME_LEN];
  if (resolve_path(name, &parent, leaf) != 0 || leaf[0] == '\0')
    return -1;

  int slot = find_in(parent, leaf);
  bool overwrite = false;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG)
      return -1;
    overwrite = true;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0)
      return -1;
  }

  uint32_t sectors = size_to_sectors(size);
  uint32_t start = alloc_run(sectors);
  if (start == 0)
    return -1;

  uint8_t zero[SECTOR_SIZE * 8];
  memset(zero, 0, sizeof zero);
  for (uint32_t s = 0; s < sectors;) {
    uint32_t n = sectors - s;
    if (n > 8)
      n = 8;
    if (blk_write(start + s, zero, n) != 0) {
      run_mark(start, sectors, 0);
      return -1;
    }
    s += n;
  }

  name_copy(dir[slot].name, leaf);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)parent;
  dir[slot].size = size;
  dir[slot].start = start;
  dir[slot].mtime = pack_now();
  if (!overwrite)
    entry_set_owner(slot, 0666);

  if (overwrite)
    free_extent(old_start, old_size);
  if (dir_store() != 0)
    return -1;
  return 0;
}

int fs_write_at(const char *name, uint32_t offset, const void *data, uint32_t len)
{
  const char *sub;
  struct mnt *mv = mount_for(name, &sub);
  if (mv) {
    int r = mv->ops->write_at ? mv->ops->write_at(mv->inst, sub, offset, data, len) : -1;
    return r < 0 ? -1 : r;
  }
  if (!mounted)
    return -1;
  int i = find_file(name);
  if (i >= 0) {
    if (offset > dir[i].size || len > dir[i].size - offset)
      return -1;

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

      if (soff == 0 && chunk == SECTOR_SIZE) {
        uint32_t n = (len - written) / SECTOR_SIZE;
        if (n > 8)
          n = 8;
        if (blk_write(sec, p + written, n) != 0)
          return -1;
        written += n * SECTOR_SIZE;
        continue;
      }
      if (soff != 0 || chunk != SECTOR_SIZE) {
        if (blk_read_sector(sec, tmp) != 0)
          return -1;
      }
      memcpy(tmp + soff, p + written, chunk);
      if (blk_write_sector(sec, tmp) != 0)
        return -1;
      written += chunk;
    }
    return (int)written;
  }
  return -1;
}

/* Streaming creation for data of unknown final size: begin, write*, end. Not reentrant. begin reserves the
 * largest free run; end releases the unused tail, and a dead stream frees its whole reservation. */
static bool strm_active;
static char strm_name[FS_NAME_LEN];       /* final path component (leaf)          */
static int strm_parent;                   /* slot of the directory to create it in */
static uint32_t strm_start;               /* first data sector (reserved at begin) */
static uint32_t strm_reserved;            /* sectors reserved for the stream       */
static uint32_t strm_sectors;             /* full sectors flushed so far           */
static uint32_t strm_size;                /* bytes accepted so far                 */
static uint8_t strm_buf[SECTOR_SIZE * 8]; /* the batch currently filling */
static uint32_t strm_fill;                /* bytes pending in strm_buf             */

static bool stream_targets(int slot) { return strm_active && strm_parent == slot; }

int fs_stream_begin(const char *name)
{
  if (!mounted || strm_active)
    return -1;
  {
    const char *sub;
    if (mount_for(name, &sub))
      return -1;
  }
  if (resolve_path(name, &strm_parent, strm_name) != 0 || strm_name[0] == '\0')
    return -1;
  strm_reserved = largest_free_run(&strm_start);
  if (strm_reserved == 0)
    return -1;
  run_mark(strm_start, strm_reserved, 1);
  strm_sectors = 0;
  strm_size = 0;
  strm_fill = 0;
  strm_active = true;
  return 0;
}

int fs_stream_write(const void *data, uint32_t size)
{
  if (!strm_active)
    return -1;
  const uint8_t *p = data;
  while (size) {
    uint32_t n = sizeof strm_buf - strm_fill;
    if (n > size)
      n = size;
    memcpy(strm_buf + strm_fill, p, n);
    strm_fill += n;
    strm_size += n;
    p += n;
    size -= n;
    if (strm_fill == sizeof strm_buf) {
      if (strm_reserved - strm_sectors < 8 || blk_write(strm_start + strm_sectors, strm_buf, 8) != 0) {
        run_mark(strm_start, strm_reserved, 0);
        strm_active = false;
        return -1;
      }
      strm_sectors += 8;
      strm_fill = 0;
    }
  }
  return 0;
}

void fs_stream_abort(void)
{
  if (!strm_active)
    return;
  run_mark(strm_start, strm_reserved, 0);
  strm_active = false;
}

int fs_stream_end(void)
{
  if (!strm_active)
    return -1;
  strm_active = false;

  if (strm_fill > 0) {
    uint32_t full = strm_fill / SECTOR_SIZE;
    if (full) {
      if (strm_reserved - strm_sectors < full || blk_write(strm_start + strm_sectors, strm_buf, full) != 0) {
        run_mark(strm_start, strm_reserved, 0);
        return -1;
      }
      strm_sectors += full;
    }
    uint32_t rem = strm_fill % SECTOR_SIZE;
    if (rem) {
      uint8_t *tail = strm_buf + full * SECTOR_SIZE;
      memset(tail + rem, 0, SECTOR_SIZE - rem);
      if (strm_sectors >= strm_reserved || blk_write_sector(strm_start + strm_sectors, tail) != 0) {
        run_mark(strm_start, strm_reserved, 0);
        return -1;
      }
      strm_sectors++;
    }
    if (strm_sectors > strm_reserved) {
      run_mark(strm_start, strm_reserved, 0);
      return -1;
    }
    strm_fill = 0;
  }

  int slot = find_in(strm_parent, strm_name);
  bool overwrite = false;
  uint32_t old_start = 0, old_size = 0;
  if (slot >= 0) {
    if (dir[slot].flags & FS_DIR_FLAG) {
      run_mark(strm_start, strm_reserved, 0);
      return -1;
    }
    overwrite = true;
    old_start = dir[slot].start;
    old_size = dir[slot].size;
  } else {
    slot = free_slot();
    if (slot < 0) {
      run_mark(strm_start, strm_reserved, 0);
      return -1;
    }
  }

  /* Return the unused tail of the reservation: keep the cluster of the last used sector, free the rest. */
  uint32_t used_end = strm_start + strm_sectors;
  uint32_t tail = (used_end + clus_sectors - 1) / clus_sectors * clus_sectors;
  if (strm_start + strm_reserved > tail)
    run_mark(tail, (strm_start + strm_reserved) - tail, 0);

  name_copy(dir[slot].name, strm_name);
  dir[slot].flags = 0;
  dir[slot].parent = (uint16_t)strm_parent;
  dir[slot].size = strm_size;
  dir[slot].start = (strm_sectors > 0) ? strm_start : 0;
  dir[slot].mtime = pack_now();
  if (!overwrite)
    entry_set_owner(slot, 0666);

  if (overwrite)
    free_extent(old_start, old_size);
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

  int zeros_ok = (fs_read(fn, buf, SZ) == (int)SZ);
  for (uint32_t i = 0; i < SZ && zeros_ok; i++)
    if (buf[i] != 0)
      zeros_ok = 0;
  con_write("  prealloc+zero ");
  con_write(zeros_ok ? "PASS\n" : "FAIL\n");

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

  /* Read the whole file back; patches must have landed and nothing else changed. */
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
  fs_enum_name(name, dir[idx].name, FS_NAME_LEN);
  if (size)
    *size = dir[idx].size;
  if (mtime)
    *mtime = dir[idx].mtime;
  return 1;
}

int fs_stat_ex(const char *path, struct fs_stat_info *st)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    int d;
    uint32_t sz;
    if (rvol_stat(mv, sub, &d, &sz) != 0)
      return -1;
    st->is_dir = d;
    st->size = sz;
    st->mtime = mnt_mtime(mv, sub);
    mnt_attr(mv, sub, d, &st->mode, &st->uid, &st->gid);
    return 0;
  }
  st->size = st->mtime = st->uid = st->gid = 0;
  st->is_dir = 1;
  st->mode = 0755;
  /* /media: the synthetic mount root is a directory even with no mounts. */
  if (is_media_root(path))
    return 0;
  if (!mounted)
    return -1;
  int slot = find_path((path && path[0]) ? path : "/");
  if (slot == FS_SLOT_ROOT)
    return 0;
  if (slot < 0)
    return -1;
  st->is_dir = (dir[slot].flags & FS_DIR_FLAG) ? 1 : 0;
  st->size = dir[slot].size;
  st->mtime = dir[slot].mtime;
  st->mode = dir[slot].mode;
  st->uid = dir[slot].uid;
  st->gid = dir[slot].gid;
  return 0;
}

int fs_stat(const char *path, int *is_dir, uint32_t *size, uint32_t *mtime)
{
  struct fs_stat_info st;
  if (fs_stat_ex(path, &st) != 0)
    return -1;
  if (is_dir)
    *is_dir = st.is_dir;
  if (size)
    *size = st.size;
  if (mtime)
    *mtime = st.mtime;
  return 0;
}

/* The owofs entry for `path`, or -1 for the root, a mount, the synthetic /media, or an absent path. */
static int owofs_slot(const char *path)
{
  const char *sub;
  if (!mounted || mount_for(path, &sub) || is_media_root(path))
    return -1;
  int slot = find_path(path);
  return slot >= 0 ? slot : -1;
}

int fs_chmod(const char *path, uint32_t mode)
{
  int slot = owofs_slot(path);
  if (slot < 0)
    return -1;
  dir[slot].mode = (uint16_t)(mode & PERM_MODE_MASK);
  return dir_store() == 0 ? 0 : -1;
}

int fs_chown(const char *path, uint32_t uid, uint32_t gid)
{
  int slot = owofs_slot(path);
  if (slot < 0 || uid > 0xFFFFu || gid > 0xFFFFu)
    return -1;
  dir[slot].uid = (uint16_t)uid;
  dir[slot].gid = (uint16_t)gid;
  return dir_store() == 0 ? 0 : -1;
}

#define FS_ACC_PATH_MAX 512

/* fs_stat_ex without the size of generated /proc and /sys files (producing it can have side effects). */
static int access_stat(const char *path, struct fs_stat_info *st)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (!mv || (mv->ops != &procfs_ops && mv->ops != &sysfs_ops))
    return fs_stat_ex(path, st);
  int k = mv->ops == &procfs_ops ? procfs_kind(sub) : sysfs_kind(sub);
  if (k < 0)
    return -1;
  st->is_dir = k == 1;
  st->size = st->mtime = 0;
  mnt_attr(mv, sub, st->is_dir, &st->mode, &st->uid, &st->gid);
  return 0;
}

/* May the caller use directory `st` with `want`? */
static bool dir_allows(const struct fs_stat_info *st, uint32_t want)
{
  return perm_allows(st->mode, st->uid, st->gid, sched_uid(), sched_gid(), want, true);
}

int fs_access(const char *path, int want)
{
  char norm[FS_ACC_PATH_MAX];
  int len = 0, last_start = 0;
  if (!path)
    return -1;
  norm[0] = '\0';
  for (const char *p = path; *p;) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *e = p;
    while (*e && *e != '/')
      e++;
    int n = (int)(e - p);
    if (n == 1 && p[0] == '.') {
    } else if (n == 2 && p[0] == '.' && p[1] == '.') {
      len = last_start;
      norm[len] = '\0';
      last_start = 0;
      for (int i = 0; i < len; i++)
        if (norm[i] == '/')
          last_start = i;
    } else {
      if (len + 1 + n >= FS_ACC_PATH_MAX)
        return -1;
      last_start = len;
      norm[len++] = '/';
      for (int i = 0; i < n; i++)
        norm[len++] = p[i];
      norm[len] = '\0';
    }
    p = e;
  }

  struct fs_stat_info st;
  /* Every directory above the final component needs x; norm[last_start..] is the final component. */
  uint32_t parent_len = (uint32_t)last_start;
  struct fs_stat_info pst = {.is_dir = 1, .mode = 0755};
  for (uint32_t i = 1; i <= parent_len; i++) {
    if (norm[i] != '/')
      continue;
    char saved = norm[i];
    norm[i] = '\0';
    int rc = access_stat(norm, &pst);
    norm[i] = saved;
    if (rc != 0 || !pst.is_dir)
      return -1;
    if (!dir_allows(&pst, PERM_X))
      return -PERM_EACCES;
  }

  if (len == 0) {
    norm[0] = '/';
    norm[1] = '\0';
  }
  bool exists = access_stat(norm, &st) == 0;
  bool entry = (want & FS_ACC_ENTRY) != 0;
  if (!exists && !entry && !(want & FS_ACC_CREATE))
    return -1;
  if (exists && entry && len > 0 && !perm_sticky_allows(pst.mode, pst.uid, st.uid, sched_uid()))
    return -PERM_EACCES;
  if ((entry || !exists) && len > 0 && !dir_allows(&pst, PERM_W | PERM_X))
    return -PERM_EACCES;
  if (exists && !perm_allows(st.mode, st.uid, st.gid, sched_uid(), sched_gid(), (uint32_t)want & 7u, st.is_dir))
    return -PERM_EACCES;
  return 0;
}

int fs_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size, uint32_t *mtime)
{
  const char *sub;
  struct mnt *mv = mount_for(path, &sub);
  if (mv) {
    int d;
    uint32_t sz;
    if (!rvol_enum(mv, sub, idx, name, &d, &sz))
      return 0;
    if (is_dir)
      *is_dir = d;
    if (size)
      *size = sz;
    if (mtime) {
      char child[256];
      uint32_t n = strlcpy(child, sub, sizeof child);
      if (n > 0 && child[n - 1] != '/' && n < sizeof child - 1)
        child[n++] = '/';
      strlcpy(child + n, name, sizeof child - n);
      *mtime = mnt_mtime(mv, child);
    }
    return 1;
  }
  if (idx < 0)
    return 0;
  /* /media: the idx-th active removable mount's leaf name (a directory). */
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
    return 0;

  int count = 0;
  for (uint32_t i = 0; i < dir_cap; i++) {
    if (!dir[i].name[0] || dir[i].parent != parent || shadowed_by_mount(i))
      continue;
    if (count == idx) {
      fs_enum_name(name, dir[i].name, FS_NAME_LEN);
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
  /* Synthetic top-level dirs: /media, then each permanent pseudo-fs mount, from the mount table. */
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

/* Validate that point is "/media/<name>" with one component after "/media/". 0, or -1. */
static int valid_media_point(const char *point)
{
  const char *m = "/media/";
  const char *p = point;
  while (*m && *p == *m) {
    p++;
    m++;
  }
  if (*m != '\0')
    return -1;
  if (*p == '\0')
    return -1;
  for (; *p; p++)
    if (*p == '/')
      return -1;
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

  int slot = -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used) {
      if (s_eq(mtab[i].point, point))
        return -1;
    } else if (slot < 0) {
      slot = i;
    }
  }
  if (slot < 0)
    return -1;

  blk_read_fn r = NULL;
  blk_write_fn w = NULL;
  int iso = 0;
  if (blk_dev_io(dev, &r, &w, &iso) != 0)
    return -1;

  const struct fs_ops *ops = NULL;
  void *inst = NULL;
  if (iso) {
    ops = iso_ops();
    inst = ops ? ops->probe(r, w) : NULL;
    if (!inst)
      return -1;
  } else {
    const struct fs_ops *cand[] = {
        fat_ops(), ext_ops(), btrfs_ops(), ntfs_ops(), hfs_ops(),
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
      return -1;
  }

  mtab[slot].used = 1;
  mtab[slot].permanent = false;
  str_copy(mtab[slot].point, point, (int)sizeof mtab[slot].point);
  str_copy(mtab[slot].dev, dev, (int)sizeof mtab[slot].dev);
  mtab[slot].ops = ops;
  mtab[slot].inst = inst;
  mtab[slot].dir_mode = ops->create ? 0777 : 0555;
  mtab[slot].file_mode = ops->create ? 0777 : 0444;
  return 0;
}

int fs_unmount(const char *point)
{
  if (!point)
    return -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mtab[i].used && s_eq(mtab[i].point, point)) {
      if (mtab[i].permanent)
        return -1;
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
  return -1;
}

int fs_mount_enum(int idx, struct fs_mountinfo *out)
{
  if (idx < 0 || !out)
    return 0;
  int count = 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mtab[i].used || mtab[i].permanent)
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
  return 0;
}
