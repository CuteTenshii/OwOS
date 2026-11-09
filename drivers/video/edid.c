/* edid.c: read a monitor's EDID over Intel GMBUS or the DisplayPort AUX channel (real hardware only). */

#include "edid.h"
#include "dpaux.h"
#include "edidparse.h"
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

/* DDI AUX channels A-F: control register at 0x64010 + 0x100 * n, then five data dwords (Gen9+ layout). */
#define AUX_CTL_BASE 0x64010
#define AUX_CTL_STRIDE 0x100
#define AUX_PORTS 6
#define AUX_DATA 0x04
#define AUX_FIFO 20

#define AUX_SEND_BUSY (1u << 31)
#define AUX_DONE (1u << 30)
#define AUX_INTERRUPT (1u << 29)
#define AUX_TIMEOUT_ERR (1u << 28)
#define AUX_TIMEOUT_MAX (3u << 26) /* 1600 us */
#define AUX_RECEIVE_ERR (1u << 25)
#define AUX_SIZE_SHIFT 20
#define AUX_SIZE_MASK 0x1Fu
#define AUX_SYNC_PULSES (31u << 5 | 31u) /* 32 FW and 32 HW sync pulses */
#define AUX_ATTEMPTS 3

/* GMBUS block offset in BAR0: 0xC5100 on PCH-split parts (Gen5+), 0x5100 on older ones. Try both. */
static const uint32_t gmbus_bases[] = {0xC5100, 0x5100};

/* Candidate DDC pin pairs (GMBUS0[2:0]); the pin to port mapping varies by generation. */
static const uint8_t gmbus_pins[] = {2, 4, 5, 6, 3, 1};

static volatile uint8_t *mmio; /* mapped GPU BAR0                         */
static uint32_t base;          /* current GMBUS block offset             */

static uint8_t edid[EDID_MAX_BLOCKS * EDID_BLOCK]; /* base block + extensions */
static bool probed;                                /* one-shot guard                         */
static int cached_hz, cached_w, cached_h, cached_dpi;
static char cached_mfg[4];   /* 3-letter PNP manufacturer id           */
static char cached_name[16]; /* monitor-name descriptor (0xFC), if any */
static bool cached_internal; /* PNP id is a known laptop-panel vendor  */
static struct edid_cta cached_cta;
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

/* Gen9+ AUX CTL programming. A request is up to 20 bytes, packed big-endian into the data dwords. */
static int aux_xfer(void *ctx, const uint8_t *tx, int txlen, uint8_t *rx, int rxmax)
{
  (void)ctx;
  if (txlen < 1 || txlen > AUX_FIFO)
    return -1;

  for (int attempt = 0; attempt < AUX_ATTEMPTS; attempt++) {
    uint32_t start = timer_us();
    while (rd(0) & AUX_SEND_BUSY)
      if (timer_us() - start > 10000)
        return -1;

    for (int i = 0; i < txlen; i += 4) {
      uint32_t word = 0;
      for (int b = 0; b < 4; b++)
        word |= (uint32_t)(i + b < txlen ? tx[i + b] : 0) << (24 - b * 8);
      wr(AUX_DATA + (uint32_t)i, word);
    }

    uint32_t flags = AUX_DONE | AUX_INTERRUPT | AUX_TIMEOUT_ERR | AUX_RECEIVE_ERR;
    wr(0, AUX_SEND_BUSY | flags | AUX_TIMEOUT_MAX | ((uint32_t)txlen << AUX_SIZE_SHIFT) | AUX_SYNC_PULSES);

    uint32_t ctl;
    start = timer_us();
    while ((ctl = rd(0)) & AUX_SEND_BUSY)
      if (timer_us() - start > 10000)
        return -1;
    wr(0, ctl | flags);

    if (!(ctl & AUX_DONE) || (ctl & (AUX_TIMEOUT_ERR | AUX_RECEIVE_ERR)))
      continue;

    int n = (int)((ctl >> AUX_SIZE_SHIFT) & AUX_SIZE_MASK);
    if (n > rxmax)
      n = rxmax;
    for (int i = 0; i < n; i++)
      rx[i] = (uint8_t)(rd(AUX_DATA + (uint32_t)(i & ~3)) >> (24 - (i & 3) * 8));
    return n;
  }
  return -1;
}

