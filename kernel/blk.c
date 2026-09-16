/* blk.c — block-device selection for owofs (see blk.h).
 *
 * Picks the first present backend in the order IDE -> AHCI -> NVMe and forwards
 * every sector access to it. The order keeps the QEMU dev setup unchanged (its
 * -hda IDE disk wins, with SATA/NVMe scratch disks ignored) while letting owofs
 * fall through to AHCI/NVMe on machines that have no IDE controller at all
 * (modern laptops, a VMware VM with only an NVMe disk).
 */

#include "blk.h"
#include "ahci.h"
#include "ata.h"
#include "atapi.h"
#include "console.h"
#include "fsmount.h"
#include "kstring.h"
#include "nvme.h"
#include "paging.h"
#include "pmm.h"
#include "sd.h"
#include "xhci.h"

enum backend { BK_NONE, BK_IDE, BK_AHCI, BK_NVME, BK_SD, BK_RAM };
static enum backend backend;

/* RAM disk (tmpfs). Sized to match owofs's 16 MiB sector space so its bump
 * allocator can never run off the end. Allocated above the identity map on
 * first fallback and identity-mapped so the kernel can address it directly. */
#define RAMDISK_SECTORS 32768u /* 16 MiB / 512                       */
static volatile uint8_t *ram_disk;
static uint32_t ram_sectors;

#define BLK_CACHE_LINES 32
struct cache_line {
  uint32_t lba;
  uint8_t valid;
  uint8_t data[512];
};
static struct cache_line cache[BLK_CACHE_LINES];

static void cache_clear(void)
{
  for (int i = 0; i < BLK_CACHE_LINES; i++)
    cache[i].valid = 0;
}

static struct cache_line *cache_slot(uint32_t lba) { return &cache[lba % BLK_CACHE_LINES]; }

static int cache_read(uint32_t lba, void *buf)
{
  struct cache_line *c = cache_slot(lba);
  if (!c->valid || c->lba != lba)
    return 0;
  memcpy(buf, c->data, sizeof c->data);
  return 1;
}

static void cache_fill(uint32_t lba, const void *buf)
{
  struct cache_line *c = cache_slot(lba);
  c->lba = lba;
  memcpy(c->data, buf, sizeof c->data);
  c->valid = 1;
}

static void cache_invalidate(uint32_t lba, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    struct cache_line *c = cache_slot(lba + i);
    if (c->valid && c->lba == lba + i)
      c->valid = 0;
  }
}

