/* fat.h - a FAT12/16/32 filesystem driver, as a multi-instance fs_ops.
 *
 * owofs (fs.c) is OwOS's own filesystem and backs the boot disk. This driver
 * is the *interchange* path: it reads (and writes) the FAT volumes the rest of
 * the world uses -- a USB stick, an SD card, a disk image from a host -- so
 * files can be carried in and out of OwOS without owofs on both ends.
 *
 * The driver auto-detects FAT12 vs FAT16 vs FAT32 from the BPB, follows cluster
 * chains, reassembles VFAT long filenames on read, and writes 8.3 short names.
 *
 * Multi-instance: all per-mount state lives in a heap-allocated instance that
 * `probe()` allocates and `close()` frees, so N FAT volumes can be mounted at
 * once. The driver exposes a single `struct fs_ops` (see fsmount.h); fs.c owns
 * the mount table and routes paths to the owning instance.
 */
#ifndef OWOS_FAT_H
#define OWOS_FAT_H

#include "fsmount.h"
#include <stdbool.h>
#include <stdint.h>

const struct fs_ops *fat_ops(void);

/* Negative error codes returned by the instance API and fs_ops hooks. */
enum fat_err {
  FAT_OK = 0,
  FAT_ENOENT = -1,
  FAT_EROFS = -2, /* read-only mount, or no valid volume */
  FAT_ENOTEMPTY = -3,
  FAT_EIO = -4,
  FAT_ENOSPC = -5,
  FAT_EINVAL = -6, /* illegal or over-long name, or an impossible request */
  FAT_EEXIST = -7,
  FAT_EISDIR = -8,
  FAT_ENOTDIR = -9,
  FAT_EACCES = -10, /* read-only attribute */
  FAT_EFBIG = -11,  /* file would exceed 4 GiB - 1 */
  FAT_ENOMEM = -12,
};

#define FAT_ATTR_RO 0x01u
#define FAT_ATTR_HIDDEN 0x02u
#define FAT_ATTR_SYSTEM 0x04u
#define FAT_ATTR_DIR 0x10u
#define FAT_ATTR_ARCHIVE 0x20u

/* Names enum_dir may write: callers pass 64 bytes, longer ones become the 8.3 alias. */
#define FAT_ENUM_NAME_MAX 64

struct fat_stat {
  uint32_t size;
  uint32_t cluster;
  uint8_t attr;
  bool is_dir;
  uint32_t ctime, mtime, atime; /* Unix seconds, FAT local time as UTC; atime has date resolution */
};

struct fat_statfs {
  uint32_t cluster_bytes;
  uint32_t total_clusters;
  uint32_t free_clusters;
  int bits; /* 12, 16 or 32 */
};

int fat_getattr(void *inst, const char *path, struct fat_stat *st);
/* Set and clear read-only/hidden/system/archive bits (other bits are ignored). */
int fat_setattr(void *inst, const char *path, uint8_t set, uint8_t clear);
/* Move or rename, across directories too; replaces an existing file at `to`. */
int fat_rename(void *inst, const char *from, const char *to);
/* Shrink (freeing clusters) or grow (zero filled) an existing file. */
int fat_truncate(void *inst, const char *path, uint32_t size);
/* Write at any offset, extending the file (a gap reads as zeros). Returns bytes written. */
int fat_write_at(void *inst, const char *path, uint32_t off, const void *buf, uint32_t len);
int fat_statfs(void *inst, struct fat_statfs *out);
int fat_enum_dir_cap(void *inst, const char *path, int idx, char *name, uint32_t cap, int *is_dir, uint32_t *size);

struct fat_format_opts {
  blk_write_fn write; /* required: 512-byte sector writer */
  uint32_t base_lba;  /* device LBA of the volume's first sector */
  uint32_t sectors;   /* volume length in 512-byte sectors */
  int kind;           /* 0 = choose by size, else 12, 16 or 32 */
  uint32_t bytes_per_sec;
  uint32_t sec_per_clus; /* 0 = choose by size */
  const char *label;     /* up to 11 ASCII characters, or NULL */
  uint32_t serial;       /* 0 = derive from the clock */
};

/* Write an empty FAT volume. 0 or negative fat_err (FAT_EINVAL if size can't hold kind). */
int fat_format(const struct fat_format_opts *o);

#endif /* OWOS_FAT_H */
