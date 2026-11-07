#include "crc32c.h"

static uint32_t table[8][256];
static int ready;

static void build(void)
{
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    table[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++)
    for (int t = 1; t < 8; t++)
      table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
  ready = 1;
}

uint32_t crc32c_raw(uint32_t crc, const void *buf, uint32_t len)
{
  const uint8_t *p = buf;
  if (!ready)
    build();
  while (len >= 8) {
    uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    crc = table[7][a & 0xFF] ^ table[6][(a >> 8) & 0xFF] ^ table[5][(a >> 16) & 0xFF] ^ table[4][a >> 24] ^
          table[3][p[4]] ^ table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
    p += 8;
    len -= 8;
  }
  while (len--)
    crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 0xFF];
  return crc;
}
