/* blk.c: block-device selection for owofs (see blk.h).
 * Picks the first present backend in the order IDE, AHCI, NVMe and forwards sector access to it. */

#include "blk.h"
#include "ahci.h"
#include "ata.h"
#include "atapi.h"
#include "console.h"
#include "fsmount.h"
#include "kstring.h"
#include "nvme.h"
#include "paging.h"
#include "part.h"
#include "pmm.h"
#include "scsi.h"
#include "sd.h"
#include "spinlock.h"
#include "ufs.h"
#include "xhci.h"

enum backend { BK_NONE, BK_IDE, BK_AHCI, BK_NVME, BK_SD, BK_RAM };
static enum backend backend;
/* The OwOS root partition on the backing disk; win_count 0 means the whole disk. */
static uint32_t win_start, win_count;

/* RAM disk for live boots, allocated above the identity map on first use. */
static volatile uint8_t *ram_disk;
static uint32_t ram_sectors;

#define BLK_CACHE_LINES 32
struct cache_line {
  uint32_t lba;
  bool valid;
  uint8_t data[512];
};
static struct cache_line cache[BLK_CACHE_LINES];
static spinlock_t blk_lock;

static void cache_clear(void)
{
  for (int i = 0; i < BLK_CACHE_LINES; i++)
    cache[i].valid = false;
}

static struct cache_line *cache_slot(uint32_t lba) { return &cache[lba % BLK_CACHE_LINES]; }

static bool blk_range_valid(uint32_t lba, uint32_t count)
{
  uint32_t sectors = blk_sectors();
  return count != 0 && lba < sectors && count <= sectors - lba;
}

static bool cache_read(uint32_t lba, void *buf)
{
  struct cache_line *c = cache_slot(lba);
  if (!c->valid || c->lba != lba)
    return false;
  memcpy(buf, c->data, sizeof c->data);
  return true;
}

static void cache_fill(uint32_t lba, const void *buf)
{
  struct cache_line *c = cache_slot(lba);
  c->lba = lba;
  memcpy(c->data, buf, sizeof c->data);
  c->valid = true;
}

static void cache_invalidate(uint32_t lba, uint32_t count)
{
  for (int i = 0; i < BLK_CACHE_LINES; i++) {
    struct cache_line *c = &cache[i];
    if (c->valid && c->lba >= lba && c->lba - lba < count)
      c->valid = false;
  }
}

/* Disks that can hold the OwOS root, in the order they are searched. */
static const struct {
  enum backend id;
  int (*present)(void);
  part_read_fn read;
} root_disks[] = {
    {BK_IDE, ata_present, ata_read_sector},
    {BK_AHCI, ahci_present, ahci_read_sector},
    {BK_NVME, nvme_present, nvme_read_sector},
};
#define NROOT (int)(sizeof root_disks / sizeof root_disks[0])

void blk_init(void)
{
  cache_clear();
  if (ata_present())
    ata_dma_init();

  /* An installed system lives in an OwOS root partition. Otherwise the first disk backs owofs whole, as older
   * installs did; fs_init runs live from RAM if it holds no owofs. */
  backend = BK_NONE;
  for (int i = 0; i < NROOT && backend == BK_NONE; i++)
    if (root_disks[i].present() && part_find(root_disks[i].read, PART_TYPE_OWOS_ROOT, &win_start, &win_count) == 0)
      backend = root_disks[i].id;
  for (int i = 0; i < NROOT && backend == BK_NONE; i++)
    if (root_disks[i].present())
      backend = root_disks[i].id;

  con_write("blk: owofs backing device = ");
  con_write(blk_backend());
  if (backend != BK_NONE) {
    con_write(" (");
    con_write(blk_model());
    con_write(")");
    if (win_count)
      con_printf(", OwOS partition at sector %u, %u MiB", win_start, win_count / 2048);
  }
  con_putc('\n');
}

bool blk_present(void) { return backend != BK_NONE; }

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

int blk_use_ram(uint32_t sectors)
{
  if (!ram_disk) {
    uint32_t bytes = sectors * 512u;
    phys_t phys = pmm_alloc_contig(bytes);
    if (!phys)
      return -1;
    if (paging_identity_map((uint32_t)phys, bytes) != 0) {
      pmm_free_contig(phys, bytes);
      return -1;
    }
    ram_disk = (volatile uint8_t *)(uintptr_t)phys;
    ram_sectors = sectors;
    for (uint32_t i = 0; i < bytes; i++)
      ram_disk[i] = 0;
  }
  cache_clear();
  backend = BK_RAM;
  win_start = win_count = 0;
  return 0;
}

/* Removable devices (the /dev layer): enumerate present devices except the disk owofs booted from
 * and hand out their sector callbacks for fs.c to mount. CDs read 2048-byte ATAPI sectors. */
struct cand {
  const char *dev; /* /dev name: "cd0","usb0","ide0",...        */
  const char *kind;
  int (*present)(void);
  const char *(*model)(void);
  enum backend id;
  int iso; /* reads 2048-byte ATAPI sectors             */
  blk_read_fn r;
  blk_write_fn w;
  uint32_t (*sectors)(void);
};

