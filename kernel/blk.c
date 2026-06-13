/* blk.c — block-device selection for owofs (see blk.h).
 *
 * Picks the first present backend in the order IDE -> AHCI -> NVMe and forwards
 * every sector access to it. The order keeps the QEMU dev setup unchanged (its
 * -hda IDE disk wins, with SATA/NVMe scratch disks ignored) while letting owofs
 * fall through to AHCI/NVMe on machines that have no IDE controller at all
 * (modern laptops, a VMware VM with only an NVMe disk).
 */

#include "blk.h"
#include "kstring.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "sd.h"
#include "xhci.h"
#include "atapi.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
#include "iso9660.h"
#include "pmm.h"
#include "paging.h"
#include "console.h"

enum backend { BK_NONE, BK_IDE, BK_AHCI, BK_NVME, BK_SD, BK_RAM };
static enum backend backend;

/* RAM disk (tmpfs). Sized to match owofs's 16 MiB sector space so its bump
 * allocator can never run off the end. Allocated above the identity map on
 * first fallback and identity-mapped so the kernel can address it directly. */
#define RAMDISK_SECTORS  32768u            /* 16 MiB / 512                       */
static volatile uint8_t *ram_disk;
static uint32_t          ram_sectors;

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

	con_write("blk: owofs backing device = ");
	con_write(blk_backend());
	if (backend != BK_NONE) {
		con_write(" (");
		con_write(blk_model());
		con_write(")");
	}
	con_putc('\n');
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
	int ro = hfs_mounted() || ext_mounted() || iso_mounted();
	const char *type = "none", *label = "";
	if (iso_mounted())      { type = iso_type(); label = iso_label(); }
	else if (ext_mounted()) { type = ext_type(); label = ext_label(); }
	else if (hfs_mounted()) { type = hfs_type(); label = hfs_label(); }
	else                    { type = fat_type(); label = fat_label(); }
	con_write("vol: mounted ");
	con_write(type);
	if (label[0]) {
		con_write(" \""); con_write(label); con_write("\"");
	}
	con_write(" on "); con_write(source);
	con_write(ro ? " (ro)\n" : fat_writable() ? " (rw)\n" : " (ro)\n");
}

/* Try every removable FS we support on one device: FAT, then HFS+, then ext. */
static int mount_removable(const char *name, fat_read_fn r, fat_write_fn w)
{
	if (fat_mount_probe(r, w) == 0) { announce_vol(name); return 0; }
	if (hfs_mount_probe(r) == 0)    { announce_vol(name); return 0; }
	if (ext_mount_probe(r) == 0)    { announce_vol(name); return 0; }
	return -1;
}

/* One row per removable-volume source we can mount at /media. The order is the
 * mount priority used at boot (CD/USB first -- the user's most likely intent --
 * then secondary disks). A CD/DVD is special: read-only ISO 9660 over the
 * 2048-byte ATAPI transport, so it can't ride the 512-byte fat/hfs/ext probe
 * path and carries an `iso` flag. `id` is the owofs backend a device would be,
 * so we can skip whichever disk owofs itself booted from. */
struct cand {
	const char  *name;             /* UI label + mount key ("USB","CD",...)    */
	int        (*present)(void);   /* hardware presence -- cheap, no sector I/O */
	const char *(*model)(void);    /* drive model string for the UI            */
	enum backend id;
	int          iso;
	fat_read_fn  r;
	fat_write_fn w;
};

static const struct cand cands[] = {
	{ "CD",   atapi_present,    atapi_model,    BK_NONE, 1, atapi_read_sector, 0                 },
	{ "USB",  xhci_msc_present, xhci_msc_model, BK_NONE, 0, xhci_msc_read,     xhci_msc_write    },
	{ "IDE",  ata_present,      ata_model,      BK_IDE,  0, ata_read_sector,   ata_write_sector  },
	{ "SATA", ahci_present,     ahci_model,     BK_AHCI, 0, ahci_read_sector,  ahci_write_sector },
	{ "NVMe", nvme_present,     nvme_model,     BK_NVME, 0, nvme_read_sector,  nvme_write_sector },
	{ "SD",   sd_present,       sd_model,       BK_SD,   0, sd_read_sector,    sd_write_sector   },
};
#define NCAND (int)(sizeof(cands) / sizeof(cands[0]))

/* A candidate is a listable removable device right now if its hardware is
 * present and it is NOT the disk owofs booted from (the system volume is not
 * offered as removable media). */
static int cand_listable(const struct cand *c)
{
	return c->present() && c->id != backend;
}

/* Mount one candidate at /media. Returns 0 on success. */
static int cand_mount(const struct cand *c)
{
	if (!cand_listable(c))
		return -1;
	if (c->iso) {
		if (iso_mount_probe(c->r) == 0) { announce_vol(c->name); return 0; }
		return -1;
	}
	return mount_removable(c->name, c->r, c->w);
}

int blk_mount_fat(void)
{
	/* Exactly one removable volume lives at /media. If something is already
	 * mounted, leave it -- a retry must not stack a second volume on top. */
	if (fat_mounted() || hfs_mounted() || ext_mounted() || iso_mounted())
		return 0;

	for (int i = 0; i < NCAND; i++)
		if (cand_mount(&cands[i]) == 0)
			return 0;
	con_write("vol: no removable volume found\n");
	return -1;
}

int blk_mount_device(const char *name)
{
	/* One volume at a time: if /media is taken, only a no-op re-mount of the same
	 * device succeeds; mounting a different one requires ejecting first. */
	if (fat_mounted() || hfs_mounted() || ext_mounted() || iso_mounted())
		return streq(fat_src, name) ? 0 : -1;

	for (int i = 0; i < NCAND; i++)
		if (streq(cands[i].name, name))
			return cand_mount(&cands[i]);
	return -1;
}

int blk_device_list(struct blk_devinfo *out, int max)
{
	int any = fat_mounted() || hfs_mounted() || ext_mounted() || iso_mounted();
	int n = 0;
	for (int i = 0; i < NCAND && n < max; i++) {
		if (!cand_listable(&cands[i]))
			continue;
		out[n].name    = cands[i].name;
		out[n].model   = cands[i].model();
		out[n].mounted = any && streq(fat_src, cands[i].name);
		n++;
	}
	return n;
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
		memcpy(buf, (const void *)(ram_disk + (uint32_t)lba * 512u), 512);
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
		memcpy((void *)(ram_disk + (uint32_t)lba * 512u), buf, 512);
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
