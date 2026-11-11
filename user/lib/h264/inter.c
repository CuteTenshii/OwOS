/* inter.c — inter prediction (H.264 8.4): motion-vector prediction, direct
 * modes, fractional-sample interpolation and (weighted) bi-prediction.
 *
 * Luma uses the separable 6-tap half-pel filter (1,-5,20,20,-5,1) with
 * quarter-pel positions formed by averaging the two nearest half/full samples;
 * chroma uses bilinear 1/8-pel interpolation. Reference planes are
 * edge-extended (H264_BORDER); a block whose filter window reaches past that
 * border is fetched through a clamped copy instead.
 *
 * Motion lives in the picture (h264_mot): per list, per 4x4 block, a
 * quarter-pel vector and a reference index. Vectors are predicted by the median
 * of the left/top/top-right neighbours, with the directional special cases for
 * 16x8 / 8x16 partitions and the P_Skip zero-vector rule. B macroblocks may use
 * either list or both, and direct modes derive their motion from neighbours
 * (spatial) or from the co-located block of the first list-1 reference
 * (temporal).
 */
#include "h264.h"
#include "h264_priv.h"
#include "owos.h"

static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int iabs(int v) { return v < 0 ? -v : v; }

/* ---- motion-vector prediction ---------------------------------------------
 */
typedef struct {
  int avail, mvx, mvy, ref;
} nbmv;

static h264_mot *mot_at(struct h264_decoder *d, int mbx, int mby) { return &d->cur->mot[mby * d->mb_w + mbx]; }

/* Fetch the 4x4 block at MB-relative (nx4,ny4) (may be -1 or 4) in `list`. */
static nbmv get_mv(struct h264_decoder *d, int mbx, int mby, int list, int nx4, int ny4)
{
  nbmv r = {0, 0, 0, -1};
  int ambx = mbx, amby = mby, lx = nx4, ly = ny4, cur_addr = mby * d->mb_w + mbx, idx;
  if (nx4 < 0) {
    ambx--;
    lx = nx4 + 4;
  } else if (nx4 > 3) {
    ambx++;
    lx = nx4 - 4;
  }
  if (ny4 < 0) {
    amby--;
    ly = ny4 + 4;
  }
  idx = ly * 4 + lx;
  if (ambx == mbx && amby == mby) {
    h264_mot *m = mot_at(d, mbx, mby);
    if (!((d->mb_done >> idx) & 1))
      return r;
    r.avail = 1;
    r.ref = m->refidx[list][idx];
    r.mvx = m->mv[list][idx][0];
    r.mvy = m->mv[list][idx][1];
    return r;
  }
  if (!h264_mb_avail(d, cur_addr, ambx, amby))
    return r;
  r.avail = 1;
  if (!h264_mb_at(d, ambx, amby)->intra) {
    h264_mot *m = mot_at(d, ambx, amby);
    r.ref = m->refidx[list][idx];
    if (r.ref >= 0) {
      r.mvx = m->mv[list][idx][0];
      r.mvy = m->mv[list][idx][1];
    }
  }
  return r;
}

static int median3(int a, int b, int c)
{
  int mn = a < b ? a : b, mx = a > b ? a : b;
  if (c < mn)
    mn = c;
  if (c > mx)
    mx = c;
  return a + b + c - mn - mx;
}

/* Neighbours A (left), B (top) and C (top-right, else top-left) of a partition. */
static void neighbours(struct h264_decoder *d, int mbx, int mby, int list, int bx4, int by4, int pw, nbmv *A, nbmv *B,
                       nbmv *C)
{
  *A = get_mv(d, mbx, mby, list, bx4 - 1, by4);
  *B = get_mv(d, mbx, mby, list, bx4, by4 - 1);
  *C = get_mv(d, mbx, mby, list, bx4 + pw / 4, by4 - 1);
  if (!C->avail)
    *C = get_mv(d, mbx, mby, list, bx4 - 1, by4 - 1);
}

