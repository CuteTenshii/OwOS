/* edidparse_test.c: preferred-timing extraction from EDID base, CTA-861 and DisplayID 1.x blocks
 * (drivers/video/edidparse.c). Run through tests/run-tests.sh edidparse. */
#define OWOS_SUITE_TEST
#include "../drivers/video/edidparse.h"
#include "test.h"
#include <string.h>

static void seal(uint8_t *block)
{
  uint8_t sum = 0;
  block[127] = 0;
  for (int i = 0; i < EDID_BLOCK; i++)
    sum += block[i];
  block[127] = (uint8_t)-sum;
}

static void put_dtd(uint8_t *d, unsigned pclk10k, unsigned hact, unsigned hbl, unsigned vact, unsigned vbl, unsigned mm)
{
  d[0] = (uint8_t)pclk10k;
  d[1] = (uint8_t)(pclk10k >> 8);
  d[2] = (uint8_t)hact;
  d[3] = (uint8_t)hbl;
  d[4] = (uint8_t)(((hact >> 8) << 4) | (hbl >> 8));
  d[5] = (uint8_t)vact;
  d[6] = (uint8_t)vbl;
  d[7] = (uint8_t)(((vact >> 8) << 4) | (vbl >> 8));
  d[12] = (uint8_t)mm;
  d[14] = (uint8_t)((mm >> 8) << 4);
}

static void put_did1(uint8_t *d, unsigned pclk10k, unsigned hact, unsigned hbl, unsigned vact, unsigned vbl, int pref)
{
  unsigned p = pclk10k - 1;
  d[0] = (uint8_t)p;
  d[1] = (uint8_t)(p >> 8);
  d[2] = (uint8_t)(p >> 16);
  d[3] = pref ? 0x80 : 0;
  d[4] = (uint8_t)(hact - 1);
  d[5] = (uint8_t)((hact - 1) >> 8);
  d[6] = (uint8_t)(hbl - 1);
  d[7] = (uint8_t)((hbl - 1) >> 8);
  d[12] = (uint8_t)(vact - 1);
  d[13] = (uint8_t)((vact - 1) >> 8);
  d[14] = (uint8_t)(vbl - 1);
  d[15] = (uint8_t)((vbl - 1) >> 8);
}

/* DisplayID 1.x extension with one Type I block holding n descriptors written by fill(i, desc). */
static void did_block(uint8_t *b, int version, int n, void (*fill)(int, uint8_t *))
{
  memset(b, 0, EDID_BLOCK);
  b[0] = 0x70;
  b[1] = (uint8_t)version;
  b[2] = (uint8_t)(3 + 20 * n);
  uint8_t *p = b + 5;
  p[0] = 0x03;
  p[1] = 0x01;
  p[2] = (uint8_t)(20 * n);
  for (int i = 0; i < n; i++)
    fill(i, p + 3 + 20 * i);
  seal(b);
}

static void base_block(uint8_t *b, int placeholder)
{
  memset(b, 0, EDID_BLOCK);
  b[0] = b[7] = 0;
  memset(b + 1, 0xFF, 6);
  b[21] = 53;
  if (!placeholder)
    put_dtd(b + 54, 14850, 1920, 280, 1080, 45, 527);
  b[126] = 1;
  seal(b);
}

static void fill_4k144(int i, uint8_t *d)
{
  (void)i;
  put_did1(d, 127987, 3840, 160, 2160, 62, 1);
}

static void fill_two(int i, uint8_t *d)
{
  if (i == 0)
    put_did1(d, 14850, 1920, 280, 1080, 45, 0);
  else
    put_did1(d, 127987, 3840, 160, 2160, 62, 1);
}

static void fill_none_preferred(int i, uint8_t *d)
{
  if (i == 0)
    put_did1(d, 14850, 1920, 280, 1080, 45, 0);
  else
    put_did1(d, 127987, 3840, 160, 2160, 62, 0);
}

static void base_dtd(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 0);
  struct edid_timing t;
  expect_eq(edid_preferred_timing(e, 1, &t), 0);
  expect_eq(t.w, 1920);
  expect_eq(t.h, 1080);
  expect_eq(t.hz, 60);
  expect_eq(t.mm, 527);
}

