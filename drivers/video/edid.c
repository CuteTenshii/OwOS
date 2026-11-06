/* edid.c: read a monitor's EDID over Intel GMBUS and recover its refresh rate (real hardware only). */

#include "edid.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

#define GMBUS0 0x00 /* clock / pin-pair select                              */
#define GMBUS1 0x04 /* command + status                                     */
#define GMBUS2 0x08 /* status                                               */
#define GMBUS3 0x0C /* 4-byte data FIFO */

#define GMBUS_RATE_100KHZ (0u << 8)
#define GMBUS_SW_CLR_INT (1u << 31)
#define GMBUS_SW_RDY (1u << 30)
#define GMBUS_CYCLE_WAIT (1u << 25)
#define GMBUS_CYCLE_STOP (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT 16
#define GMBUS_SLAVE_READ (1u << 0)

#define GMBUS_HW_RDY (1u << 11) /* a data word is ready in GMBUS3  */
#define GMBUS_SATOER (1u << 10) /* slave NAK / no monitor on pin   */

#define EDID_ADDR 0x50 /* DDC EEPROM I2C slave address                      */
#define EDID_LEN 128   /* one EDID block                                    */

/* GMBUS block offset in BAR0: 0xC5100 on PCH-split parts (Gen5+), 0x5100 on older ones. Try both. */
static const uint32_t gmbus_bases[] = {0xC5100, 0x5100};

/* Candidate DDC pin pairs (GMBUS0[2:0]); the pin to port mapping varies by generation. */
static const uint8_t gmbus_pins[] = {2, 4, 5, 6, 3, 1};

static volatile uint8_t *mmio; /* mapped GPU BAR0                         */
static uint32_t base;          /* current GMBUS block offset             */

static bool probed; /* one-shot guard                         */
static int cached_hz, cached_w, cached_h;
static char cached_mfg[4];   /* 3-letter PNP manufacturer id           */
static char cached_name[16]; /* monitor-name descriptor (0xFC), if any */
static bool cached_internal; /* PNP id is a known laptop-panel vendor  */
static char err[48];

static void seterr(const char *s)
{
  int i = 0;
  while (s[i] && i < (int)sizeof(err) - 1) {
    err[i] = s[i];
    i++;
  }
  err[i] = '\0';
}

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(mmio + base + off); }
static inline void wr(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + base + off) = val; }

/* Find the first Intel display controller on bus 0; BAR0 base goes to bar_out. 0 on success. */
static int find_intel_gpu(uint32_t *bar_out)
{
  for (uint8_t slot = 0; slot < 32; slot++) {
    uint8_t header = (pci_read32(0, slot, 0, 0x0C) >> 16) & 0xFF;
    int funcs = (header & 0x80) ? 8 : 1;
    for (int func = 0; func < funcs; func++) {
      uint32_t id = pci_read32(0, slot, func, 0x00);
      if ((id & 0xFFFF) != 0x8086)
        continue;
      uint32_t cls = pci_read32(0, slot, func, 0x08);
      if (((cls >> 24) & 0xFF) != 0x03)
        continue;

      uint32_t bar0 = pci_read32(0, slot, func, 0x10);
      if (((bar0 >> 1) & 0x3) == 0x2) { /* 64-bit memory BAR       */
        if (pci_read32(0, slot, func, 0x14) != 0) {
          seterr("GPU BAR above 4GB");
          return -1;
        }
      }
      *bar_out = bar0 & ~0xFu;
      return 0;
    }
  }
  seterr("no Intel GPU");
  return -1;
}