void blk_init(void)
{
  cache_clear();
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

int blk_present(void) { return backend != BK_NONE; }

const char *blk_backend(void)
{
  switch (backend) {
  case BK_IDE:
    return "IDE";
  case BK_AHCI:
    return "AHCI";
  case BK_NVME:
    return "NVMe";
  case BK_RAM:
    return "tmpfs";
  default:
    return "none";
  }
}

int blk_is_ide(void) { return backend == BK_IDE; }

int blk_use_ram(void)
{
  if (!ram_disk) {
    uint32_t bytes = RAMDISK_SECTORS * 512u;
    uint32_t phys = pmm_alloc_contig(bytes);
    if (!phys)
      return -1;
    if (paging_identity_map(phys, bytes) != 0)
      return -1;
    ram_disk = (volatile uint8_t *)phys;
    ram_sectors = RAMDISK_SECTORS;
    for (uint32_t i = 0; i < bytes; i++) /* start clean (unformatted)      */
      ram_disk[i] = 0;
  }
  cache_clear();
  backend = BK_RAM;
  return 0;
}

/* ---- removable devices: the /dev layer -------------------------------------
 *
 * blk.c no longer mounts or knows filesystems -- fs.c owns the mount table and
 * probes a device through the fs_ops drivers. Here we just enumerate the
 * present removable devices (everything except the disk owofs booted from)
 * under stable /dev names, and hand out each one's sector read/write callbacks
 * so fs.c can mount it. A CD/DVD reads 2048-byte ATAPI sectors (the `iso`
 * flag); the rest read 512-byte sectors. `id` is the owofs backend a device
 * would be, so we skip whichever disk owofs itself booted from. */
struct cand {
  const char *dev;            /* /dev name: "cd0","usb0","ide0",...        */
  const char *kind;           /* UI label:  "CD","USB","IDE",...           */
  int (*present)(void);       /* hardware presence -- cheap, no sector I/O */
  const char *(*model)(void); /* drive model string for the UI            */
  enum backend id;
  int iso; /* reads 2048-byte ATAPI sectors             */
  blk_read_fn r;
  blk_write_fn w;
};

static const struct cand cands[] = {
    {"cd0", "CD", atapi_present, atapi_model, BK_NONE, 1, atapi_read_sector, 0},
    {"usb0", "USB", xhci_msc_present, xhci_msc_model, BK_NONE, 0, xhci_msc_read, xhci_msc_write},
    {"ide0", "IDE", ata_present, ata_model, BK_IDE, 0, ata_read_sector, ata_write_sector},
    {"sata0", "SATA", ahci_present, ahci_model, BK_AHCI, 0, ahci_read_sector, ahci_write_sector},
    {"nvme0", "NVMe", nvme_present, nvme_model, BK_NVME, 0, nvme_read_sector, nvme_write_sector},
    {"sd0", "SD", sd_present, sd_model, BK_SD, 0, sd_read_sector, sd_write_sector},
};
#define NCAND (int)(sizeof(cands) / sizeof(cands[0]))

/* Present, and not the disk owofs itself booted from. */
static int cand_listable(const struct cand *c) { return c->present() && c->id != backend; }

/* Enumerate present removable devices (the /dev contents + the Files app list).
 */
int blk_dev_list(struct blk_devinfo *out, int max)
{
  int n = 0;
  for (int i = 0; i < NCAND && n < max; i++) {
    if (!cand_listable(&cands[i]))
      continue;
    out[n].dev = cands[i].dev;
    out[n].kind = cands[i].kind;
    out[n].model = cands[i].model();
    out[n].iso = cands[i].iso;
    n++;
  }
  return n;
}

/* Resolve a /dev name to its sector I/O callbacks (and 2048-byte CD flag), so
 * fs.c can probe + mount it. 0 on success, -1 if no present device has that
 * name. `w` is left NULL for a read-only medium (a CD). */
int blk_dev_io(const char *dev, blk_read_fn *r, blk_write_fn *w, int *iso)
{
  for (int i = 0; i < NCAND; i++) {
    if (cand_listable(&cands[i]) && streq(cands[i].dev, dev)) {
      if (r)
        *r = cands[i].r;
      if (w)
        *w = cands[i].w;
      if (iso)
        *iso = cands[i].iso;
      return 0;
    }
  }
  return -1;
}

int blk_read_sector(uint32_t lba, void *buf)
{
  if (cache_read(lba, buf))
    return 0;
  int rc;
  switch (backend) {
  case BK_IDE:
    rc = ata_read_sector(lba, buf);
    break;
  case BK_AHCI:
    rc = ahci_read_sector(lba, buf);
    break;
  case BK_NVME:
    rc = nvme_read_sector(lba, buf);
    break;
  case BK_RAM:
    if (lba >= ram_sectors)
      return -1;
    memcpy(buf, (const void *)(ram_disk + (uint32_t)lba * 512u), 512);
    rc = 0;
    break;
  default:
    return -1;
  }
  if (rc == 0)
    cache_fill(lba, buf);
  return rc;
}

int blk_read(uint32_t lba, void *buf, uint32_t count)
{
  if (count == 0)
    return 0;
  /* Keep requests within the one-page DMA buffers used by AHCI and NVMe. */
  if (backend == BK_IDE)
    return ata_read(lba, buf, count);
  if (backend == BK_AHCI) {
    uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (ahci_read(lba, p, n) != 0)
        return -1;
      lba += n;
      p += n * 512;
      count -= n;
    }
    return 0;
  }
  if (backend == BK_NVME) {
    uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (nvme_read(lba, p, n) != 0)
        return -1;
      lba += n;
      p += n * 512;
      count -= n;
    }
    return 0;
  }
  uint8_t *p = buf;
  for (uint32_t i = 0; i < count; i++, p += 512)
    if (blk_read_sector(lba + i, p) != 0)
      return -1;
  return 0;
}

int blk_write_sector(uint32_t lba, const void *buf)
{
  cache_invalidate(lba, 1);
  switch (backend) {
  case BK_IDE:
    return ata_write_sector(lba, buf);
  case BK_AHCI:
    return ahci_write_sector(lba, buf);
  case BK_NVME:
    return nvme_write_sector(lba, buf);
  case BK_RAM:
    if (lba >= ram_sectors)
      return -1;
    memcpy((void *)(ram_disk + (uint32_t)lba * 512u), buf, 512);
    return 0;
  default:
    return -1;
  }
}

int blk_write(uint32_t lba, const void *buf, uint32_t count)
{
  if (count == 0)
    return 0;
  cache_invalidate(lba, count);
  if (backend == BK_IDE) {
    for (uint32_t i = 0; i < count; i++)
      if (ata_write_sector(lba + i, (const uint8_t *)buf + i * 512) != 0)
        return -1;
    return 0;
  }
  if (backend == BK_AHCI) {
    const uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (ahci_write(lba, p, n) != 0)
        return -1;
      lba += n;
      p += n * 512;
      count -= n;
    }
    return 0;
  }
  if (backend == BK_NVME) {
    const uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (nvme_write(lba, p, n) != 0)
        return -1;
      lba += n;
      p += n * 512;
      count -= n;
    }
    return 0;
  }
  for (uint32_t i = 0; i < count; i++, lba++, buf = (const uint8_t *)buf + 512)
    if (blk_write_sector(lba, buf) != 0)
      return -1;
  return 0;
}

uint32_t blk_sectors(void)
{
  switch (backend) {
  case BK_IDE:
    return ata_sectors();
  case BK_AHCI:
    return ahci_sectors();
  case BK_NVME:
    return nvme_sectors();
  case BK_RAM:
    return ram_sectors;
  default:
    return 0;
  }
}

const char *blk_model(void)
{
  switch (backend) {
  case BK_IDE:
    return ata_model();
  case BK_AHCI:
    return ahci_model();
  case BK_NVME:
    return nvme_model();
  case BK_RAM:
    return "RAM disk";
  default:
    return "";
  }
}
