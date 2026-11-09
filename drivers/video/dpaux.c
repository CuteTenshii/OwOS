/* dpaux.c: DisplayPort AUX channel protocol (DPCD reads, EDID over I2C-over-AUX). */

#include "dpaux.h"

#define AUX_NATIVE_READ 0x9
#define AUX_I2C_WRITE 0x0
#define AUX_I2C_READ 0x1
#define AUX_I2C_MOT 0x4 /* middle of transaction: keep the I2C bus held between requests */

#define AUX_CHUNK 16 /* the sink must support at least 16 data bytes per request */
#define AUX_RETRIES 7
#define EDID_ADDR 0x50
#define EDID_SEGMENT_ADDR 0x30

#define REPLY_NATIVE(r) ((r) >> 6)
#define REPLY_I2C(r) (((r) >> 4) & 3)
#define REPLY_ACK 0
#define REPLY_DEFER 2

/* One request: header (cmd, 20-bit addr, len - 1), then payload. Retries while the sink defers. */
static int request(dp_aux_fn xfer, void *ctx, int cmd, uint32_t addr, const uint8_t *data, int len, uint8_t *rx,
                   int rxmax)
{
  uint8_t tx[4 + AUX_CHUNK];
  tx[0] = (uint8_t)((cmd << 4) | ((addr >> 16) & 0xF));
  tx[1] = (uint8_t)(addr >> 8);
  tx[2] = (uint8_t)addr;
  int txlen = 3;
  if (len > 0) {
    tx[txlen++] = (uint8_t)(len - 1);
    for (int i = 0; data && i < len; i++)
      tx[txlen++] = data[i];
  }

  for (int try = 0; try < AUX_RETRIES; try ++) {
    int n = xfer(ctx, tx, txlen, rx, rxmax);
    if (n < 1)
      return -1;
    int native = REPLY_NATIVE(rx[0]);
    int i2c = (cmd & ~AUX_I2C_MOT) == AUX_NATIVE_READ ? REPLY_ACK : REPLY_I2C(rx[0]);
    if (native == REPLY_DEFER || i2c == REPLY_DEFER)
      continue;
    if (native != REPLY_ACK || i2c != REPLY_ACK)
      return -1;
    return n;
  }
  return -1;
}

int dp_read_dpcd(dp_aux_fn xfer, void *ctx, uint32_t addr, uint8_t *buf, int len)
{
  uint8_t rx[1 + AUX_CHUNK];
  int got = 0;
  while (got < len) {
    int want = len - got < AUX_CHUNK ? len - got : AUX_CHUNK;
    int n = request(xfer, ctx, AUX_NATIVE_READ, addr + (uint32_t)got, 0, want, rx, sizeof rx);
    if (n < 2)
      return -1;
    int take = n - 1 < want ? n - 1 : want;
    for (int i = 0; i < take; i++)
      buf[got + i] = rx[1 + i];
    got += take;
  }
  return 0;
}

int dp_dpcd_valid(const uint8_t caps[3])
{
  int lanes = caps[2] & 0x1F;
  return caps[0] >= 0x10 && caps[0] != 0xFF && (lanes == 1 || lanes == 2 || lanes == 4);
}

/* Address-only I2C write without MOT: releases the bus. */
static void i2c_stop(dp_aux_fn xfer, void *ctx)
{
  uint8_t rx[1 + AUX_CHUNK];
  request(xfer, ctx, AUX_I2C_WRITE, EDID_ADDR, 0, 0, rx, sizeof rx);
}

int dp_read_edid(dp_aux_fn xfer, void *ctx, int block, uint8_t *edid)
{
  uint8_t rx[1 + AUX_CHUNK];
  uint8_t segment = (uint8_t)(block / 2);
  uint8_t offset = (uint8_t)((block % 2) * DP_EDID_LEN);

  if (segment && request(xfer, ctx, AUX_I2C_WRITE | AUX_I2C_MOT, EDID_SEGMENT_ADDR, &segment, 1, rx, sizeof rx) < 0) {
    i2c_stop(xfer, ctx);
    return -1;
  }
  if (request(xfer, ctx, AUX_I2C_WRITE | AUX_I2C_MOT, EDID_ADDR, &offset, 1, rx, sizeof rx) < 0) {
    i2c_stop(xfer, ctx);
    return -1;
  }

  int got = 0;
  while (got < DP_EDID_LEN) {
    int want = DP_EDID_LEN - got < AUX_CHUNK ? DP_EDID_LEN - got : AUX_CHUNK;
    int cmd = AUX_I2C_READ | (got + want < DP_EDID_LEN ? AUX_I2C_MOT : 0);
    int n = request(xfer, ctx, cmd, EDID_ADDR, 0, want, rx, sizeof rx);
    if (n < 2) {
      i2c_stop(xfer, ctx);
      return -1;
    }
    int take = n - 1 < want ? n - 1 : want;
    for (int i = 0; i < take; i++)
      edid[got + i] = rx[1 + i];
    got += take;
  }
  return 0;
}
