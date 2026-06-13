/* atapi.h — polled ATAPI (CD/DVD) PIO driver.
 *
 * A CD/DVD drive is an ATAPI device: SCSI commands tunnelled over the IDE/ATA
 * bus via the PACKET (0xA0) command, with 2048-byte logical sectors rather than
 * the 512-byte sectors plain ATA disks use (so it can't share ata.c). This finds
 * the first CD/DVD drive on either IDE channel (primary 0x1F0 / secondary 0x170,
 * master or slave) and reads it a logical sector at a time. Read-only: optical
 * media here is read-only and the kernel never burns.
 *
 * iso9660.c sits on top of this; the ATAPI block reader is its sector source.
 */
#ifndef OWOS_ATAPI_H
#define OWOS_ATAPI_H

#include <stdint.h>

#define ATAPI_SECTOR_SIZE 2048u

/* Probe both IDE channels for a CD/DVD drive and latch the first one found.
 * Safe to call when no IDE controller is present (floating-bus check bails). */
void        atapi_init(void);

/* Non-zero if a CD/DVD drive was found by atapi_init(). A drive can be present
 * with no medium loaded — that's still "present"; reads just fail. */
int         atapi_present(void);

/* Capacity of the loaded medium in 2048-byte logical blocks (0 if no drive or
 * no medium / not ready). Re-runs READ CAPACITY so a freshly inserted disc is
 * seen. */
uint32_t    atapi_sectors(void);

/* Drive model string ("" if no drive). Points at a static buffer. */
const char *atapi_model(void);

/* Read one 2048-byte logical block at `lba` into buf (must hold 2048 bytes).
 * Returns 0 on success, -1 on error / no medium / no drive. */
int         atapi_read_sector(uint32_t lba, void *buf);

#endif /* OWOS_ATAPI_H */
