/* blk.h: block-device selection for owofs.
 * Picks one backing device at boot (first present of IDE, AHCI, NVMe) and routes fs.c's sector I/O to it. */
#ifndef OWOS_BLK_H
#define OWOS_BLK_H

#include "fsmount.h" /* blk_read_fn / blk_write_fn */
#include <stdbool.h>
#include <stdint.h>

/* Choose the backing device. Call after ahci_init()/nvme_init() and before fs_init(). */
void blk_init(void);

bool blk_present(void);
const char *blk_backend(void); /* "IDE" / "AHCI" / "NVMe" / "tmpfs"/"none"*/

/* Switch to a volatile RAM disk of `sectors` 512-byte sectors (live boot). 0, or -1 if that much contiguous RAM
 * is not free; the size is fixed by the first successful call. */
int blk_use_ram(uint32_t sectors);

/* A present removable block device, for the /dev listing and the Files app Devices list. */
struct blk_devinfo {
  const char *dev; /* /dev name: "cd0"/"usb0"/"ide0"/"sata0"/"nvme0"/"sd0"/"scsi0"/"ufs0" */
  const char *kind;
  const char *model;
  int iso; /* reads 2048-byte ATAPI sectors (a CD/DVD)          */
};

/* Enumerate present removable devices without mounting any. Fills out[0..max), returns the count. */
int blk_dev_list(struct blk_devinfo *out, int max);

/* Resolve a /dev name to its sector callbacks and CD flag. 0, or -1 if absent; *w is NULL if read-only. */
int blk_dev_io(const char *dev, blk_read_fn *r, blk_write_fn *w, int *iso);

int blk_read_sector(uint32_t lba, void *buf);
/* Read `count` contiguous sectors at once. */
int blk_read(uint32_t lba, void *buf, uint32_t count);
int blk_write_sector(uint32_t lba, const void *buf);
int blk_write(uint32_t lba, const void *buf, uint32_t count);
uint32_t blk_sectors(void);  /* capacity in 512-byte sectors (0 if none)*/
const char *blk_model(void); /* backing drive's model string            */

#endif /* OWOS_BLK_H */
