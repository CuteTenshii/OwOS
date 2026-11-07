/* btrfs.h: a read-only btrfs filesystem driver.
 *
 * Covered: superblock with crc32c verification (primary at 64 KiB, mirrors at 64 MiB and 256 GiB when the primary
 * is bad), the system chunk array and chunk tree (SINGLE, DUP, RAID0, RAID1, RAID1C3/C4, RAID10 read from the one
 * device we have), B-tree nodes of any height with an LRU node cache, directories by DIR_INDEX/DIR_ITEM, inline,
 * regular and preallocated extents, holes, zlib/LZO/zstd compression, symlinks (resolved during lookup, depth 40,
 * confined to the volume) and subvolumes (shown as directories; '..' leaves them through ROOT_BACKREF). The default
 * subvolume is the one mounted.
 *
 * Refused: RAID5/6, ZONED, extent tree v2, raid stripe tree and unknown incompat feature bits. Multi-device
 * filesystems mount, but reads that need a stripe on a device we do not have fail with an error. Metadata checksum
 * mismatches are logged once and the data is still used; data checksums are not verified. Checksum types other
 * than crc32c are accepted without verification. Extended attributes, ACLs, quotas and send/receive are ignored.
 *
 * Multi-instance: every per-mount field lives in a heap instance returned by probe() and freed by close().
 * fs_ops carries 32-bit sizes and offsets, so the calls below expose the full 64-bit view.
 */
#ifndef OWOS_BTRFS_H
#define OWOS_BTRFS_H

#include "fsmount.h"
#include <stdint.h>

struct btrfs_attr {
  uint32_t mode; /* full st_mode including the type bits */
  uint32_t uid, gid, nlink;
  uint64_t size;
  int64_t atime, mtime, ctime, otime;
  uint32_t atime_ns, mtime_ns, ctime_ns, otime_ns;
};

/* Stat `path` (symlinks followed). 0 or -1. */
int btrfs_getinfo(void *inst, const char *path, struct btrfs_attr *out);

/* Read up to len bytes at a 64-bit offset. Returns bytes read (0 at EOF) or -1. */
int btrfs_read64(void *inst, const char *path, uint64_t off, void *buf, uint32_t len);

#endif /* OWOS_BTRFS_H */