void h264_pred_mv(struct h264_decoder *d, int mbx, int mby, int list, int bx4, int by4, int pw, int ph, int refidx,
                  int *mvx, int *mvy)
{
  nbmv A, B, C;
  neighbours(d, mbx, mby, list, bx4, by4, pw, &A, &B, &C);

  /* Directional predictors for the two-partition shapes. */
  if (pw == 16 && ph == 8) {
    if (by4 == 0 && B.ref == refidx) {
      *mvx = B.mvx;
      *mvy = B.mvy;
      return;
    }
    if (by4 != 0 && A.ref == refidx) {
      *mvx = A.mvx;
      *mvy = A.mvy;
      return;
    }
  } else if (pw == 8 && ph == 16) {
    if (bx4 == 0 && A.ref == refidx) {
      *mvx = A.mvx;
      *mvy = A.mvy;
      return;
    }
    if (bx4 != 0 && C.ref == refidx) {
      *mvx = C.mvx;
      *mvy = C.mvy;
      return;
    }
  }

  if (!B.avail && !C.avail && A.avail) {
    *mvx = A.mvx;
    *mvy = A.mvy;
    return;
  }

  {
    int m = (A.ref == refidx) + (B.ref == refidx) + (C.ref == refidx);
    if (m == 1) {
      const nbmv *s = A.ref == refidx ? &A : B.ref == refidx ? &B : &C;
      *mvx = s->mvx;
      *mvy = s->mvy;
    } else {
      *mvx = median3(A.mvx, B.mvx, C.mvx);
      *mvy = median3(A.mvy, B.mvy, C.mvy);
    }
  }
}

void h264_skip_mv(struct h264_decoder *d, int mbx, int mby, int *mvx, int *mvy)
{
  nbmv A = get_mv(d, mbx, mby, 0, -1, 0);
  nbmv B = get_mv(d, mbx, mby, 0, 0, -1);
  if (!A.avail || !B.avail || (A.ref == 0 && A.mvx == 0 && A.mvy == 0) || (B.ref == 0 && B.mvx == 0 && B.mvy == 0)) {
    *mvx = 0;
    *mvy = 0;
    return;
  }
  h264_pred_mv(d, mbx, mby, 0, 0, 0, 16, 16, 0, mvx, mvy);
}

void h264_store_motion(struct h264_decoder *d, int mbx, int mby, int list, int bx4, int by4, int bw4, int bh4,
                       int refidx, int mvx, int mvy)
{
  h264_mot *m = mot_at(d, mbx, mby);
  int x, y;
  for (y = 0; y < bh4; y++)
    for (x = 0; x < bw4; x++) {
      int idx = (by4 + y) * 4 + bx4 + x;
      m->refidx[list][idx] = (signed char)refidx;
      m->mv[list][idx][0] = (short)(refidx >= 0 ? mvx : 0);
      m->mv[list][idx][1] = (short)(refidx >= 0 ? mvy : 0);
    }
}

/* ---- direct prediction (8.4.1.2) ------------------------------------------
 */
static int min_positive(int x, int y) { return (x >= 0 && y >= 0) ? (x < y ? x : y) : (x > y ? x : y); }

/* Motion of the co-located block: picks list 0 if used, else list 1. */
static void col_motion(struct h264_decoder *d, int mbx, int mby, int blk, int *ref, int *mvx, int *mvy, int *refpic)
{
  const h264_pic *col = d->reflist[1][0];
  const h264_mot *m;
  int l;
  *ref = -1;
  *mvx = *mvy = 0;
  *refpic = -1;
  if (!col)
    return;
  m = &col->mot[mby * d->mb_w + mbx];
  l = m->refidx[0][blk] >= 0 ? 0 : 1;
  if (m->refidx[l][blk] < 0)
    return;
  *ref = m->refidx[l][blk];
  *mvx = m->mv[l][blk][0];
  *mvy = m->mv[l][blk][1];
  *refpic = m->refpic[l][(blk >> 3) * 2 + ((blk & 3) >> 1)];
}

