/* sd.c: SD/MMC driver over a PCI SDHCI controller, polled PIO, one 512-byte block at a time.
 * Identification: CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7. SDSC is byte-addressed, SDHC/SDXC block-addressed.
 * Waits are bounded; data moves through the Buffer Data Port, no DMA. */

#include <stdbool.h>

#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "sd.h"

#define SD_CLASS 0x08
#define SD_SUBCLASS 0x05

/* Byte offsets into BAR0. Widths matter - SDHCI registers are 8/16/32-bit. */
#define R_ARGUMENT 0x08
#define R_BLOCK_SIZE 0x04
#define R_BLOCK_COUNT 0x06
#define R_XFER_MODE 0x0C
#define R_COMMAND 0x0E
#define R_RESPONSE 0x10
#define R_BUFFER 0x20
#define R_PRESENT 0x24
#define R_HOST_CTRL 0x28
#define R_POWER_CTRL 0x29
#define R_CLOCK_CTRL 0x2C
#define R_TIMEOUT_CTRL 0x2E
#define R_SW_RESET 0x2F
#define R_INT_STATUS 0x30
#define R_ERR_STATUS 0x32
#define R_INT_ENABLE 0x34
#define R_SIGNAL_ENABLE 0x38
#define R_CAPS 0x40
#define R_HOST_VERSION 0xFE

/* Present state bits. */
#define PS_CMD_INHIBIT 0x00000001u
#define PS_DAT_INHIBIT 0x00000002u
#define PS_CARD_PRESENT 0x00010000u

/* Software reset bits. */
#define SR_ALL 0x01
#define SR_CMD 0x02
#define SR_DAT 0x04

/* Clock control bits. */
#define CC_INT_EN 0x0001
#define CC_INT_STABLE 0x0002
#define CC_SD_EN 0x0004

/* Power control bits. */
#define PC_BUS_ON 0x01
#define PC_V33 0x0E

/* Transfer mode bits. */
#define TM_BLKCNT_EN 0x0002
#define TM_READ 0x0010

/* Command register flag bits (the index goes in bits 8..15). */
#define CF_RESP_NONE 0x0000
#define CF_RESP_136 0x0001
#define CF_RESP_48 0x0002
#define CF_RESP_48BUSY 0x0003
#define CF_CRC_CHECK 0x0008
#define CF_IDX_CHECK 0x0010
#define CF_DATA_PRESENT 0x0020

/* Common response shapes. */
#define RESP_R1 (CF_RESP_48 | CF_CRC_CHECK | CF_IDX_CHECK)
#define RESP_R1B (CF_RESP_48BUSY | CF_CRC_CHECK | CF_IDX_CHECK)
#define RESP_R2 (CF_RESP_136 | CF_CRC_CHECK)
#define RESP_R3 (CF_RESP_48)
#define RESP_R6 (CF_RESP_48 | CF_CRC_CHECK | CF_IDX_CHECK)
#define RESP_R7 (CF_RESP_48 | CF_CRC_CHECK | CF_IDX_CHECK)

/* Normal interrupt status bits. */
#define IS_CMD_DONE 0x0001
#define IS_XFER_DONE 0x0002
#define IS_BUF_WR_RDY 0x0010
#define IS_BUF_RD_RDY 0x0020
#define IS_ERROR 0x8000

#define POLL_LIMIT 5000000u

static volatile uint8_t *mmio;
static bool found;
static bool high_capacity;
static uint16_t rca;
static uint32_t cap_sectors;
static char model[24];

