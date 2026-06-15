/* fat.h — a FAT12/16/32 filesystem driver, as a multi-instance fs_ops.
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

const struct fs_ops *fat_ops(void);

#endif /* OWOS_FAT_H */