static void displayid_replaces_placeholder(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 1);
  did_block(e + EDID_BLOCK, 0x12, 1, fill_4k144);
  struct edid_timing t;
  expect_eq(edid_preferred_timing(e, 2, &t), 0);
  expect_eq(t.w, 3840);
  expect_eq(t.h, 2160);
  expect_eq(t.hz, 144);
  expect_eq(t.mm, 530); /* falls back to the base block's width in cm */
}

static void displayid_preferred_flag(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 1);
  did_block(e + EDID_BLOCK, 0x12, 2, fill_two);
  struct edid_timing t;
  expect_eq(edid_preferred_timing(e, 2, &t), 0);
  expect_eq(t.w, 3840);

  did_block(e + EDID_BLOCK, 0x12, 2, fill_none_preferred);
  expect_eq(edid_preferred_timing(e, 2, &t), 0);
  expect_eq(t.w, 1920);
}

static void base_dtd_wins(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 0);
  did_block(e + EDID_BLOCK, 0x12, 1, fill_4k144);
  struct edid_timing t;
  expect_eq(edid_preferred_timing(e, 2, &t), 0);
  expect_eq(t.w, 1920);
}

static void cta_fallback(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 1);
  uint8_t *c = e + EDID_BLOCK;
  memset(c, 0, EDID_BLOCK);
  c[0] = 0x02;
  c[1] = 3;
  c[2] = 4;
  put_dtd(c + 4, 24200, 2560, 160, 1440, 41, 0);
  seal(c);
  struct edid_timing t;
  expect_eq(edid_preferred_timing(e, 2, &t), 0);
  expect_eq(t.w, 2560);
  expect_eq(t.h, 1440);
  expect_eq(t.hz, 60);
}

static void bad_extensions_ignored(void)
{
  uint8_t e[2 * EDID_BLOCK];
  struct edid_timing t;
  base_block(e, 1);
  did_block(e + EDID_BLOCK, 0x12, 1, fill_4k144);
  e[EDID_BLOCK + 20] ^= 0x10; /* corrupt without resealing */
  expect(edid_preferred_timing(e, 2, &t) < 0);

  did_block(e + EDID_BLOCK, 0x20, 1, fill_4k144); /* DisplayID 2.0 is not parsed */
  expect(edid_preferred_timing(e, 2, &t) < 0);

  did_block(e + EDID_BLOCK, 0x12, 1, fill_4k144);
  expect(edid_preferred_timing(e, 1, &t) < 0); /* extension not counted */
}

static void malformed_displayid_lengths(void)
{
  uint8_t e[2 * EDID_BLOCK];
  base_block(e, 1);
  did_block(e + EDID_BLOCK, 0x12, 1, fill_4k144);
  e[EDID_BLOCK + 7] = 0xFF; /* data block length past the section */
  seal(e + EDID_BLOCK);
  struct edid_timing t;
  expect(edid_preferred_timing(e, 2, &t) < 0);
  e[EDID_BLOCK + 2] = 0xFF; /* section size past the block */
  seal(e + EDID_BLOCK);
  expect(edid_preferred_timing(e, 2, &t) < 0);
}

/* CTA-861 block (revision 3) holding the given data blocks back to back, DTDs starting right after them. */
static void cta_block(uint8_t *b, int flags, const uint8_t *blocks, int len)
{
  memset(b, 0, EDID_BLOCK);
  b[0] = 0x02;
  b[1] = 3;
  b[2] = (uint8_t)(4 + len);
  b[3] = (uint8_t)flags;
  memcpy(b + 4, blocks, (size_t)len);
  seal(b);
}

static const uint8_t vsdb_hdmi[] = {0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x2F}; /* 47 * 5 = 235 MHz */
static const uint8_t vsdb_forum[] = {0x65, 0xD8, 0x5D, 0xC4, 0x01, 0x78};            /* 120 * 5 = 600 MHz */

static void cta_hdmi_sink(void)
{
  uint8_t e[2 * EDID_BLOCK];
  struct edid_cta c;
  base_block(e, 0);
  cta_block(e + EDID_BLOCK, 0x40, vsdb_hdmi, sizeof vsdb_hdmi);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 1);
  expect_eq(c.audio, 1);
  expect_eq(c.max_tmds_mhz, 235);
}

