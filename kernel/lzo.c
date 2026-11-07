/* lzo.c: LZO1X decompressor written from the published bitstream description; every read and write is checked. */

#include "lzo.h"

#define NEED_IN(n)                                                                                                     \
  do {                                                                                                                 \
    if ((uint64_t)ip + (n) > slen)                                                                                     \
      return -1;                                                                                                       \
  } while (0)

#define COPY_LIT(n)                                                                                                    \
  do {                                                                                                                 \
    uint32_t n_ = (n);                                                                                                 \
    NEED_IN(n_);                                                                                                       \
    if ((uint64_t)op + n_ > dcap)                                                                                      \
      return -1;                                                                                                       \
    for (uint32_t k_ = 0; k_ < n_; k_++)                                                                               \
      dst[op++] = src[ip++];                                                                                           \
  } while (0)

/* Length extension: each zero byte adds 255, the final nonzero byte adds itself. */
static int lzo_ext_len(const uint8_t *src, uint32_t slen, uint32_t *ipp, uint32_t cap, uint32_t *out)
{
  uint32_t ip = *ipp, t = 0;
  for (;;) {
    if (ip >= slen)
      return -1;
    if (src[ip])
      break;
    t += 255;
    ip++;
    if (t > cap)
      return -1;
  }
  t += src[ip++];
  if (t > cap)
    return -1;
  *ipp = ip;
  *out = t;
  return 0;
}

int lzo1x_unpack(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen)
{
  uint32_t ip = 0, op = 0, state = 0;

  if (slen == 0)
    return -1;
  if (src[0] > 17) {
    uint32_t n = (uint32_t)src[ip++] - 17;
    COPY_LIT(n);
    state = n < 4 ? n : 4;
  }

  for (;;) {
    uint32_t t, mlen, dist, next, ext;
    NEED_IN(1);
    t = src[ip++];

    if (t < 16) {
      if (state == 0) {
        if (t == 0) {
          if (lzo_ext_len(src, slen, &ip, dcap, &ext) != 0)
            return -1;
          t = 15 + ext;
        }
        COPY_LIT(t + 3);
        state = 4;
        continue;
      }
      NEED_IN(1);
      uint32_t b1 = src[ip++];
      if (state == 4) {
        dist = 2049 + (t >> 2) + (b1 << 2);
        mlen = 3;
      } else {
        dist = 1 + (t >> 2) + (b1 << 2);
        mlen = 2;
      }
      next = t & 3;
    } else if (t >= 64) {
      NEED_IN(1);
      uint32_t b1 = src[ip++];
      dist = 1 + ((t >> 2) & 7) + (b1 << 3);
      mlen = (t >> 5) + 1;
      next = t & 3;
    } else if (t >= 32) {
      mlen = t & 31;
      if (mlen == 0) {
        if (lzo_ext_len(src, slen, &ip, dcap, &ext) != 0)
          return -1;
        mlen = 31 + ext;
      }
      mlen += 2;
      NEED_IN(2);
      uint32_t v = (uint32_t)src[ip] | ((uint32_t)src[ip + 1] << 8);
      ip += 2;
      dist = 1 + (v >> 2);
      next = v & 3;
    } else {
      mlen = t & 7;
      if (mlen == 0) {
        if (lzo_ext_len(src, slen, &ip, dcap, &ext) != 0)
          return -1;
        mlen = 7 + ext;
      }
      mlen += 2;
      NEED_IN(2);
      uint32_t v = (uint32_t)src[ip] | ((uint32_t)src[ip + 1] << 8);
      ip += 2;
      dist = ((t & 8) << 11) + (v >> 2);
      next = v & 3;
      if (dist == 0) {
        if (mlen != 3 || ip != slen)
          return -1;
        *dlen = op;
        return 0;
      }
      dist += 0x4000;
    }

    if (dist > op || (uint64_t)op + mlen > dcap)
      return -1;
    for (uint32_t k = 0; k < mlen; k++, op++)
      dst[op] = dst[op - dist];
    state = next;
    if (next)
      COPY_LIT(next);
  }
}