static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(mmio + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(mmio + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(mmio + o); }
static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(mmio + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }

/* Crude busy delay (no timer yet): present-state reads space out the ACMD41 poll. */
static void sd_spin(uint32_t n)
{
  while (n--)
    (void)rd32(R_PRESENT);
}

static uint32_t base_khz;

/* Program the SD clock to at most target_khz (divider 2*N with N a power of two; N==0 divides by 1). */
static void sd_set_clock(uint32_t target_khz)
{
  wr16(R_CLOCK_CTRL, 0);

  uint32_t base = base_khz ? base_khz : 50000;
  uint32_t div = 1;
  while ((base / div) > target_khz && div < 256)
    div <<= 1;
  uint16_t sel = (div <= 1) ? 0 : (uint16_t)(div >> 1);

  uint16_t v = (uint16_t)((sel & 0xFF) << 8) | CC_INT_EN;
  wr16(R_CLOCK_CTRL, v);
  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if (rd16(R_CLOCK_CTRL) & CC_INT_STABLE)
      break;
  wr16(R_CLOCK_CTRL, v | CC_SD_EN);
}

/* Wait for a normal-status bit, acking it. Returns -1 on error or timeout. */
static int sd_wait(uint16_t want)
{
  for (uint32_t i = 0; i < POLL_LIMIT; i++) {
    uint16_t st = rd16(R_INT_STATUS);
    if (st & IS_ERROR) {
      wr16(R_ERR_STATUS, rd16(R_ERR_STATUS));
      wr16(R_INT_STATUS, st);
      return -1;
    }
    if (st & want) {
      wr16(R_INT_STATUS, want);
      return 0;
    }
  }
  return -1;
}

/* Issue one SD command; data adds the data-present bit. Returns 0 on Command Complete, -1 on error. */
static int sd_command(uint8_t idx, uint32_t arg, uint16_t flags, bool data)
{
  /* Wait for CMD inhibit to clear, and DAT for data transfers and R1b responses. */
  uint32_t inhibit = PS_CMD_INHIBIT;
  if (data || (flags & 0x3) == CF_RESP_48BUSY)
    inhibit |= PS_DAT_INHIBIT;
  uint32_t i;
  for (i = 0; i < POLL_LIMIT; i++)
    if (!(rd32(R_PRESENT) & inhibit))
      break;
  if (i == POLL_LIMIT)
    return -1;

  wr32(R_ARGUMENT, arg);
  uint16_t cmd = (uint16_t)((uint16_t)idx << 8) | flags;
  if (data)
    cmd |= CF_DATA_PRESENT;
  wr16(R_COMMAND, cmd);

  return sd_wait(IS_CMD_DONE);
}

/* Extract CSD[hi:lo] from the response registers; SDHCI stores CSD[127:8], so bit n is at (n-8). Width <= 32. */
static uint32_t csd_bits(const uint32_t *r, int hi, int lo)
{
  uint32_t v = 0;
  for (int b = hi - 8; b >= lo - 8; b--)
    v = (v << 1) | ((r[b >> 5] >> (b & 31)) & 1u);
  return v;
}

static void parse_csd(const uint32_t *r)
{
  cap_sectors = 0;
  if (csd_bits(r, 127, 126) == 1) {
    /* CSD v2 (SDHC/SDXC): capacity = (C_SIZE+1) * 512 KiB. */
    uint32_t c_size = csd_bits(r, 69, 48);
    cap_sectors = (c_size + 1) * 1024u;
  } else {
    /* CSD v1 (SDSC): (C_SIZE+1) * 2^(C_SIZE_MULT+2) blocks of READ_BL_LEN. */
    uint32_t c_size = csd_bits(r, 73, 62);
    uint32_t c_size_mult = csd_bits(r, 49, 47);
    uint32_t read_bl_len = csd_bits(r, 83, 80);
    uint32_t shift = c_size_mult + 2 + read_bl_len;
    if (shift >= 9)
      cap_sectors = (c_size + 1) << (shift - 9);
  }
}

static int card_init(void)
{
  uint32_t r[4];

  if (sd_command(0, 0, CF_RESP_NONE, false))
    return -1;

  /* CMD8: check-pattern 0xAA at 2.7-3.6 V. An R7 echo means an SD v2 card. */
  int v2 = 0;
  if (sd_command(8, 0x1AA, RESP_R7, false) == 0 && (rd32(R_RESPONSE) & 0xFF) == 0xAA)
    v2 = 1;

  /* ACMD41: poll until OCR bit 31; HCS (arg bit 30) advertises high-capacity support. */
  uint32_t ocr = 0;
  for (int t = 0; t < 2000; t++) {
    if (sd_command(55, 0, RESP_R1, false))
      return -1;
    uint32_t arg = 0x00FF8000u | (v2 ? 0x40000000u : 0u);
    if (sd_command(41, arg, RESP_R3, false))
      return -1;
    ocr = rd32(R_RESPONSE);
    if (ocr & 0x80000000u)
      break;
    sd_spin(20000);
  }
  if (!(ocr & 0x80000000u))
    return -1;
  high_capacity = (ocr & 0x40000000u) != 0;

  if (sd_command(2, 0, RESP_R2, false))
    return -1;
  if (sd_command(3, 0, RESP_R6, false))
    return -1;
  rca = (uint16_t)(rd32(R_RESPONSE) >> 16);

  if (sd_command(9, (uint32_t)rca << 16, RESP_R2, false))
    return -1;
  r[0] = rd32(R_RESPONSE);
  r[1] = rd32(R_RESPONSE + 4);
  r[2] = rd32(R_RESPONSE + 8);
  r[3] = rd32(R_RESPONSE + 12);
  parse_csd(r);

  if (sd_command(7, (uint32_t)rca << 16, RESP_R1B, false))
    return -1;

  /* Fix the block length at 512 (required for byte-addressed SDSC). */
  if (sd_command(16, 512, RESP_R1, false))
    return -1;
  return 0;
}

static int card_bring_up(void);

static int do_transfer(uint32_t lba, void *buf, bool write)
{
  /* Lazily bring up a hot-inserted card; card_bring_up() fails harmlessly on an empty slot. */
  if (!found && card_bring_up() != 0)
    return -1;

  /* SDSC is byte-addressed; a large lba would wrap the 32-bit argument, so fail out of range. */
  if (!high_capacity && lba > 0xFFFFFFFFu / 512u)
    return -1;
  uint32_t arg = high_capacity ? lba : lba * 512u;

  wr16(R_BLOCK_SIZE, 512);
  wr16(R_BLOCK_COUNT, 1);
  wr16(R_XFER_MODE, write ? 0 : TM_READ);

  uint8_t cmd = write ? 24 : 17;
  if (sd_command(cmd, arg, RESP_R1, true))
    return -1;

  /* Stream 512 bytes via the Buffer Data Port through an aligned local (the caller buffer may be unaligned). */
  uint8_t *p = buf;
  if (write) {
    if (sd_wait(IS_BUF_WR_RDY))
      return -1;
    for (int i = 0; i < 128; i++) {
      uint32_t w;
      memcpy(&w, p + i * 4, 4);
      wr32(R_BUFFER, w);
    }
  } else {
    if (sd_wait(IS_BUF_RD_RDY))
      return -1;
    for (int i = 0; i < 128; i++) {
      uint32_t w = rd32(R_BUFFER);
      memcpy(p + i * 4, &w, 4);
    }
  }
  return sd_wait(IS_XFER_DONE);
}

int sd_read_sector(uint32_t lba, void *buf) { return do_transfer(lba, buf, false); }
int sd_write_sector(uint32_t lba, const void *buf) { return do_transfer(lba, (void *)buf, true); }

/* Live card presence from the SDHCI card-detect bit (cheap, safe to poll); a present card is brought up lazily on first
 * transfer. */
int sd_present(void)
{
  if (!mmio)
    return 0;
  if (rd32(R_PRESENT) & PS_CARD_PRESENT)
    return 1;
  found = false;
  return 0;
}
uint32_t sd_sectors(void) { return cap_sectors; }
const char *sd_model(void) { return model; }

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
        if (((cls >> 24) & 0xFF) == SD_CLASS && ((cls >> 16) & 0xFF) == SD_SUBCLASS) {
          *bus = (uint8_t)b;
          *slot = (uint8_t)s;
          *func = (uint8_t)f;
          return 1;
        }
      }
    }
  return 0;
}

