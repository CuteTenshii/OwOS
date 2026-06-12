/* ata.h — polled ATA (IDE) PIO driver for the primary master disk. */
#ifndef OWOS_ATA_H
#define OWOS_ATA_H

#include <stdint.h>

/* Read/write one 512-byte sector at the given LBA. Return 0 on success or
 * -1 on error/timeout (e.g. no disk attached). buf must hold 512 bytes. */
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

/* Read `count` contiguous sectors at `lba` into buf (count*512 bytes) using as
 * few READ SECTORS commands as the LBA28 256-sector limit allows. Same return
 * convention as ata_read_sector. */
int ata_read(uint32_t lba, void *buf, uint32_t count);

/* Non-zero if a drive answers IDENTIFY on the primary master. */
int ata_present(void);

/* LBA28 capacity of the primary master in 512-byte sectors (0 if none). */
uint32_t ata_sectors(void);

/* Model string reported by the drive (empty if none). Points at a static
 * buffer overwritten on each call. */
const char *ata_model(void);

#endif /* OWOS_ATA_H */
