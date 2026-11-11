/* cabac.c — CABAC entropy decoding (H.264 9.3): the arithmetic decoding engine,
 * context initialisation, and the binarisations of the context-coded syntax
 * elements used by I/P slices (frame macroblocks, 4:2:0).
 *
 * Neighbour-dependent context increments (ctxIdxInc) are derived by slice.c,
 * which owns the per-macroblock state; this file only turns bins into values.
 */
#include "cabac_tables.h"
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

static void engine_start(h264_cabac *c)
{
  c->range = 510;
  c->offset = br_u(c->br, 9);
}

void h264_cabac_start(h264_cabac *c) { engine_start(c); }

void h264_cabac_init(struct h264_decoder *d)
{
  h264_cabac *c = &d->cabac;
  int qp = d->sh.qp < 0 ? 0 : d->sh.qp > 51 ? 51 : d->sh.qp, i;
  const signed char (*t)[2] = d->sh.slice_type == H264_SLICE_I ? cabac_init_i : cabac_init_pb[d->sh.cabac_init_idc];

  for (i = 0; i < 460; i++) {
    int pre = ((t[i][0] * qp) >> 4) + t[i][1];
    pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
    c->st[i] = pre <= 63 ? (unsigned char)((63 - pre) << 1) : (unsigned char)(((pre - 64) << 1) | 1);
  }
  c->br = &d->br;
  c->dqp_nz = 0;
  br_align(c->br);
  engine_start(c);
}

int h264_cabac_decision(h264_cabac *c, int ctx)
{
  unsigned char *s = &c->st[ctx];
  unsigned p = *s >> 1, mps = *s & 1, bin, lps = cabac_range_lps[p][(c->range >> 6) & 3];

  c->range -= lps;
  if (c->offset >= c->range) {
    bin = !mps;
    c->offset -= c->range;
    c->range = lps;
    if (p == 0)
      mps = !mps;
    p = cabac_trans_lps[p];
  } else {
    bin = mps;
    if (p < 62)
      p++;
  }
  *s = (unsigned char)((p << 1) | mps);
  if (c->range < 256) {
    int n = __builtin_clz(c->range) - 23;
    c->range <<= n;
    c->offset = (c->offset << n) | br_u(c->br, n);
  }
  return (int)bin;
}

static int bypass(h264_cabac *c)
{
  c->offset = (c->offset << 1) | br_u1(c->br);
  if (c->offset >= c->range) {
    c->offset -= c->range;
    return 1;
  }
  return 0;
}

int h264_cabac_terminate(h264_cabac *c)
{
  c->range -= 2;
  if (c->offset >= c->range)
    return 1;
  if (c->range < 256) {
    c->range <<= 1;
    c->offset = (c->offset << 1) | br_u1(c->br);
  }
  return 0;
}

/* k-th order Exp-Golomb suffix, bypass coded. */
static int exp_golomb(h264_cabac *c, int k)
{
  int v = 0;
  while (bypass(c)) {
    v += 1 << k;
    if (++k > 24) {
      c->br->overrun = 1;
      return 0;
    }
  }
  while (k-- > 0)
    v += bypass(c) << k;
  return v;
}

/* Bins after the I_PCM/terminate bin of an intra mb_type; `off` is ctxIdxOffset
 * (3 in I slices, 17 for the intra suffix in P slices). Returns 1..24. */
static int mb_type_i16(h264_cabac *c, int off)
{
  int i_slice = off == 3, luma, chroma = 0, pm;
  luma = h264_cabac_decision(c, off + (i_slice ? 3 : 1));
  if (h264_cabac_decision(c, off + (i_slice ? 4 : 2)))
    chroma = 1 + h264_cabac_decision(c, off + (i_slice ? 5 : 2));
  pm = h264_cabac_decision(c, off + (i_slice ? 6 : 3)) << 1;
  pm |= h264_cabac_decision(c, off + (i_slice ? 7 : 3));
  return 1 + pm + 4 * chroma + 12 * luma;
}

int h264_cabac_mb_type_i(h264_cabac *c, int inc)
{
  if (!h264_cabac_decision(c, 3 + inc))
    return 0;
  if (h264_cabac_terminate(c))
    return 25;
  return mb_type_i16(c, 3);
}

int h264_cabac_mb_type_p(h264_cabac *c)
{
  if (h264_cabac_decision(c, 14)) {
    if (!h264_cabac_decision(c, 17))
      return 5;
    if (h264_cabac_terminate(c))
      return 5 + 25;
    return 5 + mb_type_i16(c, 17);
  }
  if (!h264_cabac_decision(c, 15))
    return h264_cabac_decision(c, 16) ? 3 : 0;
  return h264_cabac_decision(c, 17) ? 1 : 2;
}