/* EDID header is 00 FF FF FF FF FF FF 00 and the 128 bytes sum to 0 mod 256. */
static bool edid_valid(const uint8_t *e)
{
  static const uint8_t hdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  for (int i = 0; i < 8; i++)
    if (e[i] != hdr[i])
      return false;
  return edid_checksum_ok(e);
}

/* Width/height/refresh/DPI from the preferred timing across the base block and its extensions. */
static int parse_preferred(const uint8_t *e, int nblocks)
{
  struct edid_timing t;
  if (edid_preferred_timing(e, nblocks, &t) != 0) {
    seterr("no preferred timing");
    return -1;
  }
  cached_hz = t.hz;
  cached_w = t.w;
  cached_h = t.h;
  if (t.mm)
    cached_dpi = (int)(((uint32_t)t.w * 254u + (uint32_t)t.mm * 5u) / ((uint32_t)t.mm * 10u));
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

/* Try each DDI AUX channel for a DisplayPort/eDP sink; the first one with a valid EDID wins. */
static bool probe_dp_aux(void)
{
  for (uint32_t port = 0; port < AUX_PORTS; port++) {
    base = AUX_CTL_BASE + port * AUX_CTL_STRIDE;
    uint8_t caps[3];
    if (dp_read_dpcd(aux_xfer, 0, 0, caps, sizeof caps) != 0 || !dp_dpcd_valid(caps))
      continue;
    if (dp_read_edid(aux_xfer, 0, 0, edid) != 0) {
      seterr("DP AUX EDID read failed");
      continue;
    }
    if (!edid_valid(edid)) {
      seterr("bad checksum");
      continue;
    }
    int nblocks = 1 + (edid[126] < EDID_MAX_BLOCKS - 1 ? edid[126] : EDID_MAX_BLOCKS - 1);
    for (int i = 1; i < nblocks; i++)
      if (dp_read_edid(aux_xfer, 0, i, edid + i * EDID_BLOCK) != 0) {
        nblocks = i;
        break;
      }
    if (parse_preferred(edid, nblocks) == 0) {
      parse_identity(edid);
      edid_cta_info(edid, nblocks, &cached_cta);
      return true;
    }
  }
  return false;
}

static void probe(void)
{
  probed = true;
  err[0] = '\0';
  cached_hz = cached_w = cached_h = cached_dpi = 0;
  cached_mfg[0] = cached_name[0] = '\0';
  cached_internal = false;
  cached_cta = (struct edid_cta){0};

  if (!mmio) {
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
  }

  for (uint32_t bi = 0; bi < sizeof(gmbus_bases) / sizeof(gmbus_bases[0]); bi++) {
    base = gmbus_bases[bi];
    for (uint32_t pi = 0; pi < sizeof(gmbus_pins); pi++) {
      if (gmbus_read(gmbus_pins[pi], EDID_ADDR, edid, EDID_BLOCK) != 0)
        continue;
      if (!edid_valid(edid)) {
        seterr("bad checksum");
        continue;
      }
      /* The sink's read pointer continues past block 0; only block 1 is reachable without the
       * E-DDC segment pointer. A sink that wraps to block 0 has no extension. */
      int nblocks = 1;
      if (edid[126] > 0 && gmbus_read(gmbus_pins[pi], EDID_ADDR, edid + EDID_BLOCK, EDID_BLOCK) == 0 &&
          !edid_valid(edid + EDID_BLOCK))
        nblocks = 2;
      if (parse_preferred(edid, nblocks) == 0) {
        parse_identity(edid);
        edid_cta_info(edid, nblocks, &cached_cta);
        return;
      }
    }
  }
  if (probe_dp_aux()) {
    err[0] = '\0';
    return;
  }
  if (err[0] == '\0')
    seterr("no EDID (GMBUS NAK, no DP AUX sink)");
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
int edid_dpi(void)
{
  ensure();
  return cached_dpi;
}
int edid_internal(void)
{
  ensure();
  return cached_internal;
}
int edid_hdmi(void)
{
  ensure();
  return cached_cta.hdmi;
}
int edid_hdmi_audio(void)
{
  ensure();
  return cached_cta.hdmi && cached_cta.audio;
}
int edid_connected(void)
{
  ensure();
  return cached_mfg[0] != '\0';
}
void edid_rescan(void) { probe(); }

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