void h264_direct_motion(struct h264_decoder *d, int mbx, int mby, int quad_mask)
{
  int infer = d->cur_sps->direct_8x8_inference, q, sub;
  const h264_pic *col1 = d->reflist[1][0];
  int ref[2] = {-1, -1}, pmv[2][2] = {{0, 0}, {0, 0}}, l;

  if (d->sh.direct_spatial) {
    nbmv A, B, C;
    for (l = 0; l < 2; l++) {
      neighbours(d, mbx, mby, l, 0, 0, 16, &A, &B, &C);
      ref[l] = min_positive(A.ref, min_positive(B.ref, C.ref));
    }
    if (ref[0] < 0 && ref[1] < 0) {
      ref[0] = ref[1] = 0;
    } else {
      for (l = 0; l < 2; l++)
        if (ref[l] >= 0)
          h264_pred_mv(d, mbx, mby, l, 0, 0, 16, 16, ref[l], &pmv[l][0], &pmv[l][1]);
    }
  }

  for (q = 0; q < 4; q++) {
    int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    if (!((quad_mask >> q) & 1))
      continue;
    for (sub = 0; sub < 4; sub++) {
      int bx = qx + (sub & 1), by = qy + (sub >> 1), blk = by * 4 + bx;
      int cblk = infer ? (qy ? 12 : 0) + (qx ? 3 : 0) : blk;
      int cref, cmx, cmy, crp, mv[2][2] = {{0, 0}, {0, 0}}, r[2];
      col_motion(d, mbx, mby, cblk, &cref, &cmx, &cmy, &crp);
      if (d->sh.direct_spatial) {
        int colzero = col1 && col1->ref == 1 && cref == 0 && iabs(cmx) <= 1 && iabs(cmy) <= 1;
        for (l = 0; l < 2; l++) {
          r[l] = ref[l];
          if (r[l] >= 0 && !(r[l] == 0 && colzero)) {
            mv[l][0] = pmv[l][0];
            mv[l][1] = pmv[l][1];
          }
        }
      } else {
        int r0 = 0, tb, td, i;
        const h264_pic *p0, *p1 = d->reflist[1][0];
        if (cref >= 0) {
          for (i = 0; i < d->nref[0]; i++)
            if (d->reflist[0][i]->pic_id == crp) {
              r0 = i;
              break;
            }
        } else {
          cmx = cmy = 0;
        }
        p0 = d->reflist[0][r0];
        tb = clip3(-128, 127, d->cur_poc - p0->poc);
        td = clip3(-128, 127, p1->poc - p0->poc);
        r[0] = r0;
        r[1] = 0;
        if (p0->ref == 2 || td == 0) {
          mv[0][0] = cmx;
          mv[0][1] = cmy;
        } else {
          int tx = (16384 + iabs(td / 2)) / td, dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
          mv[0][0] = (dsf * cmx + 128) >> 8;
          mv[0][1] = (dsf * cmy + 128) >> 8;
          mv[1][0] = mv[0][0] - cmx;
          mv[1][1] = mv[0][1] - cmy;
        }
      }
      for (l = 0; l < 2; l++)
        h264_store_motion(d, mbx, mby, l, bx, by, 1, 1, r[l], mv[l][0], mv[l][1]);
    }
  }
}

/* ---- sample interpolation -------------------------------------------------
 */
#define T6(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))

/* Horizontal half-pel samples (position b) for a w x h block at s. */
static void hpel_h(const unsigned char *s, int ss, unsigned char *o, int w, int h)
{
  int x, y;
  for (y = 0; y < h; y++, s += ss, o += 16)
    for (x = 0; x < w; x++)
      o[x] = (unsigned char)clip8((T6(s[x - 2], s[x - 1], s[x], s[x + 1], s[x + 2], s[x + 3]) + 16) >> 5);
}

/* Vertical half-pel samples (position h). */
static void hpel_v(const unsigned char *s, int ss, unsigned char *o, int w, int h)
{
  int x, y;
  for (y = 0; y < h; y++, s += ss, o += 16)
    for (x = 0; x < w; x++)
      o[x] =
          (unsigned char)clip8((T6(s[x - 2 * ss], s[x - ss], s[x], s[x + ss], s[x + 2 * ss], s[x + 3 * ss]) + 16) >> 5);
}

/* Centre half-pel samples (position j). */
static void hpel_c(const unsigned char *s, int ss, unsigned char *o, int w, int h)
{
  int t[21 * 16], x, y;
  for (y = 0; y < h + 5; y++) {
    const unsigned char *r = s + (y - 2) * ss;
    for (x = 0; x < w; x++)
      t[y * 16 + x] = T6(r[x - 2], r[x - 1], r[x], r[x + 1], r[x + 2], r[x + 3]);
  }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      const int *c = t + (y + 2) * 16 + x;
      o[y * 16 + x] = (unsigned char)clip8((T6(c[-32], c[-16], c[0], c[16], c[32], c[48]) + 512) >> 10);
    }
}

static void avg_into(unsigned char *a, const unsigned char *b, int w, int h)
{
  int x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      a[y * 16 + x] = (unsigned char)((a[y * 16 + x] + b[y * 16 + x] + 1) >> 1);
}

