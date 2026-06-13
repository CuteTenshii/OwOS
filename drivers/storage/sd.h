/* sd.h — SD/MMC card driver over a PCI SDHCI host controller, polled PIO.
 *
 * SDHCI (SD Host Controller Interface) is the standard way an SD/MMC card slot
 * is exposed to software: a PCI device (class 0x08 / subclass 0x05) with an MMIO
 * register block through which the host issues SD commands and streams block
 * data. QEMU models one as `-device sdhci-pci` carrying an `-device sd-card`.
 *
 * This driver brings up the first SDHCI controller with a card inserted, runs
 * the SD initialisation handshake (CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7), and
 * reads/writes one 512-byte block at a time by polling — the same
 * read/write/present/sectors/model contract as ata.c/ahci.c/nvme.c, so blk.c can
 * treat an SD card like any other block device.
 */
#ifndef OWOS_SD_H
#define OWOS_SD_H

#include <stdint.h>

/* Probe PCI for an SDHCI controller and initialise an inserted card. A no-op
 * (prints a line) when no controller or no card is present, so the boot path is
 * identical whether or not a card slot exists. */
void sd_init(void);

/* Non-zero once a card has been brought up. */
int sd_present(void);

/* Capacity of the card in 512-byte sectors (0 if none). */
uint32_t sd_sectors(void);

/* Human-readable card description (empty if none). */
const char *sd_model(void);

/* Read/write one 512-byte block at the given LBA. Return 0 on success or -1 on
 * error/timeout. buf must hold 512 bytes. */
int sd_read_sector(uint32_t lba, void *buf);
int sd_write_sector(uint32_t lba, const void *buf);

#endif /* OWOS_SD_H */
