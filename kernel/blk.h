/* blk.h — block-device selection for owofs.
 *
 * owofs (fs.c) is a flat 512-byte-sector filesystem. It doesn't care which
 * controller backs it, only that it can read and write LBAs. This layer picks
 * one backing device at boot — the first present of IDE -> AHCI -> NVMe — and
 * routes fs.c's sector I/O to it. That lets owofs mount on a QEMU IDE disk, a
 * VMware NVMe disk, or a real laptop's SATA/NVMe drive without fs.c knowing the
 * difference.
 *
 * The three drivers (ata.c, ahci.c, nvme.c) already expose an identical
 * read_sector/write_sector/present/sectors/model API; this just dispatches.
 */
#ifndef OWOS_BLK_H
#define OWOS_BLK_H

#include "fsmount.h" /* blk_read_fn / blk_write_fn */
#include <stdint.h>

/* Choose the backing device. Call AFTER ahci_init()/nvme_init() (so their
 * _present() is valid) and BEFORE fs_init(). */
void blk_init(void);

int blk_present(void);         /* 1 if a backing device was selected     */
const char *blk_backend(void); /* "IDE" / "AHCI" / "NVMe" / "tmpfs"/"none"*/

/* Switch the active backend to a volatile RAM disk (tmpfs). fs_init() calls
 * this when there is no usable owofs disk -- no controller at all, or an
 * AHCI/NVMe disk it must not auto-format (the user's OS drive) -- so the system
 * still has a working, non-persistent filesystem instead of none. The RAM disk
 * is allocated from physical memory above the identity map on first use.
 * Returns 0 on success, -1 if the allocation failed. */
int blk_use_ram(void);

/* A present removable block device, for the /dev listing and the Files app's
 * Devices list. blk.c no longer mounts or knows filesystems -- fs.c owns the
 * mount table and probes a device via fs_ops. */
struct blk_devinfo {
  const char *dev;   /* /dev name: "cd0"/"usb0"/"ide0"/"sata0"/"nvme0"/"sd0" */
  const char *kind;  /* "CD"/"USB"/"IDE"/"SATA"/"NVMe"/"SD"                */
  const char *model; /* drive model string                                */
  int iso;           /* reads 2048-byte ATAPI sectors (a CD/DVD)          */
};

/* Enumerate present removable devices WITHOUT mounting any -- only hardware
 * presence is checked, never a filesystem. Fills out[0..max), returns the
 * count. */
int blk_dev_list(struct blk_devinfo *out, int max);

/* Resolve a /dev name to its sector I/O callbacks (and the 2048-byte CD flag),
 * so fs.c can probe and mount it. Returns 0, or -1 if no present removable
 * device has that name. `*w` is left NULL for a read-only medium. */
int blk_dev_io(const char *dev, blk_read_fn *r, blk_write_fn *w, int *iso);

/* True only for the IDE disk. fs_init() auto-creates owofs on a blank IDE disk
 * (the dev/QEMU disk) but never on AHCI/NVMe, which on real hardware is the
 * user's OS drive — formatting that automatically would destroy their data. */
int blk_is_ide(void);

int blk_read_sector(uint32_t lba, void *buf);
/* Read `count` contiguous sectors at once. */
int blk_read(uint32_t lba, void *buf, uint32_t count);
int blk_write_sector(uint32_t lba, const void *buf);
int blk_write(uint32_t lba, const void *buf, uint32_t count);
uint32_t blk_sectors(void);  /* capacity in 512-byte sectors (0 if none)*/
const char *blk_model(void); /* backing drive's model string            */

#endif /* OWOS_BLK_H */
