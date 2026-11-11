/* transform.c — dequantisation and the inverse integer transforms (H.264 8.5).
 *
 * H.264 uses a 4x4 (and, in High profile, 8x8) integer transform, a fixed-point
 * approximation of the DCT, so reconstruction is bit-exact, unlike the MPEG-1
 * real-valued IDCT. Intra_16x16 luma and chroma additionally gather their block
 * DC terms into a separate array transformed by a Hadamard butterfly (4x4 for
 * luma, 2x2 for chroma). LevelScale = weightScale * normAdjust, where the weights
 * come from the (possibly default or transmitted) scaling lists.
 */
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

static const unsigned char def4[2][16] = {
    {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42},
    {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34},
};
static const unsigned char def8[2][64] = {
    {6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
     25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31,
     31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42},
    {9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22,
     22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27,
     27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35},
};

static const unsigned char norm8[6][6] = {
    {20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31},
    {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43},
};

/* 8x8 frame zig-zag scan: scan index -> raster position. */
static unsigned char zigzag8[64];

static void init_zigzag8(void)
{
  int s, r, n = 0;
  if (zigzag8[1])
    return;
  for (s = 0; s < 15; s++) {
    if (s & 1) {
      for (r = 0; r < 8; r++)
        if (s - r >= 0 && s - r < 8)
          zigzag8[n++] = (unsigned char)(r * 8 + s - r);
    } else {
      for (r = 7; r >= 0; r--)
        if (s - r >= 0 && s - r < 8)
          zigzag8[n++] = (unsigned char)(r * 8 + s - r);
    }
  }
}

const unsigned char *h264_zigzag8(void)
{
  init_zigzag8();
  return zigzag8;
}

static int class8(int i, int j)
{
  if (i % 4 == 0 && j % 4 == 0)
    return 0;
  if (i % 2 == 1 && j % 2 == 1)
    return 1;
  if (i % 4 == 2 && j % 4 == 2)
    return 2;
  if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0))
    return 3;
  if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0))
    return 4;
  return 5;
}

/* Resolve the scaling lists in effect (7.4.2.1.1 / 7.4.2.2 fall-back rules) and
 * expand them into the LevelScale tables used by the dequantisers. */
void h264_scaling_prepare(struct h264_decoder *d)
{
  const h264_sps *sps = d->cur_sps;
  const h264_pps *pps = d->cur_pps;
  unsigned char s4[6][16], s8[2][64], p4[6][16], p8[2][64];
  const unsigned char (*f4)[16] = s4;
  const unsigned char (*f8)[64] = s8;
  int i, m, k;
  const unsigned char *zz4 = h264_zigzag4, *zz8 = h264_zigzag8();

  memset(s4, 16, sizeof s4);
  memset(s8, 16, sizeof s8);
  if (sps->has_scaling) {
    for (i = 0; i < 8; i++) {
      const unsigned char *src = 0;
      unsigned char *dst = i < 6 ? s4[i] : s8[i - 6];
      int n = i < 6 ? 16 : 64;
      if (sps->scaling.present[i])
        src = sps->scaling.use_default[i] ? (i < 6 ? def4[i / 3] : def8[i - 6])
                                          : (i < 6 ? sps->scaling.list[i] : sps->scaling.list8[i - 6]);
      else if (i == 0 || i == 3)
        src = def4[i / 3];
      else if (i == 6 || i == 7)
        src = def8[i - 6];
      else
        src = s4[i - 1];
      memcpy(dst, src, (unsigned)n);
    }
  }
  if (pps->has_scaling) {
    for (i = 0; i < 8; i++) {
      const unsigned char *src;
      unsigned char *dst = i < 6 ? p4[i] : p8[i - 6];
      int n = i < 6 ? 16 : 64;
      if (pps->scaling.present[i])
        src = pps->scaling.use_default[i] ? (i < 6 ? def4[i / 3] : def8[i - 6])
                                          : (i < 6 ? pps->scaling.list[i] : pps->scaling.list8[i - 6]);
      else if (i == 0 || i == 3)
        src = sps->has_scaling ? s4[i] : def4[i / 3];
      else if (i == 6 || i == 7)
        src = sps->has_scaling ? s8[i - 6] : def8[i - 6];
      else
        src = p4[i - 1];
      memcpy(dst, src, (unsigned)n);
    }
    f4 = p4;
    f8 = p8;
  }

  for (i = 0; i < 6; i++) {
    int w[16];
    for (k = 0; k < 16; k++)
      w[zz4[k]] = f4[i][k];
    for (m = 0; m < 6; m++)
      for (k = 0; k < 16; k++)
        d->ls4[i][m][k] = w[k] * h264_normadjust[m][h264_dequant_class[k]];
  }
  for (i = 0; i < 2; i++) {
    int w[64];
    for (k = 0; k < 64; k++)
      w[zz8[k]] = f8[i][k];
    for (m = 0; m < 6; m++)
      for (k = 0; k < 64; k++)
        d->ls8[i][m][k] = w[k] * norm8[m][class8(k >> 3, k & 7)];
  }
}

