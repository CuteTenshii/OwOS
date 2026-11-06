/* ahci.c: polled single-port AHCI (SATA) driver, one sector at a time over DMA.
 * Per I/O: Reg H2D FIS (READ/WRITE DMA EXT), one PRDT entry to a bounce buffer, ring PxCI, poll. Waits are bounded. */

#include <stdbool.h>

#include "ahci.h"
#include "console.h"
#include "paging.h"
#include "pci.h"
#include "spinlock.h"

#define AHCI_CLASS 0x01
#define AHCI_SUBCLASS 0x06

/* Generic host control (byte offsets into ABAR). */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C

#define GHC_AE 0x80000000u

/* Per-port register block: PORT_BASE(n) + register offset. */
#define PORT_BASE(n) (0x100 + (n) * 0x80)
#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0C
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38

#define CMD_ST 0x0001
#define CMD_FRE 0x0010
#define CMD_FR 0x4000
#define CMD_CR 0x8000

#define TFD_BSY 0x80
#define TFD_DRQ 0x08
#define IS_TFES 0x40000000u

#define SIG_SATA 0x00000101u

#define ATA_READ_DMA_EX 0x25
#define ATA_WRITE_DMA_EX 0x35

#define POLL_LIMIT 5000000u

static volatile uint32_t *abar;
static bool found;
static int port;
static uint32_t cap_sectors;
static char model[41];

/* DMA areas, each a page-aligned identity-mapped frame (phys == virt). */
static volatile uint32_t *clb;
static volatile uint8_t *fis;
static volatile uint8_t *ctab;
static volatile uint8_t *dbuf;
static spinlock_t io_lock;

static inline uint32_t hba_rd(uint32_t off) { return abar[off / 4]; }
static inline void hba_wr(uint32_t off, uint32_t v) { abar[off / 4] = v; }

static inline uint32_t port_rd(uint32_t reg) { return abar[(PORT_BASE(port) + reg) / 4]; }
static inline void port_wr(uint32_t reg, uint32_t v) { abar[(PORT_BASE(port) + reg) / 4] = v; }

static void port_stop(void)
{
  uint32_t cmd = port_rd(PX_CMD);
  cmd &= ~(CMD_ST | CMD_FRE);
  port_wr(PX_CMD, cmd);
  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if (!(port_rd(PX_CMD) & (CMD_FR | CMD_CR)))
      break;
}

static void port_start(void)
{
  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if (!(port_rd(PX_CMD) & CMD_CR))
      break;
  uint32_t cmd = port_rd(PX_CMD);
  port_wr(PX_CMD, cmd | CMD_FRE);
  port_wr(PX_CMD, port_rd(PX_CMD) | CMD_ST);
}

/* Build a one-sector READ/WRITE DMA EXT command in slot 0 and ring PxCI. */
static int issue(uint32_t lba, uint32_t count, uint8_t ata_cmd, bool write)
{
  if (count == 0 || count > 8)
    return -1;
  port_wr(PX_SERR, 0xFFFFFFFFu);
  port_wr(PX_IS, 0xFFFFFFFFu);

  /* Wait for the drive to be idle before handing it a command. */
  uint32_t i;
  for (i = 0; i < POLL_LIMIT; i++)
    if (!(port_rd(PX_TFD) & (TFD_BSY | TFD_DRQ)))
      break;
  if (i == POLL_LIMIT)
    return -1;

  /* Command header 0: 5-dword command FIS, one PRDT entry, W bit for writes. */
  clb[0] = 5 | (write ? (1u << 6) : 0) | (1u << 16);
  clb[1] = 0;
  clb[2] = (uint32_t)(uintptr_t)ctab;
  clb[3] = 0;

  /* Command table: zero the CFIS + PRDT region we touch. */
  volatile uint8_t *ct = ctab;
  for (int b = 0; b < 0x80 + 0x10; b++)
    ct[b] = 0;

  /* Register Host-to-Device FIS carrying the ATA command. */
  ct[0] = 0x27; /* FIS type: Reg H2D              */
  ct[1] = 0x80; /* C=1: this is a command         */
  ct[2] = ata_cmd;
  ct[4] = lba & 0xFF;
  ct[5] = (lba >> 8) & 0xFF;
  ct[6] = (lba >> 16) & 0xFF;
  ct[7] = 0x40;
  ct[8] = (lba >> 24) & 0xFF;
  ct[12] = (uint8_t)count;

  /* PRDT entry 0 (at offset 0x80): point at the bounce buffer, 512 bytes. */
  volatile uint32_t *prdt = (volatile uint32_t *)(ctab + 0x80);
  prdt[0] = (uint32_t)(uintptr_t)dbuf;
  prdt[1] = 0;
  prdt[2] = 0;
  prdt[3] = (count * 512 - 1) & 0x3FFFFF;

  port_wr(PX_CI, 1);

  for (i = 0; i < POLL_LIMIT; i++) {
    if (!(port_rd(PX_CI) & 1))
      break;
    if (port_rd(PX_IS) & IS_TFES)
      return -1;
  }
  if (i == POLL_LIMIT)
    return -1;
  if (port_rd(PX_IS) & IS_TFES)
    return -1;
  return 0;
}

int ahci_read_sector(uint32_t lba, void *buf) { return ahci_read(lba, buf, 1); }

int ahci_write_sector(uint32_t lba, const void *buf) { return ahci_write(lba, buf, 1); }

