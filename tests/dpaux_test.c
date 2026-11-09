/* dpaux_test.c: DisplayPort AUX protocol (drivers/video/dpaux.c) against a fake sink serving DPCD and an EDID over
 * I2C-over-AUX, including DEFER replies, short reads and NACKs. Run through tests/run-tests.sh dpaux. */
#define OWOS_SUITE_TEST
#include "../drivers/video/dpaux.h"
#include "../drivers/video/edidparse.h"
#include "test.h"
#include <string.h>

struct sink {
  uint8_t dpcd[0x100];
  uint8_t edid[EDID_MAX_BLOCKS * DP_EDID_LEN];
  int defer;      /* DEFER this many requests before answering */
  int max_reply;  /* cap data bytes per reply (short reads), 0 = no cap */
  int nack_i2c;   /* answer I2C requests with an I2C NACK */
  int offset;     /* EDID read pointer */
  int segment;    /* E-DDC segment pointer */
  int open;       /* I2C transaction held open (MOT) */
  int stops;      /* address-only writes seen */
  int bad_header; /* count of malformed headers */
};

static int fake_xfer(void *ctx, const uint8_t *tx, int txlen, uint8_t *rx, int rxmax)
{
  struct sink *s = ctx;
  int cmd = tx[0] >> 4;
  uint32_t addr = ((uint32_t)(tx[0] & 0xF) << 16) | ((uint32_t)tx[1] << 8) | tx[2];
  int len = txlen > 3 ? tx[3] + 1 : 0;
  (void)rxmax;

  if (s->defer > 0) {
    s->defer--;
    rx[0] = 2 << 6;
    return 1;
  }
  if (cmd == 0x9) {
    if (len < 1 || len > 16 || addr + (uint32_t)len > sizeof s->dpcd) {
      s->bad_header++;
      rx[0] = 1 << 6;
      return 1;
    }
    int n = s->max_reply && s->max_reply < len ? s->max_reply : len;
    rx[0] = 0;
    memcpy(rx + 1, s->dpcd + addr, (size_t)n);
    return 1 + n;
  }
  if (addr == 0x30) {
    s->segment = tx[4];
    rx[0] = 0;
    return 1;
  }
  if (addr != 0x50) {
    rx[0] = 1 << 6;
    return 1;
  }
  if (s->nack_i2c) {
    if ((cmd & 3) == 0 && len == 0)
      s->stops++;
    rx[0] = 1 << 4;
    return 1;
  }
  int mot = cmd & 4;
  switch (cmd & 3) {
  case 0:
    if (len == 0) {
      s->stops++;
      s->open = 0;
    } else {
      s->offset = tx[4];
      s->open = mot;
      if (!mot)
        s->segment = 0;
    }
    rx[0] = 0;
    return 1;
  case 1: {
    if (len > 16) {
      s->bad_header++;
      rx[0] = 1 << 4;
      return 1;
    }
    int n = s->max_reply && s->max_reply < len ? s->max_reply : len;
    memcpy(rx + 1, s->edid + s->segment * 256 + s->offset, (size_t)n);
    s->offset += n;
    s->open = mot;
    if (!mot)
      s->segment = 0;
    rx[0] = 0;
    return 1 + n;
  }
  }
  return -1;
}

static void init_sink(struct sink *s)
{
  memset(s, 0, sizeof *s);
  s->dpcd[0] = 0x14;
  s->dpcd[1] = 0x14;
  s->dpcd[2] = 0x84;
  for (int i = 0; i < DP_EDID_LEN; i++)
    s->edid[i] = (uint8_t)(i * 7 + 3);
}

static void dpcd_read(void)
{
  struct sink s;
  init_sink(&s);
  uint8_t caps[3] = {0};
  expect_eq(dp_read_dpcd(fake_xfer, &s, 0, caps, 3), 0);
  expect_eq(caps[0], 0x14);
  expect_eq(caps[1], 0x14);
  expect(dp_dpcd_valid(caps));

  for (int i = 0; i < 0x100; i++)
    s.dpcd[i] = (uint8_t)i;
  uint8_t big[40];
  expect_eq(dp_read_dpcd(fake_xfer, &s, 0x30, big, sizeof big), 0);
  expect(memcmp(big, s.dpcd + 0x30, sizeof big) == 0);
  expect_eq(s.bad_header, 0);
}

static void dpcd_validity(void)
{
  uint8_t ok[3] = {0x12, 0x0A, 0x82};
  expect(dp_dpcd_valid(ok));
  uint8_t floating[3] = {0xFF, 0xFF, 0xFF};
  expect(!dp_dpcd_valid(floating));
  uint8_t zero[3] = {0, 0, 0};
  expect(!dp_dpcd_valid(zero));
  uint8_t three_lanes[3] = {0x14, 0x14, 0x03};
  expect(!dp_dpcd_valid(three_lanes));
}

static void defer_retried(void)
{
  struct sink s;
  init_sink(&s);
  s.defer = 3;
  uint8_t caps[3];
  expect_eq(dp_read_dpcd(fake_xfer, &s, 0, caps, 3), 0);
  expect_eq(caps[2], 0x84);

  init_sink(&s);
  s.defer = 100;
  expect(dp_read_dpcd(fake_xfer, &s, 0, caps, 3) < 0);
}

static void short_reads_continue(void)
{
  struct sink s;
  init_sink(&s);
  s.max_reply = 5;
  uint8_t e[DP_EDID_LEN];
  expect_eq(dp_read_edid(fake_xfer, &s, 0, e), 0);
  expect(memcmp(e, s.edid, DP_EDID_LEN) == 0);
  expect_eq(s.bad_header, 0);
}

static void edid_read(void)
{
  struct sink s;
  init_sink(&s);
  uint8_t e[DP_EDID_LEN];
  expect_eq(dp_read_edid(fake_xfer, &s, 0, e), 0);
  expect(memcmp(e, s.edid, DP_EDID_LEN) == 0);
  expect_eq(s.open, 0);
  expect_eq(s.bad_header, 0);
}

static void edid_extension_blocks(void)
{
  struct sink s;
  init_sink(&s);
  for (int i = 0; i < EDID_MAX_BLOCKS * DP_EDID_LEN; i++)
    s.edid[i] = (uint8_t)(i / DP_EDID_LEN * 50 + i % 97);
  uint8_t e[DP_EDID_LEN];
  for (int block = 0; block < EDID_MAX_BLOCKS; block++) {
    expect_eq(dp_read_edid(fake_xfer, &s, block, e), 0);
    expect(memcmp(e, s.edid + block * DP_EDID_LEN, DP_EDID_LEN) == 0);
  }
  expect_eq(s.bad_header, 0);
}

static void edid_nack_releases_bus(void)
{
  struct sink s;
  init_sink(&s);
  s.nack_i2c = 1;
  uint8_t e[DP_EDID_LEN];
  expect(dp_read_edid(fake_xfer, &s, 0, e) < 0);
  expect_eq(s.stops, 1);
}

int owos_main_dpaux(void)
{
  test("DPCD reads span 16-byte AUX requests", dpcd_read);
  test("DPCD validity rejects floating and bogus lane counts", dpcd_validity);
  test("DEFER replies are retried then give up", defer_retried);
  test("short AUX replies continue until the EDID is complete", short_reads_continue);
  test("EDID over I2C-over-AUX ends the I2C transaction", edid_read);
  test("EDID extension blocks, including past the E-DDC segment boundary", edid_extension_blocks);
  test("EDID NACK releases the I2C bus", edid_nack_releases_bus);
  return 0;
}