void h264_dequant_4x4(int blk[16], int qp, const int *ls, int dc_removed)
{
  int qbits = qp / 6, r;
  for (r = dc_removed ? 1 : 0; r < 16; r++) {
    int v = blk[r];
    if (!v)
      continue;
    if (qbits >= 4)
      blk[r] = v * ls[r] * (1 << (qbits - 4));
    else
      blk[r] = (v * ls[r] + (1 << (3 - qbits))) >> (4 - qbits);
  }
}

void h264_dequant_8x8(int blk[64], int qp, const int *ls)
{
  int qbits = qp / 6, r;
  for (r = 0; r < 64; r++) {
    int v = blk[r];
    if (!v)
      continue;
    if (qbits >= 6)
      blk[r] = v * ls[r] * (1 << (qbits - 6));
    else
      blk[r] = (v * ls[r] + (1 << (5 - qbits))) >> (6 - qbits);
  }
}

void h264_dequant_luma_dc(int dc[16], int qp, int ls00)
{
  int shift = qp / 6, k;
  for (k = 0; k < 16; k++) {
    if (shift >= 6)
      dc[k] = dc[k] * ls00 * (1 << (shift - 6));
    else
      dc[k] = (dc[k] * ls00 + (1 << (5 - shift))) >> (6 - shift);
  }
}

void h264_dequant_chroma_dc(int dc[4], int qpc, int ls00)
{
  int shift = qpc / 6, k;
  for (k = 0; k < 4; k++)
    dc[k] = (dc[k] * ls00 * (1 << shift)) >> 5;
}

/* 4x4 Hadamard butterfly on the luma DC array (its own inverse; no scaling). */
void h264_hadamard4x4(int dc[16])
{
  int t[16], i;
  for (i = 0; i < 4; i++) {
    int *r = dc + i * 4;
    int e0 = r[0] + r[2], e1 = r[0] - r[2], e2 = r[1] + r[3], e3 = r[1] - r[3];
    t[i * 4 + 0] = e0 + e2;
    t[i * 4 + 1] = e1 + e3;
    t[i * 4 + 2] = e1 - e3;
    t[i * 4 + 3] = e0 - e2;
  }
  for (i = 0; i < 4; i++) {
    int e0 = t[0 * 4 + i] + t[2 * 4 + i], e1 = t[0 * 4 + i] - t[2 * 4 + i];
    int e2 = t[1 * 4 + i] + t[3 * 4 + i], e3 = t[1 * 4 + i] - t[3 * 4 + i];
    dc[0 * 4 + i] = e0 + e2;
    dc[1 * 4 + i] = e1 + e3;
    dc[2 * 4 + i] = e1 - e3;
    dc[3 * 4 + i] = e0 - e2;
  }
}

