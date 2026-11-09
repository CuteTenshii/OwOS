/* edidparse.c: preferred-timing extraction from EDID base, CTA-861 and DisplayID 1.x blocks. */

#include "edidparse.h"

#define EXT_CTA 0x02
#define EXT_DISPLAYID 0x70
#define DID_TYPE1_TIMING 0x03
#define DID_TYPE1_LEN 20
#define DID_HEADER 5 /* tag, version, section size, product type, extension count */

int edid_checksum_ok(const uint8_t *block)
{
  uint8_t sum = 0;
  for (int i = 0; i < EDID_BLOCK; i++)
    sum += block[i];
  return sum == 0;
}

static int set_timing(struct edid_timing *t, uint32_t pclk_khz, uint32_t hact, uint32_t hbl, uint32_t vact,
                      uint32_t vbl)
{
  uint32_t total = (hact + hbl) * (vact + vbl);
  if (pclk_khz == 0 || total == 0)
    return -1;
  t->hz = (int)((pclk_khz * 1000u + total / 2) / total);
  t->w = (int)hact;
  t->h = (int)vact;
  return 0;
}

/* 18-byte detailed timing descriptor. */
static int dtd(const uint8_t *d, struct edid_timing *t)
{
  uint32_t pclk = (d[0] | ((uint32_t)d[1] << 8)) * 10u; /* 10 kHz units */
  uint32_t hact = d[2] | (((uint32_t)d[4] >> 4) << 8);
  uint32_t hbl = d[3] | (((uint32_t)d[4] & 0x0F) << 8);
  uint32_t vact = d[5] | (((uint32_t)d[7] >> 4) << 8);
  uint32_t vbl = d[6] | (((uint32_t)d[7] & 0x0F) << 8);
  return set_timing(t, pclk, hact, hbl, vact, vbl);
}

/* DisplayID 1.x Type I descriptor: 24-bit pixel clock in 10 kHz units minus 1, 16-bit fields each minus 1. */
static int did_type1(const uint8_t *d, struct edid_timing *t)
{
  uint32_t pclk = ((d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16)) + 1u) * 10u;
  uint32_t hact = (d[4] | ((uint32_t)d[5] << 8)) + 1u;
  uint32_t hbl = (d[6] | ((uint32_t)d[7] << 8)) + 1u;
  uint32_t vact = (d[12] | ((uint32_t)d[13] << 8)) + 1u;
  uint32_t vbl = (d[14] | ((uint32_t)d[15] << 8)) + 1u;
  return set_timing(t, pclk, hact, hbl, vact, vbl);
}

/* Preferred (else first) Type I timing in a DisplayID extension block. */
static int displayid(const uint8_t *b, struct edid_timing *t)
{
  if ((b[1] >> 4) != 1)
    return -1;
  int size = b[2] < EDID_BLOCK - DID_HEADER - 1 ? b[2] : EDID_BLOCK - DID_HEADER - 1;
  const uint8_t *p = b + DID_HEADER;
  int found = 0;
  for (int o = 0; o + 3 <= size;) {
    int len = p[o + 2];
    if (o + 3 + len > size)
      break;
    if (p[o] == DID_TYPE1_TIMING) {
      for (int k = 0; k + DID_TYPE1_LEN <= len; k += DID_TYPE1_LEN) {
        const uint8_t *d = p + o + 3 + k;
        struct edid_timing c = {0};
        if (did_type1(d, &c) != 0)
          continue;
        if (d[3] & 0x80) {
          *t = c;
          return 0;
        }
        if (!found) {
          *t = c;
          found = 1;
        }
      }
    }
    o += 3 + len;
  }
  return found ? 0 : -1;
}

/* First detailed timing of a CTA-861 block; byte 2 is where the descriptors start. */
static int cta(const uint8_t *b, struct edid_timing *t)
{
  int start = b[2];
  if (start < 4 || start > EDID_BLOCK - 1 - 18)
    return -1;
  return dtd(b + start, t);
}

int edid_preferred_timing(const uint8_t *edid, int nblocks, struct edid_timing *t)
{
  uint32_t mm = edid[54 + 12] | (((uint32_t)edid[54 + 14] >> 4) << 8);
  if (mm == 0)
    mm = (uint32_t)edid[21] * 10; /* base block: max image width in cm */

  struct edid_timing found = {0};
  int ok = dtd(edid + 54, &found) == 0;
  for (int pass = 0; !ok && pass < 2; pass++) {
    for (int i = 1; i < nblocks && !ok; i++) {
      const uint8_t *b = edid + i * EDID_BLOCK;
      if (!edid_checksum_ok(b))
        continue;
      if (pass == 0 && b[0] == EXT_DISPLAYID)
        ok = displayid(b, &found) == 0;
      else if (pass == 1 && b[0] == EXT_CTA)
        ok = cta(b, &found) == 0;
    }
  }
  if (!ok)
    return -1;
  found.mm = (int)mm;
  *t = found;
  return 0;
}
