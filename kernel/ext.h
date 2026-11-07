/* ext.h - a read-only ext2/3/4 filesystem driver.
 *
 * The Linux-world member of the interchange family (fat.c = Windows/SD,
 * hfs.c = Mac, this = Linux). Read-only by design: ext4 writes mean journal
 * (jbd2) replay, block/inode-bitmap allocation, extent-tree insertion, and
 * metadata checksums -- exactly where a bug corrupts a real disk. Reading
 * covers "get my files off it" and is safe to point at a live removable volume.
 *
 * ext4 is little-endian. A Superblock at byte 1024 describes block size, the
 * inode layout, and feature flags; block-group descriptors locate each group's
 * inode table; files map logical->physical blocks via an extent tree (ext4) or
 * classic indirect blocks (ext2/3); directories are files full of linked
 * dir_entry_2 records. Root is inode 2. Names are case-sensitive.
 *
 * Multi-instance: every per-mount field lives in a heap-allocated instance the
 * driver returns from probe() and frees in close(), so N ext volumes (even two
 * of the same type) can mount at once. See fsmount.h for the vtable.
 */
#ifndef OWOS_EXT_H
#define OWOS_EXT_H

#include "fsmount.h"
#include <stdint.h>

const struct fs_ops *ext_ops(void);

/* Negative return values of the instance API are -EXT_ERR_*. */
enum ext_err {
  EXT_ERR_IO = 1,
  EXT_ERR_CORRUPT,
  EXT_ERR_NOENT,
  EXT_ERR_NOTDIR,
  EXT_ERR_ISDIR,
  EXT_ERR_LOOP,
  EXT_ERR_NAMETOOLONG,
  EXT_ERR_ENCRYPTED,
  EXT_ERR_NOTSUP, /* device node, FIFO or socket */
  EXT_ERR_NOMEM,
  EXT_ERR_INVAL,
};

struct ext_attr {
  uint32_t ino;
  uint32_t mode; /* full st_mode including the type bits */
  uint32_t uid, gid, nlink;
  uint32_t flags; /* raw i_flags (0x800 = encrypted) */
  uint64_t size;
  uint64_t blocks512;
  int64_t atime, mtime, ctime, crtime; /* crtime is 0 when the inode has no creation time */
  uint32_t atime_ns, mtime_ns, ctime_ns, crtime_ns;
};

/* Stat `path` (symlinks followed). 0 or -EXT_ERR_*. */
int ext_getinfo(void *inst, const char *path, struct ext_attr *out);

/* Read up to len bytes at a 64-bit offset. Returns bytes read (0 at EOF) or -EXT_ERR_*. */
int ext_read64(void *inst, const char *path, uint64_t off, void *buf, uint32_t len);

#endif /* OWOS_EXT_H */
