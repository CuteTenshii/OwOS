/* nvme.h — minimal NVMe (PCIe SSD) block driver.
 *
 * NVMe is the modern storage interface: instead of ATA task-file registers it
 * uses paired submission/completion queues in main memory and a small MMIO
 * register block with per-queue doorbells. This driver brings up the admin
 * queue, creates one I/O queue pair, identifies namespace 1, and reads/writes
 * one 512-byte block at a time by polling the completion queue.
 */
#ifndef OWOS_NVME_H
#define OWOS_NVME_H

#include <stdint.h>

/* Probe PCI for an NVMe controller and bring up namespace 1. A no-op (prints a
 * line) when none is present, so the boot path is unchanged without an SSD. */
void nvme_init(void);

/* Non-zero once a namespace has been brought up. */
int nvme_present(void);

/* Capacity of namespace 1 in 512-byte sectors (0 if none). */
uint32_t nvme_sectors(void);

/* Controller model string (empty if none). */
const char *nvme_model(void);

/* Read/write one 512-byte block at the given LBA. Return 0 on success or -1 on
 * error/timeout. buf must hold 512 bytes. */
int nvme_read_sector(uint32_t lba, void *buf);
int nvme_write_sector(uint32_t lba, const void *buf);

#endif /* OWOS_NVME_H */