static void copy_block(const unsigned char *s, int ss, unsigned char *o, int w, int h)
{
  int y;
  for (y = 0; y < h; y++)
    memcpy(o + y * 16, s + y * ss, (unsigned)w);
}

/* Fetch the window [x0,x0+ww) x [y0,y0+hh) of a plane, clamping coordinates when
 * it reaches past the extended border. Returns a pointer to (x0,y0). */
static const unsigned char *window(const unsigned char *plane, int stride, int pw, int ph, int x0, int y0, int ww,
                                   int hh, unsigned char *emu, int *ostride)
{
  int x, y;
  if (x0 >= -H264_BORDER && y0 >= -H264_BORDER && x0 + ww <= pw + H264_BORDER && y0 + hh <= ph + H264_BORDER) {
    *ostride = stride;
    return plane + y0 * stride + x0;
  }
  for (y = 0; y < hh; y++) {
    int sy = clip3(0, ph - 1, y0 + y);
    for (x = 0; x < ww; x++)
      emu[y * ww + x] = plane[sy * stride + clip3(0, pw - 1, x0 + x)];
  }
  *ostride = ww;
  return emu;
}

/* Quarter-pel luma prediction of a w x h block into `o` (stride 16). */
static void mc_luma(struct h264_decoder *d, const h264_pic *ref, int x, int y, int w, int h, int mvx, int mvy,
                    unsigned char *o)
{
  unsigned char emu[21 * 21], a[16 * 16], b[16 * 16];
  int fx = mvx & 3, fy = mvy & 3, ix = x + (mvx >> 2), iy = y + (mvy >> 2), ss;
  const unsigned char *s = window(ref->y, d->ystride, d->yw, d->yh, ix - 2, iy - 2, w + 5, h + 5, emu, &ss);
  s += 2 * ss + 2;

  switch (fy * 4 + fx) {
  case 0:
    copy_block(s, ss, o, w, h);
    return;
  case 1: /* a = avg(G, b) */
    hpel_h(s, ss, o, w, h);
    copy_block(s, ss, a, w, h);
    avg_into(o, a, w, h);
    return;
  case 2:
    hpel_h(s, ss, o, w, h);
    return;
  case 3: /* c = avg(H, b) */
    hpel_h(s, ss, o, w, h);
    copy_block(s + 1, ss, a, w, h);
    avg_into(o, a, w, h);
    return;
  case 4: /* d = avg(G, h) */
    hpel_v(s, ss, o, w, h);
    copy_block(s, ss, a, w, h);
    avg_into(o, a, w, h);
    return;
  case 8:
    hpel_v(s, ss, o, w, h);
    return;
  case 12: /* n = avg(M, h) */
    hpel_v(s, ss, o, w, h);
    copy_block(s + ss, ss, a, w, h);
    avg_into(o, a, w, h);
    return;
  case 5: /* e = avg(b, h) */
    hpel_h(s, ss, o, w, h);
    hpel_v(s, ss, b, w, h);
    avg_into(o, b, w, h);
    return;
  case 7: /* g = avg(b, m) */
    hpel_h(s, ss, o, w, h);
    hpel_v(s + 1, ss, b, w, h);
    avg_into(o, b, w, h);
    return;
  case 13: /* p = avg(h, s) */
    hpel_v(s, ss, o, w, h);
    hpel_h(s + ss, ss, b, w, h);
    avg_into(o, b, w, h);
    return;
  case 15: /* r = avg(m, s) */
    hpel_v(s + 1, ss, o, w, h);
    hpel_h(s + ss, ss, b, w, h);
    avg_into(o, b, w, h);
    return;
  default:
    break;
  }
  hpel_c(s, ss, o, w, h);
  switch (fy * 4 + fx) {
  case 10: /* j */
    return;
  case 6: /* f = avg(b, j) */
    hpel_h(s, ss, b, w, h);
    break;
  case 14: /* q = avg(j, s) */
    hpel_h(s + ss, ss, b, w, h);
    break;
  case 9: /* i = avg(h, j) */
    hpel_v(s, ss, b, w, h);
    break;
  case 11: /* k = avg(j, m) */
    hpel_v(s + 1, ss, b, w, h);
    break;
  default:
    return;
  }
  avg_into(o, b, w, h);
}

