/* rar_filter.c - the RAR post-filters (E8/E8E9, delta, ARM, Itanium, RGB, audio), run natively. */
#include "owos.h"
#include "rar_int.h"

#define E8_FILESIZE 0x1000000u

static int iabs(int v) { return v < 0 ? -v : v; }

static uint32_t get4(const uint8_t *p)
{
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put4(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void filter_e8(uint8_t *data, uint32_t len, uint32_t file_off, bool e9, bool v5)
{
  if (len < 4)
    return;
  uint8_t cmp2 = e9 ? 0xe9 : 0xe8;
  for (uint32_t cur = 0; cur + 4 < len;) {
    uint8_t b = data[cur++];
    if (b == 0xe8 || b == cmp2) {
      uint32_t offset = cur + file_off;
      if (v5)
        offset %= E8_FILESIZE;
      uint32_t addr = get4(data + cur);
      if (addr & 0x80000000u) {
        if (((addr + offset) & 0x80000000u) == 0)
          put4(data + cur, addr + E8_FILESIZE);
      } else if (((addr - E8_FILESIZE) & 0x80000000u) != 0)
        put4(data + cur, addr - offset);
      cur += 4;
    }
  }
}

static void filter_arm(uint8_t *data, uint32_t len, uint32_t file_off)
{
  for (uint32_t cur = 0; cur + 3 < len; cur += 4) {
    uint8_t *d = data + cur;
    if (d[3] == 0xeb) {
      uint32_t off = d[0] + (uint32_t)d[1] * 0x100 + (uint32_t)d[2] * 0x10000;
      off -= (file_off + cur) / 4;
      d[0] = (uint8_t)off;
      d[1] = (uint8_t)(off >> 8);
      d[2] = (uint8_t)(off >> 16);
    }
  }
}

static void filter_delta(const uint8_t *src, uint8_t *dst, uint32_t len, uint32_t channels)
{
  uint32_t sp = 0;
  for (uint32_t ch = 0; ch < channels; ch++) {
    uint8_t prev = 0;
    for (uint32_t dp = ch; dp < len; dp += channels)
      dst[dp] = (prev = (uint8_t)(prev - src[sp++]));
  }
}

static uint32_t get_bits(const uint8_t *d, uint32_t bitpos, unsigned count)
{
  uint32_t a = bitpos / 8, bit = bitpos & 7;
  uint32_t v = d[a] | ((uint32_t)d[a + 1] << 8) | ((uint32_t)d[a + 2] << 16) | ((uint32_t)d[a + 3] << 24);
  v >>= bit;
  return v & (0xffffffffu >> (32 - count));
}

static void set_bits(uint8_t *d, uint32_t v, uint32_t bitpos, unsigned count)
{
  uint32_t a = bitpos / 8, bit = bitpos & 7;
  uint32_t andm = 0xffffffffu >> (32 - count);
  andm = ~(andm << bit);
  v <<= bit;
  for (int i = 0; i < 4; i++) {
    d[a + i] &= (uint8_t)andm;
    d[a + i] |= (uint8_t)v;
    andm = (andm >> 8) | 0xff000000u;
    v >>= 8;
  }
}

static void filter_itanium(uint8_t *data, uint32_t len, uint32_t file_off)
{
  static const uint8_t masks[16] = {4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
  if (len < 21)
    return;
  file_off >>= 4;
  for (uint32_t cur = 0; cur < len - 21; cur += 16, data += 16, file_off++) {
    int b = (data[0] & 0x1f) - 0x10;
    if (b < 0)
      continue;
    uint8_t m = masks[b];
    if (!m)
      continue;
    for (int i = 0; i <= 2; i++) {
      if (!(m & (1 << i)))
        continue;
      uint32_t start = (uint32_t)i * 41 + 5;
      if (get_bits(data, start + 37, 4) == 5) {
        uint32_t off = get_bits(data, start + 13, 20);
        set_bits(data, (off - file_off) & 0xfffff, start + 13, 20);
      }
    }
  }
}

static void filter_rgb(const uint8_t *src, uint8_t *dst, uint32_t len, uint32_t width, uint32_t pos_r)
{
  const uint32_t channels = 3;
  for (uint32_t ch = 0; ch < channels; ch++) {
    uint32_t prev = 0;
    for (uint32_t i = ch; i < len; i += channels) {
      uint32_t pred;
      if (i >= width + 3) {
        const uint8_t *up = dst + i - width;
        uint32_t ub = up[0], ulb = up[-3];
        pred = prev + ub - ulb;
        int pa = iabs((int)(pred - prev)), pb = iabs((int)(pred - ub)), pc = iabs((int)(pred - ulb));
        if (pa <= pb && pa <= pc)
          pred = prev;
        else if (pb <= pc)
          pred = ub;
        else
          pred = ulb;
      } else
        pred = prev;
      dst[i] = (uint8_t)(prev = (uint8_t)(pred - *src++));
    }
  }
  for (uint32_t i = pos_r; i + 2 < len && i < len - 2; i += 3) {
    uint8_t g = dst[i + 1];
    dst[i] = (uint8_t)(dst[i] + g);
    dst[i + 2] = (uint8_t)(dst[i + 2] + g);
  }
}

static void filter_audio(const uint8_t *src, uint8_t *dst, uint32_t len, uint32_t channels)
{
  for (uint32_t ch = 0; ch < channels; ch++) {
    uint32_t prev_byte = 0, prev_delta = 0, dif[7] = {0};
    int d1 = 0, d2 = 0, d3, k1 = 0, k2 = 0, k3 = 0;
    uint32_t count = 0;
    for (uint32_t i = ch; i < len; i += channels, count++) {
      d3 = d2;
      d2 = (int)(prev_delta - (uint32_t)d1);
      d1 = (int)prev_delta;
      uint32_t pred = 8 * prev_byte + (uint32_t)(k1 * d1) + (uint32_t)(k2 * d2) + (uint32_t)(k3 * d3);
      pred = (pred >> 3) & 0xff;
      uint32_t cur = *src++;
      pred -= cur;
      dst[i] = (uint8_t)pred;
      prev_delta = (uint32_t)(int32_t)(int8_t)(uint8_t)(pred - prev_byte);
      prev_byte = pred & 0xff;
      int d = (int)(int8_t)cur * 8;
      dif[0] += (uint32_t)iabs(d);
      dif[1] += (uint32_t)iabs(d - d1);
      dif[2] += (uint32_t)iabs(d + d1);
      dif[3] += (uint32_t)iabs(d - d2);
      dif[4] += (uint32_t)iabs(d + d2);
      dif[5] += (uint32_t)iabs(d - d3);
      dif[6] += (uint32_t)iabs(d + d3);
      if ((count & 0x1f) == 0) {
        uint32_t min_dif = dif[0], num_min = 0;
        dif[0] = 0;
        for (uint32_t j = 1; j < 7; j++) {
          if (dif[j] < min_dif) {
            min_dif = dif[j];
            num_min = j;
          }
          dif[j] = 0;
        }
        switch (num_min) {
        case 1:
          if (k1 >= -16)
            k1--;
          break;
        case 2:
          if (k1 < 16)
            k1++;
          break;
        case 3:
          if (k2 >= -16)
            k2--;
          break;
        case 4:
          if (k2 < 16)
            k2++;
          break;
        case 5:
          if (k3 >= -16)
            k3--;
          break;
        case 6:
          if (k3 < 16)
            k3++;
          break;
        }
      }
    }
  }
}

uint32_t rar_filter_max_len(uint8_t type)
{
  switch (type) {
  case FT_E8:
  case FT_E8E9:
  case FT_ITANIUM:
    return 0x3C000;
  case FT_ARM:
  case FT_DELTA:
    return 0x400000;
  default:
    return 0x1E000;
  }
}

const uint8_t *rar_filter_run(const struct lzfilter *f, uint8_t *buf, uint8_t *scratch, uint32_t len, uint32_t file_off)
{
  switch (f->type) {
  case FT_E8:
  case FT_E8E9:
    filter_e8(buf, len, file_off, f->type == FT_E8E9, f->v5);
    return buf;
  case FT_ARM:
    filter_arm(buf, len, file_off);
    return buf;
  case FT_ITANIUM:
    filter_itanium(buf, len, file_off);
    return buf;
  case FT_DELTA:
    if (f->r[0] == 0 || f->r[0] > 32)
      return NULL;
    filter_delta(buf, scratch, len, f->r[0]);
    return scratch;
  case FT_RGB:
    if (len < 3 || f->r[0] < 3)
      return NULL;
    filter_rgb(buf, scratch, len, f->r[0] - 3, f->r[1]);
    return scratch;
  case FT_AUDIO:
    if (f->r[0] == 0 || f->r[0] > 32)
      return NULL;
    filter_audio(buf, scratch, len, f->r[0]);
    return scratch;
  }
  return buf;
}
