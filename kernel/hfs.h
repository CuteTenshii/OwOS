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
 * Multi-instance: all per-mount state lives in a heap-allocated `struct
 * hfs_mount`; the driver is reached only through the `struct fs_ops` vtable
 * (probe allocates an instance, close frees it), so N volumes can mount at once.
 */
#ifndef OWOS_HFS_H
#define OWOS_HFS_H

#include "fsmount.h"

const struct fs_ops *hfs_ops(void);

#endif /* OWOS_HFS_H */
