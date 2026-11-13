/* ata.c: primary-master ATA disk access, polled (no IRQ14).
 *
 * Reads use PIO. Writes go through the IDE controller's bus-master DMA when
 * there is one, because KVM traps every word of a `rep outsw` (about 3 ms per
 * sector), and fall back to PIO otherwise. Every wait loop is bounded so a
 * missing or wedged drive returns an error instead of hanging the kernel.
 */

#include "ata.h"
#include "console.h"
#include "io.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "spinlock.h"

#define ATA_DATA 0x1F0
#define ATA_SECCNT 0x1F2
#define ATA_LBA0 0x1F3
#define ATA_LBA1 0x1F4
#define ATA_LBA2 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7 /* read: status; write: command                  */
#define ATA_CTRL 0x3F6   /* alternate status (no side effects on read)     */

#define ST_BSY 0x80
#define ST_DRQ 0x08
#define ST_ERR 0x01

#define ST_DF 0x20

#define CMD_READ 0x20
#define CMD_WRITE 0x30
#define CMD_WRITE_DMA 0xCA
#define CMD_FLUSH 0xE7

#define POLL_LIMIT 1000000u
#define DMA_POLL_LIMIT (16 * POLL_LIMIT)

/* Bus-master IDE registers for the primary channel, relative to BAR4. */
#define BM_CMD 0
#define BM_STATUS 2
#define BM_PRDT 4
#define BM_START 0x01
#define BM_ST_ACTIVE 0x01
#define BM_ST_ERR 0x02
#define BM_ST_IRQ 0x04

#define PRD_EOT 0x8000
#define DMA_FRAME 4096u
/* One LBA28 command moves at most 256 sectors, so 32 frames hold any single transfer. */
#define DMA_FRAMES (256u * 512u / DMA_FRAME)

struct prd {
  uint32_t addr;
  uint16_t bytes;
  uint16_t flags;
};

static spinlock_t io_lock;
static uint16_t bm_base; /* 0 while writes use PIO */
static volatile struct prd *prdt;
static uint8_t *dma_frame[DMA_FRAMES];

/* Reading the alternate status 4x gives the drive the ~400ns it needs after
 * a command before its status is meaningful. */
static void ata_delay(void)
{
  for (int i = 0; i < 4; i++)
    (void)inb(ATA_CTRL);
}

static int wait_not_busy(void)
{
  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if (!(inb(ATA_STATUS) & ST_BSY))
      return 0;
  return -1;
}

/* Wait for the drive to be ready to transfer a sector (DRQ set, no error). */
static int wait_drq(void)
{
  ata_delay();
  if (wait_not_busy())
    return -1;
  for (uint32_t i = 0; i < POLL_LIMIT; i++) {
    uint8_t s = inb(ATA_STATUS);
    if (s & ST_ERR)
      return -1;
    if (s & ST_DRQ)
      return 0;
  }
  return -1;
}

/* Start a transfer of `count` sectors (1..256) at `lba`. The SECCNT register is
 * 8-bit and reads 0 as 256, so (uint8_t)256 == 0 is exactly right for a
 * full-block transfer. */
static int issue(uint32_t lba, uint8_t cmd, uint32_t count)
{
  if (wait_not_busy())
    return -1;
  outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F)); /* master, LBA mode        */
  outb(ATA_SECCNT, (uint8_t)count);
  outb(ATA_LBA0, lba & 0xFF);
  outb(ATA_LBA1, (lba >> 8) & 0xFF);
  outb(ATA_LBA2, (lba >> 16) & 0xFF);
  outb(ATA_STATUS, cmd);
  return 0;
}

/* Read `count` sectors into buf. One READ SECTORS command covers up to 256
 * sectors (the LBA28 limit); the drive raises DRQ once per sector, and each is
 * drained with a single `rep insw`. Batching this way turns the old
 * 256-inw-per-sector loop (a VM-exit per word) into one bulk transfer per
 * sector and amortizes the command setup across the whole run. */
