/* blk.c — block-device selection for owofs (see blk.h).
 *
 * Picks the first present backend in the order IDE -> AHCI -> NVMe and forwards
 * every sector access to it. The order keeps the QEMU dev setup unchanged (its
 * -hda IDE disk wins, with SATA/NVMe scratch disks ignored) while letting owofs
 * fall through to AHCI/NVMe on machines that have no IDE controller at all
 * (modern laptops, a VMware VM with only an NVMe disk).
 */

#include "blk.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"

enum backend { BK_NONE, BK_IDE, BK_AHCI, BK_NVME, BK_RAM };
static enum backend backend;

/* RAM disk (tmpfs). Sized to match owofs's 16 MiB sector space so its bump
 * allocator can never run off the end. Allocated above the identity map on
 * first fallback and identity-mapped so the kernel can address it directly. */
#define RAMDISK_SECTORS  32768u            /* 16 MiB / 512                       */
static volatile uint8_t *ram_disk;
static uint32_t          ram_sectors;

static void blk_memcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
}

void blk_init(void)
{
	if (ata_present())
		backend = BK_IDE;
	else if (ahci_present())
		backend = BK_AHCI;
	else if (nvme_present())
		backend = BK_NVME;
	else
		backend = BK_NONE;

	vga_write("blk: owofs backing device = ");
	vga_write(blk_backend());
	if (backend != BK_NONE) {
		vga_write(" (");
		vga_write(blk_model());
		vga_write(")");
	}
	vga_putc('\n');
}

int blk_present(void)
{
	return backend != BK_NONE;
}

const char *blk_backend(void)
{
	switch (backend) {
	case BK_IDE:  return "IDE";
	case BK_AHCI: return "AHCI";
	case BK_NVME: return "NVMe";
	case BK_RAM:  return "tmpfs";
	default:      return "none";
	}
}

int blk_is_ide(void)
{
	return backend == BK_IDE;
}

int blk_use_ram(void)
{
	if (!ram_disk) {
		uint32_t bytes = RAMDISK_SECTORS * 512u;
		uint32_t phys  = pmm_alloc_contig(bytes);
		if (!phys)
			return -1;
		if (paging_identity_map(phys, bytes) != 0)
			return -1;
		ram_disk = (volatile uint8_t *)phys;
		ram_sectors = RAMDISK_SECTORS;
		for (uint32_t i = 0; i < bytes; i++)   /* start clean (unformatted)      */
			ram_disk[i] = 0;
	}
	backend = BK_RAM;
	return 0;
}

int blk_read_sector(uint32_t lba, void *buf)
{
	switch (backend) {
	case BK_IDE:  return ata_read_sector(lba, buf);
	case BK_AHCI: return ahci_read_sector(lba, buf);
	case BK_NVME: return nvme_read_sector(lba, buf);
	case BK_RAM:
		if (lba >= ram_sectors)
			return -1;
		blk_memcpy(buf, (const void *)(ram_disk + (uint32_t)lba * 512u), 512);
		return 0;
	default:      return -1;
	}
}

int blk_write_sector(uint32_t lba, const void *buf)
{
	switch (backend) {
	case BK_IDE:  return ata_write_sector(lba, buf);
	case BK_AHCI: return ahci_write_sector(lba, buf);
	case BK_NVME: return nvme_write_sector(lba, buf);
	case BK_RAM:
		if (lba >= ram_sectors)
			return -1;
		blk_memcpy((void *)(ram_disk + (uint32_t)lba * 512u), buf, 512);
		return 0;
	default:      return -1;
	}
}

uint32_t blk_sectors(void)
{
	switch (backend) {
	case BK_IDE:  return ata_sectors();
	case BK_AHCI: return ahci_sectors();
	case BK_NVME: return nvme_sectors();
	case BK_RAM:  return ram_sectors;
	default:      return 0;
	}
}

const char *blk_model(void)
{
	switch (backend) {
	case BK_IDE:  return ata_model();
	case BK_AHCI: return ahci_model();
	case BK_NVME: return nvme_model();
	case BK_RAM:  return "RAM disk";
	default:      return "";
	}
}
