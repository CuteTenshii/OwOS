/* ntfs.h — a read-only NTFS filesystem driver.
 *
 * The Windows-NT member of the interchange family (fat.c = Windows/SD,
 * hfs.c = Mac, ext.c = Linux, this = NTFS). Read-only by design: NTFS writes
 * mean $LogFile journalling, $Bitmap allocation, B-tree index splits and run
 * fixups -- exactly where a bug corrupts a real disk. Reading covers "get my
 * files off it" and is safe to point at a live removable volume.
 *
 * Little-endian throughout. The boot sector (LBA 0) gives bytes/sector,
 * sectors/cluster, the $MFT LCN and the MFT record size. The MFT is an array
 * of "FILE" records (each protected by a fixup/update-sequence array); a record
 * is a list of attributes. $FILE_NAME (0x30) holds names + parent; $DATA (0x80)
 * holds file contents (resident inline, or non-resident via a runlist of
 * (length, LCN-delta) pairs); $INDEX_ROOT (0x90) + $INDEX_ALLOCATION (0xA0)
 * hold directory B-tree entries. Root is record 5, $Volume is record 3.
 *
 * Multi-instance via the `struct fs_ops` vtable (fsmount.h): each mount is a
 * heap-allocated instance the driver allocates in probe() and frees in close().
 * The backing `blk_read_fn` reads 512-byte sectors.
 */
#ifndef OWOS_NTFS_H
#define OWOS_NTFS_H

#include "fsmount.h"

const struct fs_ops *ntfs_ops(void);

#endif /* OWOS_NTFS_H */
