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
#include "xhci.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
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

/* What kind of device currently backs the removable volume mounted at /media, for the
 * UI to label it (e.g. "USB" in the file explorer's Devices list). Set by
 * blk_mount_fat; meaningful only while a FAT volume is mounted. */
static const char *fat_src = "";

const char *blk_fat_source(void)
{
	return fat_src;
}

/* Announce whichever removable volume just mounted (FAT via fat.c or HFS+ via
 * hfs.c) and remember its backing device for the UI's Devices list. */
static void announce_vol(const char *source)
{
	fat_src = source;
	int ro = hfs_mounted() || ext_mounted();
	const char *type  = ext_mounted() ? ext_type()  : hfs_mounted() ? hfs_type()  : fat_type();
	const char *label = ext_mounted() ? ext_label() : hfs_mounted() ? hfs_label() : fat_label();
	vga_write("vol: mounted ");
	vga_write(type);
	if (label[0]) {
		vga_write(" \""); vga_write(label); vga_write("\"");
	}
	vga_write(" on "); vga_write(source);
	vga_write(ro ? " (ro)\n" : fat_writable() ? " (rw)\n" : " (ro)\n");
}

/* Try every removable FS we support on one device: FAT, then HFS+, then ext. */
static int mount_removable(const char *name, fat_read_fn r, fat_write_fn w)
{
	if (fat_mount_probe(r, w) == 0) { announce_vol(name); return 0; }
	if (hfs_mount_probe(r) == 0)    { announce_vol(name); return 0; }
	if (ext_mount_probe(r) == 0)    { announce_vol(name); return 0; }
	return -1;
}

int blk_mount_fat(void)
{
	/* Mount a removable volume on the first present device that owofs is NOT
	 * using as its own backing store. owofs lives on one disk (blk_init's pick);
	 * a stick/SD/second image lives on another. This is where device knowledge
	 * belongs -- fat.c/hfs.c stay controller-agnostic. */

	/* A USB mass-storage device is the most natural target -- a real removable
	 * stick -- so try it first. It only becomes present after xhci_boot()
	 * enumerates it, which is why this runs again post-boot. */
	if (xhci_msc_present()) {
		/* USB MSC has no write hook wired here; FAT writes still work because
		 * xhci_msc_write is passed. */
		if (mount_removable("USB", xhci_msc_read, xhci_msc_write) == 0)
			return 0;
	}

	struct { int present; enum backend id; const char *name;
	         fat_read_fn r; fat_write_fn w; } dev[] = {
		{ ata_present(),  BK_IDE,  "IDE",  ata_read_sector,  ata_write_sector  },
		{ ahci_present(), BK_AHCI, "SATA", ahci_read_sector, ahci_write_sector },
		{ nvme_present(), BK_NVME, "NVMe", nvme_read_sector, nvme_write_sector },
	};
	for (unsigned i = 0; i < sizeof(dev) / sizeof(dev[0]); i++) {
		if (!dev[i].present || dev[i].id == backend)
			continue;                  /* skip absent disks and owofs's own */
		if (mount_removable(dev[i].name, dev[i].r, dev[i].w) == 0)
			return 0;
	}
	vga_write("vol: no FAT/HFS+/ext volume found\n");
	return -1;
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

int blk_read(uint32_t lba, void *buf, uint32_t count)
{
	if (count == 0)
		return 0;
	/* IDE gets the batched READ SECTORS path; the others have no multi-sector
	 * primitive here, so fan out to their per-sector read (RAM is a memcpy,
	 * AHCI/NVMe already issue one fast command each). */
	if (backend == BK_IDE)
		return ata_read(lba, buf, count);
	uint8_t *p = buf;
	for (uint32_t i = 0; i < count; i++, p += 512)
		if (blk_read_sector(lba + i, p) != 0)
			return -1;
	return 0;
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
