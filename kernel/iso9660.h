/* iso9660.h — a read-only ISO 9660 (CD/DVD) filesystem driver.
 *
 * The optical-disc member of the interchange family (fat.c = Windows/SD,
 * hfs.c = Mac, ext.c = Linux, this = CD/DVD). ISO 9660 is inherently read-only
 * here — the medium is, and the kernel never burns.
 *
 * Layout: 2048-byte logical sectors. A chain of Volume Descriptors begins at
 * sector 16; the Primary Volume Descriptor (type 1, magic "CD001") names the
 * volume and points at the root directory record. Directories are runs of
 * sectors packed with variable-length records (extent LBA + length + name).
 * Most discs also carry a Joliet Supplementary Volume Descriptor (type 2) whose
 * names are UCS-2 — long, mixed-case — which this prefers when present so files
 * don't show up as SHOUTING 8.3 names with a ";1" version suffix.
 *
 * Multi-instance via the `struct fs_ops` vtable (fsmount.h): each mount is a
 * heap-allocated instance the driver allocates in probe() and frees in close().
 * Device-agnostic — but its sector unit is the 2048-byte logical block, so the
 * `blk_read_fn` passed to probe() reads one of those (typically atapi_read_
 * sector), not a 512-byte sector.
 */
#ifndef OWOS_ISO9660_H
#define OWOS_ISO9660_H

#include "fsmount.h"

const struct fs_ops *iso_ops(void);

#endif /* OWOS_ISO9660_H */