int ata_read(uint32_t lba, void *buf, uint32_t count)
{
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  uint8_t *p = buf;
  while (count > 0) {
    uint32_t n = count > 256 ? 256 : count;
    if (issue(lba, CMD_READ, n))
      goto fail;
    for (uint32_t s = 0; s < n; s++) {
      if (wait_drq())
        goto fail;
      insw(ATA_DATA, p, 256);
      p += 512;
    }
    lba += n;
    count -= n;
  }
  spin_unlock_irqrestore(&io_lock, flags);
  return 0;
fail:
  spin_unlock_irqrestore(&io_lock, flags);
  return -1;
}

int ata_read_sector(uint32_t lba, void *buf) { return ata_read(lba, buf, 1); }

/* Write n (1..256) sectors with one WRITE DMA command, staged through the identity-mapped frames. */
static int dma_write(uint32_t lba, const uint8_t *p, uint32_t n)
{
  uint32_t bytes = n * 512u, i = 0;
  for (uint32_t off = 0; off < bytes; off += DMA_FRAME, i++) {
    uint32_t len = bytes - off < DMA_FRAME ? bytes - off : DMA_FRAME;
    memcpy(dma_frame[i], p + off, len);
    prdt[i].addr = (uint32_t)(uintptr_t)dma_frame[i];
    prdt[i].bytes = (uint16_t)len;
    prdt[i].flags = 0;
  }
  prdt[i - 1].flags = PRD_EOT;

  outb((uint16_t)(bm_base + BM_CMD), 0); /* stopped, and direction = memory to disk */
  outb((uint16_t)(bm_base + BM_STATUS), BM_ST_ERR | BM_ST_IRQ);
  outl((uint16_t)(bm_base + BM_PRDT), (uint32_t)(uintptr_t)prdt);
  __asm__ volatile("" ::: "memory");
  if (issue(lba, CMD_WRITE_DMA, n))
    return -1;
  outb((uint16_t)(bm_base + BM_CMD), BM_START);

  uint8_t bm = 0;
  uint32_t tries = 0;
  for (; tries < DMA_POLL_LIMIT; tries++) {
    bm = inb((uint16_t)(bm_base + BM_STATUS));
    bool settled = (bm & (BM_ST_IRQ | BM_ST_ERR)) || !(bm & BM_ST_ACTIVE);
    if (settled && !(inb(ATA_CTRL) & ST_BSY))
      break;
  }
  outb((uint16_t)(bm_base + BM_CMD), 0);
  uint8_t st = inb(ATA_STATUS); /* the plain status read also acknowledges the drive's interrupt */
  outb((uint16_t)(bm_base + BM_STATUS), BM_ST_ERR | BM_ST_IRQ);
  if (tries == DMA_POLL_LIMIT || (bm & BM_ST_ERR) || (st & (ST_ERR | ST_DF)))
    return -1;
  return 0;
}

static int pio_write(uint32_t lba, const uint8_t *p, uint32_t n)
{
  if (issue(lba, CMD_WRITE, n))
    return -1;
  for (uint32_t s = 0; s < n; s++) {
    if (wait_drq())
      return -1;
    outsw(ATA_DATA, p, 256);
    p += 512;
  }
  return 0;
}

int ata_write(uint32_t lba, const void *buf, uint32_t count)
{
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  const uint8_t *p = buf;
  while (count > 0) {
    uint32_t n = count > 256 ? 256 : count;
    if (bm_base && dma_write(lba, p, n) != 0) {
      /* A drive or controller that rejects DMA keeps working over PIO. */
      bm_base = 0;
      con_write("ata: DMA write failed, falling back to PIO\n");
      if (wait_not_busy())
        goto fail;
    }
    if (!bm_base && pio_write(lba, p, n) != 0)
      goto fail;
    p += n * 512u;
    outb(ATA_STATUS, CMD_FLUSH);
    if (wait_not_busy())
      goto fail;
    lba += n;
    count -= n;
  }
  spin_unlock_irqrestore(&io_lock, flags);
  return 0;
fail:
  spin_unlock_irqrestore(&io_lock, flags);
  return -1;
}

