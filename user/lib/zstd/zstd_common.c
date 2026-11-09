/* zstd_common.c: the constant tables shared by the FSE/Huffman/sequence code
 * plus the XXH64 content-checksum hash. Values are the RFC 8878 defaults. */
#include "zstd_internal.h"

const u32 LL_base[MAX_LL + 1] = {0,  1,  2,   3,   4,   5,    6,    7,    8,    9,     10,    11,
                                 12, 13, 14,  15,  16,  18,   20,   22,   24,   28,    32,    40,
                                 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const u8 LL_bits[MAX_LL + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
                                1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const u32 ML_base[MAX_ML + 1] = {3,  4,   5,   6,   7,    8,    9,    10,   11,    12,    13,   14, 15, 16,
                                 17, 18,  19,  20,  21,   22,   23,   24,   25,    26,    27,   28, 29, 30,
                                 31, 32,  33,  34,  35,   37,   39,   41,   43,    47,    51,   59, 67, 83,
                                 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const u8 ML_bits[MAX_ML + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
                                0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

const short LL_defaultNorm[MAX_LL + 1] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
                                          2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const short ML_defaultNorm[MAX_ML + 1] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1,  1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const short OF_defaultNorm[DEF_MAX_OFF + 1] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
                                               1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

/* ---- XXH64 (Yann Collet), seed-parameterized ----------------------------- */
#define XXH_P1 11400714785074694791ull
#define XXH_P2 14029467366897019727ull
#define XXH_P3 1609587929392839161ull
#define XXH_P4 9650029242287828579ull
#define XXH_P5 2870177450012600261ull

static inline u64 xxh_rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static inline u64 xxh_round(u64 acc, u64 in)
{
  acc += in * XXH_P2;
  acc = xxh_rotl(acc, 31);
  acc *= XXH_P1;
  return acc;
}
static inline u64 xxh_merge(u64 acc, u64 val)
{
  val = xxh_round(0, val);
  acc ^= val;
  acc = acc * XXH_P1 + XXH_P4;
  return acc;
}

u64 xxh64(const void *input, u32 len, u64 seed)
{
  const u8 *p = (const u8 *)input;
  const u8 *end = p + len;
  u64 h64;

  if (len >= 32) {
    const u8 *limit = end - 32;
    u64 v1 = seed + XXH_P1 + XXH_P2;
    u64 v2 = seed + XXH_P2;
    u64 v3 = seed + 0;
    u64 v4 = seed - XXH_P1;
    do {
      v1 = xxh_round(v1, zstd_rd64(p));
      p += 8;
      v2 = xxh_round(v2, zstd_rd64(p));
      p += 8;
      v3 = xxh_round(v3, zstd_rd64(p));
      p += 8;
      v4 = xxh_round(v4, zstd_rd64(p));
      p += 8;
    } while (p <= limit);
    h64 = xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) + xxh_rotl(v4, 18);
    h64 = xxh_merge(h64, v1);
    h64 = xxh_merge(h64, v2);
    h64 = xxh_merge(h64, v3);
    h64 = xxh_merge(h64, v4);
  } else {
    h64 = seed + XXH_P5;
  }
  h64 += (u64)len;

  while (p + 8 <= end) {
    u64 k1 = xxh_round(0, zstd_rd64(p));
    h64 ^= k1;
    h64 = xxh_rotl(h64, 27) * XXH_P1 + XXH_P4;
    p += 8;
  }
  if (p + 4 <= end) {
    h64 ^= (u64)zstd_rd32(p) * XXH_P1;
    h64 = xxh_rotl(h64, 23) * XXH_P2 + XXH_P3;
    p += 4;
  }
  while (p < end) {
    h64 ^= (u64)(*p) * XXH_P5;
    h64 = xxh_rotl(h64, 11) * XXH_P1;
    p++;
  }
  h64 ^= h64 >> 33;
  h64 *= XXH_P2;
  h64 ^= h64 >> 29;
  h64 *= XXH_P3;
  h64 ^= h64 >> 32;
  return h64;
}
