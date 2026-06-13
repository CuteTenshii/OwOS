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
 * Device-agnostic, like fat.c/hfs.c/ext.c — but its sector unit is the 2048-byte
 * logical block, so its read callback reads one of those (typically
 * atapi_read_sector), not a 512-byte sector.
 */
#ifndef OWOS_ISO9660_H
#define OWOS_ISO9660_H

#include <stdint.h>

/* Read one 2048-byte logical sector at `lba` into buf; 0 on success. */
typedef int (*iso_read_fn)(uint32_t lba, void *buf);

/* Mount the ISO 9660 volume reachable through `read`. Returns 0 on success, -1
 * on failure (no "CD001" magic, I/O error, no medium). */
int iso_mount(iso_read_fn read);

/* Probe a device for an ISO 9660 volume and mount it if found (same as
 * iso_mount today; named for symmetry with the other fs_*_mount_probe). */
int iso_mount_probe(iso_read_fn read);

void iso_unmount(void);
int  iso_mounted(void);
const char *iso_label(void);       /* volume identifier, or "" */
const char *iso_type(void);        /* "ISO9660" / "ISO9660+Joliet" / "none" */

/* List directory `path` (NULL/""/"/" = root) to the console. 0, -1 absent/not a
 * dir, -2 nothing mounted. */
int iso_list(const char *path);

/* Stat `path`: fill is_dir and size if non-NULL. 0 / -1 absent / -2 none. */
int iso_stat(const char *path, int *is_dir, uint32_t *size);

/* Read up to `len` bytes at byte `offset` of file `path`. Returns bytes read
 * (0 at EOF), -1 absent/a directory, -2 nothing mounted. */
int iso_read_at(const char *path, uint32_t offset, void *buf, uint32_t len);

/* Enumerate children of directory `path`. For the idx-th child copy its name
 * into name[] (>= 256 bytes) and fill is_dir and size; return 1, else 0. */
int iso_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size);

#endif /* OWOS_ISO9660_H */