/* One GMBUS I2C read of len bytes from addr on pin. 0 ok, -1 NAK, -2 timeout. */
static int gmbus_read(uint8_t pin, uint8_t addr, uint8_t *buf, int len)
{
  wr(GMBUS1, GMBUS_SW_CLR_INT);
  wr(GMBUS1, 0);
  wr(GMBUS0, pin | GMBUS_RATE_100KHZ);

  wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_WAIT | GMBUS_CYCLE_STOP | ((uint32_t)len << GMBUS_BYTE_COUNT_SHIFT) |
                 ((uint32_t)addr << 1) | GMBUS_SLAVE_READ);

  int got = 0;
  while (got < len) {
    uint32_t start = timer_us();
    for (;;) {
      uint32_t st = rd(GMBUS2);
      if (st & GMBUS_SATOER) {
        wr(GMBUS0, 0);
        return -1;
      }
      if (st & GMBUS_HW_RDY)
        break;
      if (timer_us() - start > 50000) {
        wr(GMBUS0, 0);
        return -2;
      }
    }
    uint32_t word = rd(GMBUS3);
    for (int b = 0; b < 4 && got < len; b++)
      buf[got++] = (word >> (b * 8)) & 0xFF;
  }
  wr(GMBUS0, 0);
  return 0;
}

/* EDID header is 00 FF FF FF FF FF FF 00 and the 128 bytes sum to 0 mod 256. */
static bool edid_valid(const uint8_t *e)
{
  static const uint8_t hdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  for (int i = 0; i < 8; i++)
    if (e[i] != hdr[i])
      return false;
  uint8_t sum = 0;
  for (int i = 0; i < EDID_LEN; i++)
    sum += e[i];
  return sum == 0;
}

/* Width/height/refresh from the preferred detailed timing descriptor at offset 54. */
static int parse_preferred(const uint8_t *e)
{
  const uint8_t *d = e + 54;
  uint32_t pclk = d[0] | ((uint32_t)d[1] << 8); /* 10 kHz units           */
  if (pclk == 0) {
    seterr("no preferred DTD");
    return -1;
  }
  uint32_t hact = d[2] | (((uint32_t)d[4] >> 4) << 8);
  uint32_t hbl = d[3] | (((uint32_t)d[4] & 0x0F) << 8);
  uint32_t vact = d[5] | (((uint32_t)d[7] >> 4) << 8);
  uint32_t vbl = d[6] | (((uint32_t)d[7] & 0x0F) << 8);

  uint32_t htotal = hact + hbl;
  uint32_t vtotal = vact + vbl;
  if (htotal == 0 || vtotal == 0) {
    seterr("bad DTD totals");
    return -1;
  }
  uint32_t total = htotal * vtotal;
  cached_hz = (int)((pclk * 10000u + total / 2) / total);
  cached_w = (int)hact;
  cached_h = (int)vact;
  return 0;
}

/* PNP ids of laptop-panel makers; EDID has no "internal" bit, so the vendor is the signal. */
static bool panel_vendor(const char *mfg)
{
  static const char *panels[] = {
      "AUO",
      /* AU Optronics */ "BOE", /* BOE Technology */
      "CMN",
      /* Chi Mei / Innolux */ "LGD", /* LG Display */
      "SHP",
      /* Sharp */ "SDC", /* Samsung Display */
      "IVO",
      /* InfoVision */ "CSO", /* CSOT */
      "CSW",
      /* CSOT */ "APP",
      /* Apple internal */ "PNP",
  };
  for (uint32_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++)
    if (mfg[0] == panels[i][0] && mfg[1] == panels[i][1] && mfg[2] == panels[i][2])
      return true;
  return false;
}