int ahci_read(uint32_t lba, void *buf, uint32_t count)
{
  if (!found || count == 0 || count > 8)
    return -1;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  if (issue(lba, count, ATA_READ_DMA_EX, false)) {
    spin_unlock_irqrestore(&io_lock, flags);
    return -1;
  }
  uint8_t *d = buf;
  for (uint32_t i = 0; i < count * 512; i++)
    d[i] = dbuf[i];
  spin_unlock_irqrestore(&io_lock, flags);
  return 0;
}

int ahci_write(uint32_t lba, const void *buf, uint32_t count)
{
  if (!found || count == 0 || count > 8)
    return -1;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  const uint8_t *s = buf;
  for (uint32_t i = 0; i < count * 512; i++)
    dbuf[i] = s[i];
  int rc = issue(lba, count, ATA_WRITE_DMA_EX, true);
  spin_unlock_irqrestore(&io_lock, flags);
  return rc;
}

int ahci_present(void) { return found; }
uint32_t ahci_sectors(void) { return cap_sectors; }
const char *ahci_model(void) { return model; }

/* IDENTIFY DEVICE (0xEC): read the 48-bit sector count (words 100..103) and model (words 27..46); best-effort. */
static uint32_t identify_drive(void)
{
  model[0] = '\0';
  if (issue(0, 1, 0xEC, false))
    return 0;
  const volatile uint16_t *id = (const volatile uint16_t *)dbuf;

  /* Model: 20 words, big-endian within each word; trim trailing spaces. */
  int n = 0;
  for (int i = 0; i < 20; i++) {
    model[n++] = (char)(id[27 + i] >> 8);
    model[n++] = (char)(id[27 + i] & 0xFF);
  }
  while (n > 0 && model[n - 1] == ' ')
    n--;
  model[n] = '\0';

  /* Words 100-103 = LBA48 sector count; low 32 bits is plenty here. */
  return (uint32_t)id[100] | ((uint32_t)id[101] << 16);
}

static int find_controller(uint8_t *bus, uint8_t *slot, uint8_t *func)
{
  for (int b = 0; b < 256; b++)
    for (int s = 0; s < 32; s++) {
      if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      for (int f = 0; f < 8; f++) {
        if ((pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00) & 0xFFFF) == 0xFFFF)
          continue;
        uint32_t cls = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08);
        if (((cls >> 24) & 0xFF) == AHCI_CLASS && ((cls >> 16) & 0xFF) == AHCI_SUBCLASS) {
          *bus = (uint8_t)b;
          *slot = (uint8_t)s;
          *func = (uint8_t)f;
          return 1;
        }
      }
    }
  return 0;
}

void ahci_init(void)
{
  uint8_t bus, slot, func;
  if (!find_controller(&bus, &slot, &func)) {
    con_write("ahci: no SATA controller found\n");
    return;
  }

  /* ABAR is BAR5 (config offset 0x24): the MMIO register block. */
  uint32_t bar5 = pci_read32(bus, slot, func, 0x24) & 0xFFFFFFF0u;
  if (!bar5) {
    con_write("ahci: controller has no MMIO base\n");
    return;
  }

  /* Enable memory space + bus-mastering DMA in the PCI command register. */
  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x06);

  /* The register block sits above the 16 MiB identity map; map it in. */
  if (paging_identity_map(bar5, 0x2000) != 0) {
    con_write("ahci: could not map MMIO\n");
    return;
  }
  /* Mark MMIO uncached, or a doorbell write can linger in the write-back cache (QEMU hides this). */
  paging_mark_uc(bar5, 0x2000);
  abar = (volatile uint32_t *)(uintptr_t)bar5;

  hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);

  /* Find the first implemented port with a SATA disk whose PHY is up. */
  uint32_t pi = hba_rd(HBA_PI);
  int chosen = -1;
  for (int p = 0; p < 32; p++) {
    if (!(pi & (1u << p)))
      continue;
    port = p;
    uint32_t ssts = port_rd(PX_SSTS);
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    if (det != 3 || ipm != 1)
      continue;
    if (port_rd(PX_SIG) != SIG_SATA)
      continue;
    chosen = p;
    break;
  }
  if (chosen < 0) {
    con_write("ahci: controller present but no SATA disk attached\n");
    return;
  }
  port = chosen;

  /* Carve the per-port DMA areas from the identity-mapped pool (phys == virt). */
  clb = palloc();
  fis = palloc();
  ctab = palloc();
  dbuf = palloc();
  if (!clb || !fis || !ctab || !dbuf) {
    con_write("ahci: out of DMA frames\n");
    return;
  }

  port_stop();
  port_wr(PX_CLB, (uint32_t)(uintptr_t)clb);
  port_wr(PX_CLBU, 0);
  port_wr(PX_FB, (uint32_t)(uintptr_t)fis);
  port_wr(PX_FBU, 0);
  port_wr(PX_SERR, 0xFFFFFFFFu);
  port_wr(PX_IS, 0xFFFFFFFFu);
  port_wr(PX_IE, 0);
  port_start();

  found = true;
  cap_sectors = identify_drive();

  con_printf("ahci: SATA disk on port %u", (uint32_t)port);
  if (model[0]) {
    con_printf(" [%s]", model);
  }
  if (cap_sectors) {
    con_printf(", %u MiB", cap_sectors / 2048);
  }
  con_putc('\n');
}
