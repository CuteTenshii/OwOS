/* bt_aes.c: AES-128 block encryption and AES-CMAC (RFC 4493) for the LE security manager. */

#include "bt_int.h"

static uint8_t sbox[256];

static uint8_t rotl8(uint8_t x, int n) { return (uint8_t)(x << n | x >> (8 - n)); }

static uint8_t xtime(uint8_t x) { return (uint8_t)(x << 1 ^ (x >> 7) * 0x1B); }

/* S-box from the GF(2^8) inverse and the affine map. */
static void sbox_init(void)
{
  uint8_t p = 1, q = 1;
  do {
    p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
    q ^= (uint8_t)(q << 1);
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    if (q & 0x80)
      q ^= 0x09;
    sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
}

void bt_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
  uint8_t rk[176], s[16], t[16];
  if (!sbox[0])
    sbox_init();
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t w[4];
    memcpy(w, rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t first = w[0];
      w[0] = (uint8_t)(sbox[w[1]] ^ rcon);
      w[1] = sbox[w[2]];
      w[2] = sbox[w[3]];
      w[3] = sbox[first];
      rcon = xtime(rcon);
    }
    for (int j = 0; j < 4; j++)
      rk[i + j] = (uint8_t)(rk[i + j - 16] ^ w[j]);
  }

  for (int i = 0; i < 16; i++)
    s[i] = (uint8_t)(in[i] ^ rk[i]);
  for (int round = 1; round <= 10; round++) {
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        t[c * 4 + r] = sbox[s[(c + r) % 4 * 4 + r]];
    if (round < 10) {
      for (int c = 0; c < 4; c++) {
        const uint8_t *a = t + c * 4;
        s[c * 4] = (uint8_t)(xtime(a[0] ^ a[1]) ^ a[1] ^ a[2] ^ a[3]);
        s[c * 4 + 1] = (uint8_t)(xtime(a[1] ^ a[2]) ^ a[2] ^ a[3] ^ a[0]);
        s[c * 4 + 2] = (uint8_t)(xtime(a[2] ^ a[3]) ^ a[3] ^ a[0] ^ a[1]);
        s[c * 4 + 3] = (uint8_t)(xtime(a[3] ^ a[0]) ^ a[0] ^ a[1] ^ a[2]);
      }
    } else {
      memcpy(s, t, 16);
    }
    for (int i = 0; i < 16; i++)
      s[i] = (uint8_t)(s[i] ^ rk[round * 16 + i]);
  }
  memcpy(out, s, 16);
}

static void double_block(uint8_t b[16])
{
  uint8_t carry = b[0] >> 7;
  for (int i = 0; i < 15; i++)
    b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
  b[15] = (uint8_t)(b[15] << 1 ^ carry * 0x87);
}

void bt_cmac(const uint8_t key[16], const uint8_t *msg, int len, uint8_t out[16])
{
  uint8_t k1[16] = {0}, k2[16], x[16] = {0}, last[16] = {0};
  bt_aes128(key, k1, k1);
  double_block(k1);
  memcpy(k2, k1, 16);
  double_block(k2);

  int blocks = (len + 15) / 16;
  bool whole = len > 0 && len % 16 == 0;
  if (!blocks)
    blocks = 1;
  for (int b = 0; b < blocks - 1; b++) {
    for (int i = 0; i < 16; i++)
      x[i] ^= msg[b * 16 + i];
    bt_aes128(key, x, x);
  }
  int rest = len - (blocks - 1) * 16;
  if (rest)
    memcpy(last, msg + (blocks - 1) * 16, (uint32_t)rest);
  if (!whole)
    last[rest] = 0x80;
  for (int i = 0; i < 16; i++)
    x[i] ^= (uint8_t)(last[i] ^ (whole ? k1[i] : k2[i]));
  bt_aes128(key, x, out);
}