/* Bilinear 1/8-pel chroma prediction of a w x h block into `o` (stride 8). */
static void mc_chroma(struct h264_decoder *d, const unsigned char *plane, int x, int y, int w, int h, int mvx, int mvy,
                      unsigned char *o)
{
  unsigned char emu[9 * 9];
  int xf = mvx & 7, yf = mvy & 7, ix = x + (mvx >> 3), iy = y + (mvy >> 3), ss, cx, cy;
  const unsigned char *s = window(plane, d->cstride, d->cw, d->ch, ix, iy, w + 1, h + 1, emu, &ss);
  for (cy = 0; cy < h; cy++, s += ss, o += 8)
    for (cx = 0; cx < w; cx++)
      o[cx] = (unsigned char)(((8 - xf) * (8 - yf) * s[cx] + xf * (8 - yf) * s[cx + 1] + (8 - xf) * yf * s[cx + ss] +
                               xf * yf * s[cx + ss + 1] + 32) >>
                              6);
}

/* ---- weighted prediction --------------------------------------------------
 */
void h264_slice_weights(struct h264_decoder *d)
{
  int r0, r1;
  if (d->sh.slice_type != H264_SLICE_B || d->cur_pps->weighted_bipred_idc != 2)
    return;
  for (r0 = 0; r0 < d->nref[0]; r0++)
    for (r1 = 0; r1 < d->nref[1]; r1++) {
      const h264_pic *p0 = d->reflist[0][r0], *p1 = d->reflist[1][r1];
      int tb = clip3(-128, 127, d->cur_poc - p0->poc), td = clip3(-128, 127, p1->poc - p0->poc), w1 = 32;
      if (td != 0 && p0->ref != 2 && p1->ref != 2) {
        int tx = (16384 + iabs(td / 2)) / td, dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
        if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128)
          w1 = dsf >> 2;
      }
      d->implicit_w1[r0][r1] = (short)w1;
    }
}

/* Combine the list predictions of one component and store them in the picture. */
static void put_pred(struct h264_decoder *d, unsigned char *dst, int dstride, unsigned char *const p[2], int pstride,
                     int w, int h, int r0, int r1, int comp)
{
  const h264_pps *pps = d->cur_pps;
  const h264_slice *sh = &d->sh;
  int b = sh->slice_type == H264_SLICE_B, x, y;
  int explicit_w = b ? pps->weighted_bipred_idc == 1 : pps->weighted_pred;
  int logwd = comp ? sh->chroma_log2_denom : sh->luma_log2_denom;

  if (explicit_w) {
    int w0 = 0, o0 = 0, w1 = 0, o1 = 0;
    if (r0 >= 0) {
      w0 = comp ? sh->cw[0][r0][comp - 1] : sh->lw[0][r0];
      o0 = comp ? sh->co[0][r0][comp - 1] : sh->lo[0][r0];
    }
    if (r1 >= 0) {
      w1 = comp ? sh->cw[1][r1][comp - 1] : sh->lw[1][r1];
      o1 = comp ? sh->co[1][r1][comp - 1] : sh->lo[1][r1];
    }
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
        int v;
        if (r0 >= 0 && r1 >= 0) {
          v = ((p[0][y * pstride + x] * w0 + p[1][y * pstride + x] * w1 + (1 << logwd)) >> (logwd + 1)) +
              ((o0 + o1 + 1) >> 1);
        } else {
          int pv = r0 >= 0 ? p[0][y * pstride + x] : p[1][y * pstride + x], wt = r0 >= 0 ? w0 : w1,
              of = r0 >= 0 ? o0 : o1;
          v = logwd >= 1 ? ((pv * wt + (1 << (logwd - 1))) >> logwd) + of : pv * wt + of;
        }
        dst[y * dstride + x] = (unsigned char)clip8(v);
      }
    return;
  }
  if (r0 >= 0 && r1 >= 0) {
    if (b && pps->weighted_bipred_idc == 2) {
      int w1 = d->implicit_w1[r0][r1], w0 = 64 - w1;
      for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
          dst[y * dstride + x] =
              (unsigned char)clip8((p[0][y * pstride + x] * w0 + p[1][y * pstride + x] * w1 + 32) >> 6);
    } else {
      for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
          dst[y * dstride + x] = (unsigned char)((p[0][y * pstride + x] + p[1][y * pstride + x] + 1) >> 1);
    }
    return;
  }
  for (y = 0; y < h; y++)
    memcpy(dst + y * dstride, p[r0 >= 0 ? 0 : 1] + y * pstride, (unsigned)w);
}

