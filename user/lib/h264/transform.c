/* transform.c — dequantisation and the inverse integer transforms (H.264 8.5).
 *
 * H.264 uses a 4x4 integer transform (a fixed-point approximation of the DCT)
 * so reconstruction is bit-exact, unlike the MPEG-1 real-valued IDCT.
 * Intra_16x16 luma and chroma additionally gather their block DC terms into a
 * separate array transformed by a Hadamard butterfly (4x4 for luma, 2x2 for
 * chroma). Flat scaling only (no scaling lists): LevelScale = 16 * normAdjust.
 */
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

static inline int ls4(int m, int r) { return 16 * (int)h264_normadjust[m][h264_dequant_class[r]]; }

void h264_dequant_4x4(int blk[16], int qp, int dc_removed)
{
  int qbits = qp / 6, m = qp % 6, r;
  for (r = dc_removed ? 1 : 0; r < 16; r++) {
    int d = blk[r];
    if (!d)
      continue;
    if (qbits >= 4)
      blk[r] = (d * ls4(m, r)) << (qbits - 4);
    else
      blk[r] = (d * ls4(m, r) + (1 << (3 - qbits))) >> (4 - qbits);
  }
}

void h264_dequant_luma_dc(int dc[16], int qp)
{
  int scale = 16 * (int)h264_normadjust[qp % 6][0], shift = qp / 6, k;
  for (k = 0; k < 16; k++) {
    if (shift >= 6)
      dc[k] = (dc[k] * scale) << (shift - 6);
    else
      dc[k] = (dc[k] * scale + (1 << (5 - shift))) >> (6 - shift);
  }
}

void h264_dequant_chroma_dc(int dc[4], int qpc)
{
  int scale = 16 * (int)h264_normadjust[qpc % 6][0], shift = qpc / 6, k;
  for (k = 0; k < 4; k++)
    dc[k] = ((dc[k] * scale) << shift) >> 5;
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
    int a[4], y;
    a[0] = z0 + z3;
    a[1] = z1 + z2;
    a[2] = z1 - z2;
    a[3] = z0 - z3;
    for (y = 0; y < 4; y++) {
      int v = dst[y * stride + j] + ((a[y] + 32) >> 6);
      dst[y * stride + j] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}
