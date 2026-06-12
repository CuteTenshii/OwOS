/* ext.h — a read-only ext2/3/4 filesystem driver.
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
 * Device-agnostic, like fat.c/hfs.c: a sector-read callback plus a base LBA.
 */
#ifndef OWOS_EXT_H
#define OWOS_EXT_H

#include <stdint.h>

/* Read one 512-byte sector at absolute LBA into buf; 0 on success. */
typedef int (*ext_read_fn)(uint32_t lba, void *buf);

/* Mount the ext volume whose Superblock is at byte 1024 of `base_lba`. Returns
 * 0 on success, -1 on failure (bad magic, unsupported geometry/feature, I/O). */
int ext_mount(ext_read_fn read, uint32_t base_lba);

/* Probe one block device: whole-disk first, then each MBR/GPT partition. */
int ext_mount_probe(ext_read_fn read);

void ext_unmount(void);
int  ext_mounted(void);
const char *ext_label(void);       /* volume label, or "" */
const char *ext_type(void);        /* "ext4"/"ext3"/"ext2"/"none" */

/* List directory `path` (NULL/""/"/" = root) to the console. 0, -1 absent/not a
 * dir, -2 nothing mounted. */
int ext_list(const char *path);

/* Stat `path`: fill is_dir and size if non-NULL. 0 / -1 absent / -2 none. */
int ext_stat(const char *path, int *is_dir, uint32_t *size);

/* Read up to `len` bytes at byte `offset` of file `path`. Returns bytes read
 * (0 at EOF), -1 absent/a directory, -2 nothing mounted. */
int ext_read_at(const char *path, uint32_t offset, void *buf, uint32_t len);
int ext_read(const char *path, void *buf, uint32_t max);

/* Enumerate children of directory `path`. For the idx-th child copy its name
 * into name[] (>= 256 bytes) and fill is_dir and size; return 1, else 0. */
int ext_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size);

#endif /* OWOS_EXT_H */
