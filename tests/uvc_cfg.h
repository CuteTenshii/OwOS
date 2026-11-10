/* uvc_cfg.h: builders for synthetic USB Video Class configuration descriptors, shared by the UVC suites. */
#ifndef OWOS_TEST_UVC_CFG_H
#define OWOS_TEST_UVC_CFG_H

#include <stdint.h>
#include <string.h>

struct blob {
  uint8_t b[1024];
  int n;
};

static inline void put(struct blob *o, const void *d, int n)
{
  memcpy(o->b + o->n, d, (size_t)n);
  o->n += n;
}

static inline void le(uint8_t *p, uint32_t v, int bytes)
{
  for (int i = 0; i < bytes; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static inline void iface(struct blob *o, int num, int alt, int neps, int sub)
{
  uint8_t d[9] = {9, 4, (uint8_t)num, (uint8_t)alt, (uint8_t)neps, 14, (uint8_t)sub, 0, 0};
  put(o, d, 9);
}

static inline void vc_header(struct blob *o, int bcd)
{
  uint8_t d[13] = {13, 0x24, 1, (uint8_t)bcd, (uint8_t)(bcd >> 8), 0, 0, 0, 0, 0, 0, 0, 1};
  put(o, d, 13);
}

static inline void format_mjpeg(struct blob *o, int index, int nframes)
{
  uint8_t d[11] = {11, 0x24, 6, (uint8_t)index, (uint8_t)nframes, 1, 1, 0, 0, 0, 0};
  put(o, d, 11);
}

static inline void format_uncompressed(struct blob *o, int index, int nframes, const char *guid4)
{
  static const uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  uint8_t d[27] = {27, 0x24, 4, (uint8_t)index, (uint8_t)nframes};
  memcpy(d + 5, guid4, 4);
  memcpy(d + 9, tail, 12);
  d[21] = 16;
  d[22] = 1;
  put(o, d, 27);
}

/* ivals: discrete list, or NULL with lo/hi for a continuous range */
static inline void frame(struct blob *o, int sub, int index, int w, int h, const uint32_t *ivals, int nival,
                         uint32_t lo, uint32_t hi, uint32_t max_bytes)
{
  uint8_t d[64] = {0};
  int len = nival ? 26 + 4 * nival : 38;
  d[0] = (uint8_t)len;
  d[1] = 0x24;
  d[2] = (uint8_t)sub;
  d[3] = (uint8_t)index;
  le(d + 5, (uint32_t)w, 2);
  le(d + 7, (uint32_t)h, 2);
  le(d + 17, max_bytes, 4);
  le(d + 21, ivals ? ivals[0] : lo, 4);
  d[25] = (uint8_t)nival;
  if (nival) {
    for (int i = 0; i < nival; i++)
      le(d + 26 + 4 * i, ivals[i], 4);
  } else {
    le(d + 26, lo, 4);
    le(d + 30, hi, 4);
    le(d + 34, 10000, 4);
  }
  put(o, d, len);
}

static inline void iso_ep(struct blob *o, int addr, int wmax, int binterval)
{
  uint8_t d[7] = {7, 5, (uint8_t)addr, 1, (uint8_t)wmax, (uint8_t)(wmax >> 8), (uint8_t)binterval};
  put(o, d, 7);
}

static inline void bulk_ep(struct blob *o, int addr, int wmax)
{
  uint8_t d[7] = {7, 5, (uint8_t)addr, 2, (uint8_t)wmax, (uint8_t)(wmax >> 8), 0};
  put(o, d, 7);
}

static inline void ss_companion(struct blob *o, int burst, int attrs, int bytes_per_interval)
{
  uint8_t d[6] = {
      6, 0x30, (uint8_t)burst, (uint8_t)attrs, (uint8_t)bytes_per_interval, (uint8_t)(bytes_per_interval >> 8)};
  put(o, d, 6);
}

static const uint32_t fps30_15[] = {333333, 666666};
static const uint32_t fps30[] = {333333};

static inline void camera(struct blob *o)
{
  memset(o, 0, sizeof *o);
  uint8_t cfg[9] = {9, 2, 0, 0, 2, 1, 0, 0x80, 50};
  put(o, cfg, 9);
  iface(o, 0, 0, 0, 1);
  vc_header(o, 0x0110);
  iface(o, 1, 0, 0, 2);
  format_mjpeg(o, 1, 2);
  frame(o, 7, 1, 640, 480, fps30_15, 2, 0, 0, 640 * 480 * 2);
  frame(o, 7, 2, 1280, 720, NULL, 0, 333333, 2000000, 0);
  format_uncompressed(o, 2, 1, "YUY2");
  frame(o, 5, 1, 640, 480, fps30, 1, 0, 0, 640 * 480 * 2);
  iface(o, 1, 1, 1, 2);
  iso_ep(o, 0x81, 128, 1);
  iface(o, 1, 2, 1, 2);
  iso_ep(o, 0x81, 512, 1);
  iface(o, 1, 3, 1, 2);
  iso_ep(o, 0x81, 1023, 1);
}

#endif
