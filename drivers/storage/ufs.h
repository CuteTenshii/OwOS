/* ufs.h: Universal Flash Storage disk over a UFSHCI host controller on PCI (class 01h, subclass 09h).
 *
 * Brings the link up, initialises the device (fDeviceInit) and uses the first direct-access logical unit among
 * LUNs 0..7 through SCSI commands carried in UPIUs. LUs with 512- or 4096-byte logical blocks are accepted; either way
 * the disk is presented as 512-byte sectors. Same contract as ata.c/ahci.c/nvme.c/scsi.c. */
#ifndef OWOS_UFS_H
#define OWOS_UFS_H

#include <stdint.h>

/* Probe PCI for a UFS host controller and bring up its first disk LU. Prints nothing when there is no controller. */
void ufs_init(void);

int ufs_present(void);
uint32_t ufs_sectors(void); /* capacity in 512-byte sectors (0 if none) */
const char *ufs_model(void);
uint8_t ufs_lun(void);

/* Return 0 on success, -1 on error/timeout. buf holds 512 * count bytes; count is at most 8. */
int ufs_read_sector(uint32_t lba, void *buf);
int ufs_write_sector(uint32_t lba, const void *buf);
int ufs_read(uint32_t lba, void *buf, uint32_t count);
int ufs_write(uint32_t lba, const void *buf, uint32_t count);

#endif /* OWOS_UFS_H */