/* Decode the manufacturer id (bytes 8-9) and the monitor-name descriptor (tag 0xFC), if any. */
static void parse_identity(const uint8_t *e)
{
  uint16_t m = ((uint16_t)e[8] << 8) | e[9];
  cached_mfg[0] = (char)('@' + ((m >> 10) & 0x1F));
  cached_mfg[1] = (char)('@' + ((m >> 5) & 0x1F));
  cached_mfg[2] = (char)('@' + (m & 0x1F));
  cached_mfg[3] = '\0';

  /* Descriptors start at offset 54; 00 00 00 FC leads the ASCII name (<=13 bytes, 0x0A-terminated). */
  for (int off = 54; off <= 108; off += 18) {
    const uint8_t *d = e + off;
    if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFC) {
      int j = 0;
      for (int k = 0; k < 13 && j < (int)sizeof(cached_name) - 1; k++) {
        uint8_t c = d[5 + k];
        if (c == 0x0A)
          break;
        cached_name[j++] = (char)c;
      }
      while (j > 0 && cached_name[j - 1] == ' ')
        j--;
      cached_name[j] = '\0';
      break;
    }
  }
  cached_internal = panel_vendor(cached_mfg);
}

static void probe(void)
{
  probed = true;
  err[0] = '\0';

  uint32_t bar;
  if (find_intel_gpu(&bar) != 0)
    return;

  /* Map the first ~1 MiB of BAR0 uncached (one-shot, not reclaimed). */
  if (paging_identity_map(bar, 0x100000) != 0) {
    seterr("BAR map failed");
    return;
  }
  paging_mark_uc(bar, 0x100000);
  mmio = (volatile uint8_t *)(uintptr_t)bar;

  uint8_t edid[EDID_LEN];
  for (uint32_t bi = 0; bi < sizeof(gmbus_bases) / sizeof(gmbus_bases[0]); bi++) {
    base = gmbus_bases[bi];
    for (uint32_t pi = 0; pi < sizeof(gmbus_pins); pi++) {
      if (gmbus_read(gmbus_pins[pi], EDID_ADDR, edid, EDID_LEN) != 0)
        continue;
      if (!edid_valid(edid)) {
        seterr("bad checksum");
        continue;
      }
      if (parse_preferred(edid) == 0) {
        parse_identity(edid);
        return;
      }
    }
  }
  if (err[0] == '\0')
    seterr("no EDID (GMBUS NAK)");
}

static void ensure(void)
{
  if (!probed)
    probe();
}

int edid_refresh_hz(void)
{
  ensure();
  return cached_hz;
}
int edid_width(void)
{
  ensure();
  return cached_w;
}
int edid_height(void)
{
  ensure();
  return cached_h;
}
int edid_internal(void)
{
  ensure();
  return cached_internal;
}

/* Friendly brand for common PNP ids, else the raw 3-letter code. */
static const char *brand_name(const char *mfg)
{
  static const struct {
    char id[4];
    const char *name;
  } map[] = {
      {"DEL", "Dell"},         {"SAM", "Samsung"},   {"SDC", "Samsung"}, {"GSM", "LG"},    {"LGD", "LG"},
      {"AUO", "AU Optronics"}, {"BOE", "BOE"},       {"CMN", "Innolux"}, {"SHP", "Sharp"}, {"ACI", "ASUS"},
      {"AUS", "ASUS"},         {"BNQ", "BenQ"},      {"HWP", "HP"},      {"ACR", "Acer"},  {"AOC", "AOC"},
      {"APP", "Apple"},        {"VSC", "ViewSonic"}, {"PHL", "Philips"},
  };
  for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (mfg[0] == map[i].id[0] && mfg[1] == map[i].id[1] && mfg[2] == map[i].id[2])
      return map[i].name;
  return mfg;
}

/* Compose "<brand> <model>" into out; "" if no EDID. */
void edid_model(char *out, int len)
{
  ensure();
  int i = 0;
  if (cached_mfg[0]) {
    const char *b = brand_name(cached_mfg);
    for (; b[i] && i < len - 1; i++)
      out[i] = b[i];
    if (cached_name[0] && i < len - 1) {
      out[i++] = ' ';
      for (int k = 0; cached_name[k] && i < len - 1; k++)
        out[i++] = cached_name[k];
    }
  }
  out[i] = '\0';
}

const char *edid_error(void)
{
  ensure();
  return err;
}
