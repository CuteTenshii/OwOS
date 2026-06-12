/* fat.h — a read-only FAT12/16/32 filesystem driver.
 *
 * owofs (fs.c) is OwOS's own filesystem and backs the boot disk. This driver
 * is the *interchange* path: it reads the FAT volumes the rest of the world
 * uses -- a USB stick, an SD card, a disk image from a host -- so files can be
 * carried in and out of OwOS without owofs on both ends.
 *
 * It is deliberately read-only. FAT writes (cluster allocation, FAT-chain and
 * directory updates, dual-FAT mirroring) are where on-disk corruption hides;
 * reading is the 90% case for an interchange filesystem and is safe to point at
 * a real removable disk. The driver auto-detects FAT12 vs FAT16 vs FAT32 from
 * the BPB, follows cluster chains, and reassembles VFAT long filenames.
 *
 * Device-agnostic: a volume is mounted behind a sector-read callback plus a
 * base LBA, so the same code reads a whole unpartitioned disk (base 0) or a
 * partition inside an MBR (the partition's start LBA). fat_init() wires this up
 * to the AHCI/NVMe disks at boot; fat_mount() is the raw entry point.
 */
#ifndef OWOS_FAT_H
#define OWOS_FAT_H

#include <stdint.h>

/* A volume's sector source/sink: read or write one 512-byte sector at absolute
 * LBA. Returns 0 on success, non-zero on error. Same shape as ata/ahci/nvme. */
typedef int (*fat_read_fn)(uint32_t lba, void *buf);
typedef int (*fat_write_fn)(uint32_t lba, const void *buf);

/* Mount the FAT volume whose boot sector sits at absolute LBA `base_lba`,
 * doing I/O through `read` and `write`. Pass base_lba 0 for an unpartitioned
 * disk; pass write==NULL to mount strictly read-only (writes then fail). Only
 * 512-byte-sector volumes are supported. Replaces any currently-mounted volume.
 * Returns 0 on success, -1 on failure (bad BPB, unsupported geometry, I/O). */
int fat_mount(fat_read_fn read, fat_write_fn write, uint32_t base_lba);

/* Unmount the current volume ("eject"). All FAT writes are write-through, so
 * there is nothing buffered to lose -- this just drops the mount and the sector
 * cache so the medium can be safely removed. Always safe to call. */
void fat_unmount(void);

int fat_writable(void);            /* 1 if the mount was given a write fn        */

/* Probe one block device for a mountable FAT volume -- whole-disk first (BPB at
 * LBA 0), then each primary MBR partition -- and mount the first found through
 * the given callbacks. Device-agnostic: the caller (blk.c) supplies a specific
 * disk's read/write; this code never names a controller. Returns 0 on a mount,
 * -1 otherwise. */
int fat_mount_probe(fat_read_fn read, fat_write_fn write);

int         fat_mounted(void);     /* 1 if a volume is currently mounted        */
const char *fat_type(void);        /* "FAT12" / "FAT16" / "FAT32" / "none"      */
const char *fat_label(void);       /* volume label, or "" if none               */

/* List directory `path` (NULL / "" / "/" = root) to the console, one entry per
 * line with size and an attribute marker. Returns 0, -1 if the path is absent
 * or is a file (not a directory), -2 if nothing is mounted. */
int fat_list(const char *path);

/* Stat `path`. On success fills (if non-NULL) *is_dir and *size and returns 0;
 * returns -1 if the path does not exist, -2 if nothing is mounted. The root is
 * a directory of size 0. */
int fat_stat(const char *path, int *is_dir, uint32_t *size);

/* Read up to `len` bytes starting at byte `offset` of file `path` into buf
 * (streaming-friendly: walks the cluster chain to the offset). Returns bytes
 * read (0 at EOF), -1 if absent / a directory, -2 if nothing is mounted. */
int fat_read_at(const char *path, uint32_t offset, void *buf, uint32_t len);

/* Convenience whole-file read: up to `max` bytes from the start of `path`.
 * Returns bytes read, or the same negative codes as fat_read_at. */
int fat_read(const char *path, void *buf, uint32_t max);

/* Enumerate the children of directory `path` (NULL/""/"/" = root). For the
 * idx-th child (0-based, on-disk order, "." and ".." skipped) copy its name
 * into name[] (>= 256 bytes) and fill *is_dir and *size; return 1. Returns 0
 * past the last child, if `path` is not a directory, or if nothing is mounted.
 * Lets a directory browser walk the FAT tree without knowing the layout. */
int fat_enum_dir(const char *path, int idx, char *name, int *is_dir,
                 uint32_t *size);

/* -- Writes (write-through; require a writable mount) ----------------------- *
 * Names are stored as generated 8.3 short names (UPPERCASE, with ~N to keep
 * them unique). The driver reads VFAT long names but does not yet *create*
 * them, so a created file may appear under a shortened name. All return 0 on
 * success and a negative code on failure (-2 = nothing mounted / read-only). */

/* Create or overwrite file `path` with `size` bytes from `data`. */
int fat_create(const char *path, const void *data, uint32_t size);

/* Delete file `path` (frees its clusters and directory slot). -1 if absent,
 * -3 if it is a directory (use fat_rmdir). */
int fat_delete(const char *path);

/* Create directory `path` (with "." and ".." entries). -1 on failure. */
int fat_mkdir(const char *path);

/* Remove the *empty* directory `path`. -1 not found, -3 not empty. */
int fat_rmdir(const char *path);

#endif /* OWOS_FAT_H */