int ata_write_sector(uint32_t lba, const void *buf) { return ata_write(lba, buf, 1); }

#define CMD_IDENTIFY 0xEC

/* Run IDENTIFY DEVICE on the primary master into a 256-word buffer. Returns 0
 * and fills words[] on success, -1 if no drive responds. */
static int identify(uint16_t *words)
{
  /* Floating-bus check. With no IDE controller decoding these ports (e.g. an
   * AHCI/NVMe-only laptop), reads return 0xFF. Without this we'd mistake the
   * stuck-high 0xFF (BSY set) for a perpetually-busy drive and spin the full
   * POLL_LIMIT on every probe — ~5s of dead port reads before concluding "no
   * drive". Bail immediately instead. */
  if (inb(ATA_STATUS) == 0xFF)
    return -1;

  if (wait_not_busy())
    return -1;
  outb(ATA_DRIVE, 0xA0); /* select master                    */
  outb(ATA_SECCNT, 0);
  outb(ATA_LBA0, 0);
  outb(ATA_LBA1, 0);
  outb(ATA_LBA2, 0);
  outb(ATA_STATUS, CMD_IDENTIFY);

  if (inb(ATA_STATUS) == 0) /* status 0 = no drive present      */
    return -1;
  if (wait_drq())
    return -1;
  for (int i = 0; i < 256; i++)
    words[i] = inw(ATA_DATA);
  return 0;
}

int ata_present(void)
{
  uint16_t words[256];
  return identify(words) == 0;
}

void ata_dma_init(void)
{
  struct pci_dev_info d;
  int idx = 0;
  while (pci_dev_info(idx, &d) && !(d.class_code == 0x01 && d.subclass == 0x01))
    idx++;
  if (!pci_dev_info(idx, &d))
    return;
  /* Prog-if bit 7: bus-master capable; bit 0: primary channel in native mode, off the legacy ports this driver uses. */
  uint8_t progif = (uint8_t)(pci_read32(d.bus, d.slot, d.func, 0x08) >> 8);
  uint32_t bar4 = pci_read32(d.bus, d.slot, d.func, 0x20);
  if (!(progif & 0x80) || (progif & 0x01) || !(bar4 & 1) || !(bar4 & 0xFFFCu))
    return;

  prdt = palloc();
  if (!prdt)
    return;
  for (uint32_t i = 0; i < DMA_FRAMES; i++) {
    dma_frame[i] = palloc();
    if (!dma_frame[i])
      return;
  }

  uint32_t cmd = pci_read32(d.bus, d.slot, d.func, 0x04);
  pci_write32(d.bus, d.slot, d.func, 0x04, cmd | 0x5u); /* I/O space + bus master */
  bm_base = (uint16_t)(bar4 & 0xFFFCu);
  con_printf("ata: bus-master DMA writes at io 0x%x\n", (uint32_t)bm_base);
}

uint32_t ata_sectors(void)
{
  uint16_t words[256];
  if (identify(words))
    return 0;
  /* Words 60-61: total addressable sectors in LBA28 mode. */
  return (uint32_t)words[60] | ((uint32_t)words[61] << 16);
}

/* ATA IDENTIFY strings are stored big-endian within each 16-bit word: the high
 * byte is the first character. Copy `count` words out, swapping, then trim
 * trailing spaces. */
static void ata_extract_string(char *dst, const uint16_t *words, int base, int count)
{
  int n = 0;
  for (int i = 0; i < count; i++) {
    dst[n++] = (char)(words[base + i] >> 8);
    dst[n++] = (char)(words[base + i] & 0xFF);
  }
  while (n > 0 && dst[n - 1] == ' ')
    n--;
  dst[n] = '\0';
}

const char *ata_model(void)
{
  static char model[41];
  uint16_t words[256];
  if (identify(words)) {
    model[0] = '\0';
    return model;
  }
  ata_extract_string(model, words, 27, 20); /* words 27-46: model number    */
  return model;
}
