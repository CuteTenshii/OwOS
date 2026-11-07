/* rar_blake2.c - BLAKE2s and BLAKE2sp (the RAR5 file hash), RFC 7693. */
#include "owos.h"
#include "rar_int.h"

static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

static const uint8_t sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint32_t le32(const uint8_t *p)
{
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void compress(struct b2s *s, const uint8_t *blk)
{
  uint32_t m[16], v[16];
  for (int i = 0; i < 16; i++)
    m[i] = le32(blk + 4 * i);
  for (int i = 0; i < 8; i++) {
    v[i] = s->h[i];
    v[i + 8] = iv[i];
  }
  v[12] ^= s->t[0];
  v[13] ^= s->t[1];
  v[14] ^= s->f[0];
  v[15] ^= s->f[1];
#define G(a, b, c, d, x, y)                                                                                            \
  do {                                                                                                                 \
    v[a] = v[a] + v[b] + (x);                                                                                          \
    v[d] = rotr(v[d] ^ v[a], 16);                                                                                      \
    v[c] = v[c] + v[d];                                                                                                \
    v[b] = rotr(v[b] ^ v[c], 12);                                                                                      \
    v[a] = v[a] + v[b] + (y);                                                                                          \
    v[d] = rotr(v[d] ^ v[a], 8);                                                                                       \
    v[c] = v[c] + v[d];                                                                                                \
    v[b] = rotr(v[b] ^ v[c], 7);                                                                                       \
  } while (0)
  for (int r = 0; r < 10; r++) {
    const uint8_t *z = sigma[r];
    G(0, 4, 8, 12, m[z[0]], m[z[1]]);
    G(1, 5, 9, 13, m[z[2]], m[z[3]]);
    G(2, 6, 10, 14, m[z[4]], m[z[5]]);
    G(3, 7, 11, 15, m[z[6]], m[z[7]]);
    G(0, 5, 10, 15, m[z[8]], m[z[9]]);
    G(1, 6, 11, 12, m[z[10]], m[z[11]]);
    G(2, 7, 8, 13, m[z[12]], m[z[13]]);
    G(3, 4, 9, 14, m[z[14]], m[z[15]]);
  }
#undef G
  for (int i = 0; i < 8; i++)
    s->h[i] ^= v[i] ^ v[i + 8];
}

static void b2s_init(struct b2s *s, uint32_t w2, uint32_t w3)
{
  memset(s, 0, sizeof *s);
  for (int i = 0; i < 8; i++)
    s->h[i] = iv[i];
  s->h[0] ^= 0x02080020u;
  s->h[2] ^= w2;
  s->h[3] ^= w3;
}

static void b2s_update(struct b2s *s, const uint8_t *p, size_t n)
{
  while (n) {
    if (s->buflen == 64) {
      s->t[0] += 64;
      if (s->t[0] < 64)
        s->t[1]++;
      compress(s, s->buf);
      s->buflen = 0;
    }
    size_t take = 64 - s->buflen;
    if (take > n)
      take = n;
    memcpy(s->buf + s->buflen, p, take);
    s->buflen += (uint32_t)take;
    p += take;
    n -= take;
  }
}

static void b2s_final(struct b2s *s, bool last_node, uint8_t out[32])
{
  s->t[0] += s->buflen;
  if (s->t[0] < s->buflen)
    s->t[1]++;
  s->f[0] = 0xffffffffu;
  if (last_node)
    s->f[1] = 0xffffffffu;
  memset(s->buf + s->buflen, 0, 64 - s->buflen);
  compress(s, s->buf);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)s->h[i];
    out[4 * i + 1] = (uint8_t)(s->h[i] >> 8);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 3] = (uint8_t)(s->h[i] >> 24);
  }
}

void b2sp_init(struct b2sp *s)
{
  for (uint32_t i = 0; i < 8; i++)
    b2s_init(&s->leaf[i], i, 32u << 24);
  b2s_init(&s->root, 0, (1u << 16) | (32u << 24));
  s->buflen = 0;
}

void b2sp_update(struct b2sp *s, const void *data, size_t len)
{
  const uint8_t *p = data;
  while (len) {
    size_t take = 512 - s->buflen;
    if (take > len)
      take = len;
    memcpy(s->buf + s->buflen, p, take);
    s->buflen += (uint32_t)take;
    p += take;
    len -= take;
    if (s->buflen == 512) {
      for (int i = 0; i < 8; i++)
        b2s_update(&s->leaf[i], s->buf + 64 * i, 64);
      s->buflen = 0;
    }
  }
}

void b2sp_final(struct b2sp *s, uint8_t out[32])
{
  uint8_t d[8][32];
  for (int i = 0; i < 8; i++) {
    int left = (int)s->buflen - 64 * i;
    if (left > 64)
      left = 64;
    if (left > 0)
      b2s_update(&s->leaf[i], s->buf + 64 * i, (size_t)left);
    b2s_final(&s->leaf[i], i == 7, d[i]);
  }
  for (int i = 0; i < 8; i++)
    b2s_update(&s->root, d[i], 32);
  b2s_final(&s->root, true, out);
}