/* 2x2 Hadamard on the chroma DC array {c00,c01,c10,c11}. */
void h264_chroma_dc_transform(int dc[4])
{
  int a = dc[0] + dc[1], b = dc[0] - dc[1], c = dc[2] + dc[3], e = dc[2] - dc[3];
  dc[0] = a + c;
  dc[1] = b + e;
  dc[2] = a - c;
  dc[3] = b - e;
}

static inline unsigned char clip_add(unsigned char p, int r)
{
  int v = p + ((r + 32) >> 6);
  return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Inverse 4x4 core transform of dequantised coefficients `blk` (raster), added
 * to the prediction already in `dst` and clipped to 8 bits. */
void h264_idct4x4_add(const int blk[16], unsigned char *dst, int stride)
{
  int t[16], i, j;
  for (i = 0; i < 4; i++) {
    const int *r = blk + i * 4;
    int z0 = r[0] + r[2];
    int z1 = r[0] - r[2];
    int z2 = (r[1] >> 1) - r[3];
    int z3 = r[1] + (r[3] >> 1);
    t[i * 4 + 0] = z0 + z3;
    t[i * 4 + 1] = z1 + z2;
    t[i * 4 + 2] = z1 - z2;
    t[i * 4 + 3] = z0 - z3;
  }
  for (j = 0; j < 4; j++) {
    int z0 = t[0 * 4 + j] + t[2 * 4 + j];
    int z1 = t[0 * 4 + j] - t[2 * 4 + j];
    int z2 = (t[1 * 4 + j] >> 1) - t[3 * 4 + j];
    int z3 = t[1 * 4 + j] + (t[3 * 4 + j] >> 1);
    dst[0 * stride + j] = clip_add(dst[0 * stride + j], z0 + z3);
    dst[1 * stride + j] = clip_add(dst[1 * stride + j], z1 + z2);
    dst[2 * stride + j] = clip_add(dst[2 * stride + j], z1 - z2);
    dst[3 * stride + j] = clip_add(dst[3 * stride + j], z0 - z3);
  }
}

/* One 1-D pass of the 8-point inverse transform on in[0..7] (stride `is`). */
static void idct8_1d(const int *in, int is, int *out, int os)
{
  int d0 = in[0], d1 = in[is], d2 = in[2 * is], d3 = in[3 * is];
  int d4 = in[4 * is], d5 = in[5 * is], d6 = in[6 * is], d7 = in[7 * is];
  int e0 = d0 + d4, e1 = -d3 + d5 - d7 - (d7 >> 1);
  int e2 = d0 - d4, e3 = d1 + d7 - d3 - (d3 >> 1);
  int e4 = (d2 >> 1) - d6, e5 = -d1 + d7 + d5 + (d5 >> 1);
  int e6 = d2 + (d6 >> 1), e7 = d3 + d5 + d1 + (d1 >> 1);
  int f0 = e0 + e6, f1 = e1 + (e7 >> 2), f2 = e2 + e4, f3 = e3 + (e5 >> 2);
  int f4 = e2 - e4, f5 = (e3 >> 2) - e5, f6 = e0 - e6, f7 = e7 - (e1 >> 2);
  out[0] = f0 + f7;
  out[os] = f2 + f5;
  out[2 * os] = f4 + f3;
  out[3 * os] = f6 + f1;
  out[4 * os] = f6 - f1;
  out[5 * os] = f4 - f3;
  out[6 * os] = f2 - f5;
  out[7 * os] = f0 - f7;
}

void h264_idct8x8_add(const int blk[64], unsigned char *dst, int stride)
{
  int t[64], u[64], i, j;
  for (i = 0; i < 8; i++)
    idct8_1d(blk + i * 8, 1, t + i * 8, 1);
  for (j = 0; j < 8; j++)
    idct8_1d(t + j, 8, u + j, 8);
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      dst[i * stride + j] = clip_add(dst[i * stride + j], u[i * 8 + j]);
}
