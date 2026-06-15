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
 * Multi-instance: every per-mount field lives in a heap-allocated instance the
 * driver returns from probe() and frees in close(), so N ext volumes (even two
 * of the same type) can mount at once. See fsmount.h for the vtable.
 */
#ifndef OWOS_EXT_H
#define OWOS_EXT_H

#include "fsmount.h"

const struct fs_ops *ext_ops(void);

#endif /* OWOS_EXT_H */