static void predict_block(struct h264_decoder *d, int mbx, int mby, int bx4, int by4, int w4, int h4)
{
  const h264_mot *m = mot_at(d, mbx, mby);
  int blk = by4 * 4 + bx4, l, c;
  int r[2] = {m->refidx[0][blk], m->refidx[1][blk]};
  unsigned char pl[2][16 * 16], pc[2][2][8 * 8];
  unsigned char *pp[2];
  int x = mbx * 16 + bx4 * 4, y = mby * 16 + by4 * 4, w = w4 * 4, h = h4 * 4;

  for (l = 0; l < 2; l++) {
    const h264_pic *ref;
    int mvx = m->mv[l][blk][0], mvy = m->mv[l][blk][1];
    if (r[l] < 0)
      continue;
    ref = d->reflist[l][r[l]];
    mc_luma(d, ref, x, y, w, h, mvx, mvy, pl[l]);
    mc_chroma(d, ref->cb, x / 2, y / 2, w / 2, h / 2, mvx, mvy, pc[l][0]);
    mc_chroma(d, ref->cr, x / 2, y / 2, w / 2, h / 2, mvx, mvy, pc[l][1]);
  }
  pp[0] = pl[0];
  pp[1] = pl[1];
  put_pred(d, d->cur->y + y * d->ystride + x, d->ystride, pp, 16, w, h, r[0], r[1], 0);
  for (c = 0; c < 2; c++) {
    unsigned char *dst = (c ? d->cur->cr : d->cur->cb) + (y / 2) * d->cstride + x / 2;
    pp[0] = pc[0][c];
    pp[1] = pc[1][c];
    put_pred(d, dst, d->cstride, pp, 8, w / 2, h / 2, r[0], r[1], 1 + c);
  }
}

/* Do blocks a and b (4x4 indices) carry identical motion? */
static int same_motion(const h264_mot *m, int a, int b)
{
  int l;
  for (l = 0; l < 2; l++) {
    if (m->refidx[l][a] != m->refidx[l][b])
      return 0;
    if (m->refidx[l][a] >= 0 && (m->mv[l][a][0] != m->mv[l][b][0] || m->mv[l][a][1] != m->mv[l][b][1]))
      return 0;
  }
  return 1;
}

static int uniform(const h264_mot *m, int bx4, int by4, int w4, int h4)
{
  int x, y, first = by4 * 4 + bx4;
  for (y = 0; y < h4; y++)
    for (x = 0; x < w4; x++)
      if (!same_motion(m, first, (by4 + y) * 4 + bx4 + x))
        return 0;
  return 1;
}

/* Motion-compensate the whole macroblock from its stored motion, in the
 * largest blocks of uniform motion. */
void h264_predict_mb(struct h264_decoder *d, int mbx, int mby)
{
  const h264_mot *m = mot_at(d, mbx, mby);
  int q, s;
  if (uniform(m, 0, 0, 4, 4)) {
    predict_block(d, mbx, mby, 0, 0, 4, 4);
    return;
  }
  if (uniform(m, 0, 0, 4, 2) && uniform(m, 0, 2, 4, 2)) {
    predict_block(d, mbx, mby, 0, 0, 4, 2);
    predict_block(d, mbx, mby, 0, 2, 4, 2);
    return;
  }
  if (uniform(m, 0, 0, 2, 4) && uniform(m, 2, 0, 2, 4)) {
    predict_block(d, mbx, mby, 0, 0, 2, 4);
    predict_block(d, mbx, mby, 2, 0, 2, 4);
    return;
  }
  for (q = 0; q < 4; q++) {
    int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    if (uniform(m, qx, qy, 2, 2)) {
      predict_block(d, mbx, mby, qx, qy, 2, 2);
      continue;
    }
    for (s = 0; s < 4; s++)
      predict_block(d, mbx, mby, qx + (s & 1), qy + (s >> 1), 1, 1);
  }
}

void h264_mb_refpics(struct h264_decoder *d, int mbx, int mby)
{
  h264_mot *m = mot_at(d, mbx, mby);
  int l, q;
  for (l = 0; l < 2; l++)
    for (q = 0; q < 4; q++) {
      int r = m->refidx[l][(q >> 1) * 8 + (q & 1) * 2];
      m->refpic[l][q] = r >= 0 ? d->reflist[l][r]->pic_id : -1;
    }
}