static void cta_forum_rate_wins(void)
{
  uint8_t e[2 * EDID_BLOCK], blocks[sizeof vsdb_hdmi + sizeof vsdb_forum];
  struct edid_cta c;
  memcpy(blocks, vsdb_hdmi, sizeof vsdb_hdmi);
  memcpy(blocks + sizeof vsdb_hdmi, vsdb_forum, sizeof vsdb_forum);
  base_block(e, 0);
  cta_block(e + EDID_BLOCK, 0, blocks, sizeof blocks);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 1);
  expect_eq(c.audio, 0);
  expect_eq(c.max_tmds_mhz, 600);
}

static void cta_dvi_sink(void)
{
  uint8_t e[2 * EDID_BLOCK];
  struct edid_cta c;
  const uint8_t video_block[] = {0x42, 16, 4}; /* a video data block, not the vendor one */
  base_block(e, 0);
  cta_block(e + EDID_BLOCK, 0, video_block, sizeof video_block);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 0);
  expect_eq(c.max_tmds_mhz, 0);

  edid_cta_info(e, 1, &c); /* no extension counted */
  expect_eq(c.hdmi, 0);
}

static void cta_ignores_bad_blocks(void)
{
  uint8_t e[2 * EDID_BLOCK];
  struct edid_cta c;
  base_block(e, 0);
  cta_block(e + EDID_BLOCK, 0x40, vsdb_hdmi, sizeof vsdb_hdmi);
  e[EDID_BLOCK + 3] ^= 0x01; /* checksum broken */
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 0);
  expect_eq(c.audio, 0);

  cta_block(e + EDID_BLOCK, 0x40, vsdb_hdmi, sizeof vsdb_hdmi);
  e[EDID_BLOCK + 1] = 1; /* revision 1 has no data blocks */
  seal(e + EDID_BLOCK);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 0);
}

static void cta_stays_in_bounds(void)
{
  uint8_t e[2 * EDID_BLOCK];
  struct edid_cta c;
  uint8_t overrun[] = {0x6F, 0x03, 0x0C, 0x00}; /* claims 15 bytes, only 3 follow before the DTD offset */
  base_block(e, 0);
  cta_block(e + EDID_BLOCK, 0, overrun, sizeof overrun);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 0);

  uint8_t shortvs[] = {0x63, 0x03, 0x0C, 0x00}; /* HDMI OUI but too short for a TMDS byte */
  cta_block(e + EDID_BLOCK, 0, shortvs, sizeof shortvs);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 1);
  expect_eq(c.max_tmds_mhz, 0);

  cta_block(e + EDID_BLOCK, 0, vsdb_hdmi, sizeof vsdb_hdmi);
  e[EDID_BLOCK + 2] = 0xFF; /* DTD offset past the block */
  seal(e + EDID_BLOCK);
  edid_cta_info(e, 2, &c);
  expect_eq(c.hdmi, 1);
}

int owos_main_edidparse(void)
{
  test("base block detailed timing", base_dtd);
  test("DisplayID timing replaces a placeholder base DTD", displayid_replaces_placeholder);
  test("DisplayID preferred flag, else first timing", displayid_preferred_flag);
  test("a real base DTD beats DisplayID", base_dtd_wins);
  test("CTA-861 detailed timing as last resort", cta_fallback);
  test("bad checksum, DisplayID 2.0 and uncounted blocks are ignored", bad_extensions_ignored);
  test("malformed DisplayID lengths stay in bounds", malformed_displayid_lengths);
  test("CTA HDMI vendor block, audio flag and TMDS clock", cta_hdmi_sink);
  test("HDMI Forum block raises the TMDS rate", cta_forum_rate_wins);
  test("CTA block without a vendor block is a DVI-style sink", cta_dvi_sink);
  test("CTA blocks with bad checksum or old revision are ignored", cta_ignores_bad_blocks);
  test("CTA data block lengths stay in bounds", cta_stays_in_bounds);
  return 0;
}