/* Power, clock and run the SD handshake on the card in the slot; re-runnable for hot insert. Needs base_khz. Returns 0
 * on success. */
static int card_bring_up(void)
{
  if (!mmio || !(rd32(R_PRESENT) & PS_CARD_PRESENT))
    return -1;

  /* Power the slot at 3.3 V and start the ID clock (~400 kHz). */
  wr8(R_POWER_CTRL, 0);
  wr8(R_POWER_CTRL, PC_V33 | PC_BUS_ON);
  sd_set_clock(400);
  wr8(R_TIMEOUT_CTRL, 0x0E);

  /* Polled operation: let status bits latch (INT_ENABLE) but raise no IRQs. */
  wr32(R_INT_STATUS, 0xFFFFFFFFu);
  wr32(R_INT_ENABLE, 0xFFFFFFFFu);
  wr32(R_SIGNAL_ENABLE, 0);

  if (card_init() != 0)
    return -1;

  sd_set_clock(25000);
  found = true;
  if (high_capacity)
    memcpy(model, "SDHC/SDXC card", sizeof("SDHC/SDXC card"));
  else
    memcpy(model, "SD card", sizeof("SD card"));
  return 0;
}

void sd_init(void)
{
  uint8_t bus, slot, func;
  if (!find_controller(&bus, &slot, &func)) {
    con_write("sd: no SDHCI controller found\n");
    return;
  }

  /* BAR0 (config offset 0x10) is the MMIO register block. */
  uint32_t bar0 = pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0u;
  if (!bar0 || bar0 == 0xFFFFFFF0u) {
    con_write("sd: controller has no MMIO base\n");
    return;
  }

  /* Enable memory space + bus mastering in the PCI command register. */
  uint32_t pcmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, pcmd | 0x06);

  /* Map the register block uncached. */
  if (paging_identity_map(bar0, 0x1000) != 0) {
    con_write("sd: could not map MMIO\n");
    return;
  }
  paging_mark_uc(bar0, 0x1000);
  mmio = (volatile uint8_t *)(uintptr_t)bar0;

  /* Full software reset, then wait for it to self-clear. */
  wr8(R_SW_RESET, SR_ALL);
  uint32_t i;
  for (i = 0; i < POLL_LIMIT; i++)
    if (!(rd8(R_SW_RESET) & SR_ALL))
      break;
  if (i == POLL_LIMIT) {
    con_write("sd: controller reset timed out\n");
    return;
  }

  /* Base clock from capabilities (SDHCI 3.0: bits 15:8 MHz; 2.0: 13:8), read even with an empty slot for later hot
   * insert. */
  uint32_t caps = rd32(R_CAPS);
  uint16_t ver = (uint16_t)(rd16(R_HOST_VERSION) & 0xFF);
  uint32_t base_mhz = (ver >= 2) ? ((caps >> 8) & 0xFF) : ((caps >> 8) & 0x3F);
  base_khz = base_mhz * 1000u;

  if (!(rd32(R_PRESENT) & PS_CARD_PRESENT)) {
    /* No card yet: keep the controller live so hotplug works (sd_present() polls card-detect). */
    con_write("sd: controller ready, slot empty (hotplug-capable)\n");
    return;
  }

  if (card_bring_up() != 0) {
    con_write("sd: card initialisation failed\n");
    return;
  }

  con_printf("sd: %s", model);
  if (cap_sectors) {
    con_printf(", %u MiB", cap_sectors / 2048);
  }
  con_putc('\n');
}
