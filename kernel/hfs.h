/* hfs.h — a read-only HFS+ (Apple "Mac OS Extended") filesystem driver.
 *
 * The companion to fat.c on the interchange path: where FAT carries files from
 * the Windows/SD world, HFS+ carries them from the Mac world (a USB stick or SD
 * card formatted on macOS). Read-only by design -- HFS+ writes mean journal
 * replay, B-tree insertion, and allocation-bitmap maintenance, which is exactly
 * where a bug corrupts a real disk; reading covers the "get my files off it"
 * case and is safe to point at a live removable volume.
 *
 * HFS+ is big-endian, with a Volume Header at byte 1024 and a Catalog B-tree
 * (keyed by parent-CNID + name) describing the folder tree; file data lives in
 * fork extents of allocation blocks. This driver parses the volume header,
 * walks the catalog leaf chain, resolves paths, and reads files via their data
 * fork's (up to 8 inline) extents.
 *
 * Device-agnostic, like fat.c: a volume is read through a sector-read callback
 * plus a base LBA, so the same code reads a whole disk or a partition.
 */
#ifndef OWOS_HFS_H
#define OWOS_HFS_H

#include <stdint.h>

/* Read one 512-byte sector at absolute LBA into buf; 0 on success. */
typedef int (*hfs_read_fn)(uint32_t lba, void *buf);

/* Mount the HFS+ volume whose Volume Header sits at byte 1024 of `base_lba`
 * (i.e. sector base_lba+2), reading through `read`. Returns 0 on success, -1 on
 * failure (bad signature, unsupported geometry, I/O error). */
int hfs_mount(hfs_read_fn read, uint32_t base_lba);

/* Probe one block device for an HFS+ volume: whole-disk first, then each GPT
 * and APM partition. Returns 0 if one was mounted, -1 otherwise. */
int hfs_mount_probe(hfs_read_fn read);

void hfs_unmount(void);
int  hfs_mounted(void);
const char *hfs_label(void);       /* volume name, or "" if none */
const char *hfs_type(void);        /* "HFS+" / "none" */

/* List directory `path` (NULL/""/"/" = root) to the console. Returns 0, -1 if
 * absent or not a directory, -2 if nothing is mounted. */
int hfs_list(const char *path);

/* Stat `path`: fill is_dir and size if non-NULL. 0 on success, -1 absent, -2 none. */
int hfs_stat(const char *path, int *is_dir, uint32_t *size);

/* Read up to `len` bytes from byte `offset` of file `path`. Returns bytes read
 * (0 at EOF), -1 if absent/a directory, -2 if nothing is mounted. */
int hfs_read_at(const char *path, uint32_t offset, void *buf, uint32_t len);
int hfs_read(const char *path, void *buf, uint32_t max);

/* Enumerate children of directory `path` (NULL/""/"/" = root). For the idx-th
 * child copy its name into name[] (>= 256 bytes) and fill is_dir and size; return
 * 1, or 0 past the end / not a directory / nothing mounted. */
int hfs_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size);

#endif /* OWOS_HFS_H */