int h264_cabac_sub_mb_type(h264_cabac *c)
{
  if (h264_cabac_decision(c, 21))
    return 0;
  if (!h264_cabac_decision(c, 22))
    return 1;
  return h264_cabac_decision(c, 23) ? 2 : 3;
}

int h264_cabac_ref_idx(h264_cabac *c, int inc)
{
  int v = 0, ctx = 54 + inc;
  while (h264_cabac_decision(c, ctx)) {
    ctx = v == 0 ? 54 + 4 : 54 + 5;
    if (++v > 32) {
      c->br->overrun = 1;
      break;
    }
  }
  return v;
}

int h264_cabac_mvd(h264_cabac *c, int comp, int sum)
{
  static const unsigned char bin_inc[8] = {3, 4, 5, 6, 6, 6, 6, 6};
  int base = comp ? 47 : 40, v = 0;
  if (!h264_cabac_decision(c, base + (sum < 3 ? 0 : sum > 32 ? 2 : 1)))
    return 0;
  v = 1;
  while (v < 9 && h264_cabac_decision(c, base + bin_inc[v - 1]))
    v++;
  if (v == 9)
    v += exp_golomb(c, 3);
  return bypass(c) ? -v : v;
}

int h264_cabac_qp_delta(h264_cabac *c)
{
  int v = 0, ctx = 60 + (c->dqp_nz ? 1 : 0);
  while (h264_cabac_decision(c, ctx)) {
    ctx = v == 0 ? 62 : 63;
    if (++v > 104) {
      c->br->overrun = 1;
      break;
    }
  }
  return (v & 1) ? (v + 1) >> 1 : -(v >> 1);
}

int h264_cabac_chroma_pred_mode(h264_cabac *c, int inc)
{
  if (!h264_cabac_decision(c, 64 + inc))
    return 0;
  if (!h264_cabac_decision(c, 64 + 3))
    return 1;
  return 2 + h264_cabac_decision(c, 64 + 3);
}

int h264_cabac_rem_intra4x4(h264_cabac *c)
{
  int v = h264_cabac_decision(c, 69);
  v |= h264_cabac_decision(c, 69) << 1;
  v |= h264_cabac_decision(c, 69) << 2;
  return v;
}

/* One residual block. `cat` is ctxBlockCat (0 Intra16x16 DC, 1 Intra16x16 AC,
 * 2 luma 4x4, 3 chroma DC, 4 chroma AC); coeff[0..max-1] receives the levels in
 * scan order. Returns the number of non-zero coefficients. */
int h264_cabac_residual(h264_cabac *c, int cat, int cbf_inc, int max, int *coeff)
{
  static const unsigned char cbf_off[5] = {0, 4, 8, 12, 16};
  static const unsigned char sig_off[5] = {0, 15, 29, 44, 47};
  static const unsigned char abs_off[5] = {0, 10, 20, 30, 39};
  int sig_base = 105 + sig_off[cat], last_base = 166 + sig_off[cat], abs_base = 227 + abs_off[cat];
  int pos[16], n = 0, i, k, eq1 = 0, gt1 = 0, last = 0;

  for (i = 0; i < max; i++)
    coeff[i] = 0;
  if (!h264_cabac_decision(c, 85 + cbf_off[cat] + cbf_inc))
    return 0;

  for (i = 0; i < max - 1 && !last; i++) {
    int inc = cat == 3 ? (i < 2 ? i : 2) : i;
    if (h264_cabac_decision(c, sig_base + inc)) {
      pos[n++] = i;
      last = h264_cabac_decision(c, last_base + inc);
    }
  }
  if (!last)
    pos[n++] = max - 1;

  for (k = n - 1; k >= 0; k--) {
    int abs = 1, ctx = abs_base + (gt1 ? 0 : (eq1 + 1 < 4 ? eq1 + 1 : 4));
    if (h264_cabac_decision(c, ctx)) {
      int lim = 4 - (cat == 3), cap = gt1 < lim ? gt1 : lim;
      ctx = abs_base + 5 + cap;
      abs = 2;
      while (abs < 15 && h264_cabac_decision(c, ctx))
        abs++;
      if (abs == 15)
        abs += exp_golomb(c, 0);
      gt1++;
    } else {
      eq1++;
    }
    coeff[pos[k]] = bypass(c) ? -abs : abs;
  }
  return n;
}
