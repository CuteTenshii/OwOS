/* ahci.h — AHCI (SATA) disk driver: a single-port, DMA, polled reader/writer.
 *
 * AHCI is the standard way SATA controllers are exposed on PCI (class 0x01,
 * subclass 0x06). Unlike the legacy IDE/ATA path (ata.c, programmed I/O over
 * fixed ports), AHCI talks through an MMIO register block and moves data by
 * bus-master DMA into memory the driver hands it. This driver brings up the
 * first attached SATA drive and reads/writes one 512-byte sector at a time.
 */
#ifndef OWOS_AHCI_H
#define OWOS_AHCI_H

#include <stdint.h>

/* Probe PCI for an AHCI controller and initialise the first SATA drive on it.
 * A no-op (and prints a line) when no controller/drive is present, so the boot
 * path is identical whether or not QEMU was started with a SATA disk. */
void ahci_init(void);

/* Non-zero once a usable SATA drive has been brought up. */
int ahci_present(void);

/* 48-bit-LBA capacity of the drive in 512-byte sectors (0 if none). */
uint32_t ahci_sectors(void);

/* Model string reported by the drive (empty if none). */
const char *ahci_model(void);

/* Read/write one 512-byte sector at the given LBA. Return 0 on success or -1
 * on error/timeout (no drive, DMA error). buf must hold 512 bytes. */
int ahci_read_sector(uint32_t lba, void *buf);
int ahci_write_sector(uint32_t lba, const void *buf);
int ahci_read(uint32_t lba, void *buf, uint32_t count);
int ahci_write(uint32_t lba, const void *buf, uint32_t count);

#endif /* OWOS_AHCI_H */
