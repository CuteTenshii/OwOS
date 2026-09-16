/* cavlc.c — Context-Adaptive Variable-Length residual decoding (H.264 9.2).
 *
 * One 4x4 (or 2x2 chroma DC) block's coefficients are coded as: a coeff_token
 * giving TotalCoeff and TrailingOnes (its VLC table chosen by the neighbour-
 * derived context nC); the trailing-one signs; the remaining levels via an
 * adaptive prefix/suffix code; total_zeros; and run_before gaps. The pieces are
 * reassembled into coefficients indexed by scan position, which the caller maps
 * to raster with the zig-zag before dequant + inverse transform.
 */
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

static int vlc_match(bitr *b, const unsigned char *len, const unsigned char *bits, int n, int maxlen)
{
  unsigned code = 0;
  int l, i;
  for (l = 1; l <= maxlen; l++) {
    if (b->overrun)
      return -1;
    code = (code << 1) | br_u1(b);
    for (i = 0; i < n; i++)
      if (len[i] == (unsigned char)l && bits[i] == code)
        return i;
  }
  return -1;
}

/* Read coeff_token, returning TotalCoeff via *tc and TrailingOnes via *t1. */
static int read_coeff_token(bitr *b, int nC, int *tc, int *t1)
{
  int idx;
  if (nC == -1) {
    idx = vlc_match(b, h264_chroma_dc_ct_len, h264_chroma_dc_ct_bits, 20, 8);
  } else if (nC >= 8) {
    unsigned code = br_u(b, 6);
    if (code == 3) {
      *tc = 0;
      *t1 = 0;
      return 0;
    }
    *tc = (int)(code >> 2) + 1;
    *t1 = (int)(code & 3);
    return 0;
  } else {
    int g = nC < 2 ? 0 : nC < 4 ? 1 : 2;
    idx = vlc_match(b, h264_coeff_token_len[g], h264_coeff_token_bits[g], 68, 16);
  }
  if (idx < 0)
    return -1;
  *tc = idx >> 2;
  *t1 = idx & 3;
  return 0;
}

static int iabs(int v) { return v < 0 ? -v : v; }

int h264_cavlc_block(bitr *b, int nC, int max, int *coeff)
{
  int tc = 0, t1 = 0, i, level[16], run[16], total_zeros = 0, zeros_left, coeff_num;
  int suffix_len;

  for (i = 0; i < max; i++)
    coeff[i] = 0;

  if (read_coeff_token(b, nC, &tc, &t1) != 0)
    return -1;
  if (tc < 0 || tc > max || t1 > tc)
    return -1;
  if (tc == 0)
    return 0;

  /* Levels: TrailingOnes are ±1; the rest use the adaptive prefix/suffix code.
   */
  suffix_len = (tc > 10 && t1 < 3) ? 1 : 0;
  for (i = 0; i < tc; i++) {
    if (i < t1) {
      level[i] = br_u1(b) ? -1 : 1;
    } else {
      int lp = 0, ssize, lsuffix, level_code;
      while (br_u1(b) == 0) {
        if (++lp > 30 || b->overrun)
          return -1;
      }
      if (lp == 14 && suffix_len == 0)
        ssize = 4;
      else if (lp >= 15)
        ssize = lp - 3;
      else
        ssize = suffix_len;
      lsuffix = ssize > 0 ? (int)br_u(b, ssize) : 0;
      level_code = ((lp < 15 ? lp : 15) << suffix_len) + lsuffix;
      if (lp >= 15 && suffix_len == 0)
        level_code += 15;
      if (lp >= 16)
        level_code += (1 << (lp - 3)) - 4096;
      if (i == t1 && t1 < 3)
        level_code += 2;
      level[i] = (level_code & 1) ? ((-level_code - 1) >> 1) : ((level_code + 2) >> 1);
      if (suffix_len == 0)
        suffix_len = 1;
      if (iabs(level[i]) > (3 << (suffix_len - 1)) && suffix_len < 6)
        suffix_len++;
    }
  }

  /* total_zeros: the number of zeros between the last coefficient and the DC.
   */
  if (tc < max) {
    int tz;
    if (nC == -1)
      tz = vlc_match(b, h264_chroma_dc_tz_len[tc - 1], h264_chroma_dc_tz_bits[tc - 1], 4, 3);
    else
      tz = vlc_match(b, h264_total_zeros_len[tc - 1], h264_total_zeros_bits[tc - 1], 16, 9);
    if (tz < 0)
      return -1;
    total_zeros = tz;
  }

  /* run_before: the gap of zeros preceding each coefficient. */
  for (i = 0; i < tc; i++)
    run[i] = 0;
  zeros_left = total_zeros;
  for (i = 0; i < tc - 1; i++) {
    if (zeros_left > 0) {
      int ridx = (zeros_left > 6 ? 7 : zeros_left) - 1;
      int rb = vlc_match(b, h264_run_len[ridx], h264_run_bits[ridx], 16, 11);
      if (rb < 0 || rb > zeros_left)
        return -1;
      run[i] = rb;
      zeros_left -= rb;
    }
  }
  run[tc - 1] = zeros_left;

  /* Combine level + run into scan-position-indexed coefficients (9.2.4). */
  coeff_num = -1;
  for (i = tc - 1; i >= 0; i--) {
    coeff_num += run[i] + 1;
    if (coeff_num < 0 || coeff_num >= max)
      return -1;
    coeff[coeff_num] = level[i];
  }
  return tc;
}