static const struct cand cands[] = {
    {"cd0", "CD", atapi_present, atapi_model, BK_NONE, 1, atapi_read_sector, 0, 0},
    {"usb0", "USB", xhci_msc_present, xhci_msc_model, BK_NONE, 0, xhci_msc_read, xhci_msc_write, xhci_msc_sectors},
    {"ide0", "IDE", ata_present, ata_model, BK_IDE, 0, ata_read_sector, ata_write_sector, ata_sectors},
    {"sata0", "SATA", ahci_present, ahci_model, BK_AHCI, 0, ahci_read_sector, ahci_write_sector, ahci_sectors},
    {"nvme0", "NVMe", nvme_present, nvme_model, BK_NVME, 0, nvme_read_sector, nvme_write_sector, nvme_sectors},
    {"sd0", "SD", sd_present, sd_model, BK_SD, 0, sd_read_sector, sd_write_sector, sd_sectors},
    {"scsi0", "SCSI", scsi_present, scsi_model, BK_NONE, 0, scsi_read_sector, scsi_write_sector, scsi_sectors},
    {"ufs0", "UFS", ufs_present, ufs_model, BK_NONE, 0, ufs_read_sector, ufs_write_sector, ufs_sectors},
};
#define NCAND (int)(sizeof(cands) / sizeof(cands[0]))

/* Present, and not the disk owofs itself booted from. */
static bool cand_listable(const struct cand *c) { return c->present() && c->id != backend; }

/* Enumerate present removable devices (the /dev contents and the Files app list). */
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
    out[n].sectors = cands[i].sectors ? cands[i].sectors() : 0;
    n++;
  }
  return n;
}

/* Resolve a /dev name to its sector callbacks and CD flag. 0, or -1 if absent; *w is NULL if read-only. */
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

static int blk_read_sector_locked(uint32_t lba, void *buf)
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

int blk_read_sector(uint32_t lba, void *buf)
{
  uintptr_t flags = spin_lock_irqsave(&blk_lock);
  if (!blk_range_valid(lba, 1)) {
    spin_unlock_irqrestore(&blk_lock, flags);
    return -1;
  }
  lba += win_start;
  int rc = blk_read_sector_locked(lba, buf);
  spin_unlock_irqrestore(&blk_lock, flags);
  return rc;
}

int blk_read(uint32_t lba, void *buf, uint32_t count)
{
  uintptr_t flags;
  int rc = 0;
  uint32_t requested = count;
  if (count == 0)
    return 0;
  flags = spin_lock_irqsave(&blk_lock);
  if (!blk_range_valid(lba, count)) {
    spin_unlock_irqrestore(&blk_lock, flags);
    return -1;
  }
  lba += win_start;
  /* Keep requests within the one-page DMA buffers used by AHCI and NVMe. */
  if (backend == BK_IDE) {
    rc = ata_read(lba, buf, count);
    if (rc == 0 && requested <= BLK_CACHE_LINES) {
      uint8_t *p = buf;
      for (uint32_t i = 0; i < requested; i++, p += 512)
        cache_fill(lba + i, p);
    }
  } else if (backend == BK_AHCI) {
    uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (ahci_read(lba, p, n) != 0)
        goto fail;
      if (requested <= BLK_CACHE_LINES)
        for (uint32_t i = 0; i < n; i++)
          cache_fill(lba + i, p + i * 512);
      lba += n;
      p += n * 512;
      count -= n;
    }
  } else if (backend == BK_NVME) {
    uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (nvme_read(lba, p, n) != 0)
        goto fail;
      if (requested <= BLK_CACHE_LINES)
        for (uint32_t i = 0; i < n; i++)
          cache_fill(lba + i, p + i * 512);
      lba += n;
      p += n * 512;
      count -= n;
    }
  } else {
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, p += 512)
      if (blk_read_sector_locked(lba + i, p) != 0)
        goto fail;
  }
  spin_unlock_irqrestore(&blk_lock, flags);
  return rc;
fail:
  spin_unlock_irqrestore(&blk_lock, flags);
  return -1;
}

static int blk_write_sector_locked(uint32_t lba, const void *buf)
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

int blk_write_sector(uint32_t lba, const void *buf)
{
  uintptr_t flags = spin_lock_irqsave(&blk_lock);
  if (!blk_range_valid(lba, 1)) {
    spin_unlock_irqrestore(&blk_lock, flags);
    return -1;
  }
  lba += win_start;
  int rc = blk_write_sector_locked(lba, buf);
  spin_unlock_irqrestore(&blk_lock, flags);
  return rc;
}

int blk_write(uint32_t lba, const void *buf, uint32_t count)
{
  uintptr_t flags;
  int rc = 0;
  uint32_t requested = count;
  const uint8_t *start_buf = buf;
  if (count == 0)
    return 0;
  flags = spin_lock_irqsave(&blk_lock);
  if (!blk_range_valid(lba, count)) {
    spin_unlock_irqrestore(&blk_lock, flags);
    return -1;
  }
  lba += win_start;
  uint32_t start_lba = lba;
  cache_invalidate(lba, count);
  if (backend == BK_IDE) {
    rc = ata_write(lba, buf, count);
  } else if (backend == BK_AHCI) {
    const uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (ahci_write(lba, p, n) != 0)
        goto fail;
      lba += n;
      p += n * 512;
      count -= n;
    }
  } else if (backend == BK_NVME) {
    const uint8_t *p = buf;
    while (count) {
      uint32_t n = count > 8 ? 8 : count;
      if (nvme_write(lba, p, n) != 0)
        goto fail;
      lba += n;
      p += n * 512;
      count -= n;
    }
  } else {
    for (uint32_t i = 0; i < count; i++, lba++, buf = (const uint8_t *)buf + 512)
      if (blk_write_sector_locked(lba, buf) != 0)
        goto fail;
  }
  if (rc == 0 && requested <= BLK_CACHE_LINES) { /* cache only what reached the disk */
    const uint8_t *p = start_buf;
    for (uint32_t i = 0; i < requested; i++, p += 512)
      cache_fill(start_lba + i, p);
  }
  spin_unlock_irqrestore(&blk_lock, flags);
  return rc;
fail:
  spin_unlock_irqrestore(&blk_lock, flags);
  return -1;
}

static uint32_t device_sectors(void)
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

uint32_t blk_sectors(void) { return win_count ? win_count : device_sectors(); }

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
