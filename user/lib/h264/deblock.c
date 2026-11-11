/* deblock.c — the in-loop deblocking filter (H.264 8.7).
 *
 * After a picture is reconstructed, block-edge discontinuities are smoothed to
 * reduce blocking artefacts and, crucially, to make the filtered picture the
 * exact reference future frames predict from. Each macroblock's four vertical
 * then four horizontal luma 4x4 edges (and the chroma 8x8 edges) are filtered;
 * the strength (bS 0..4) of each edge comes from whether either side is intra,
 * carries residual, or has differing motion, and controls how many samples are
 * adjusted. The alpha/ beta/tc0 thresholds (from qP + the slice offsets) are
 * the standard tables.
 */
#include "h264.h"
#include "h264_priv.h"
#include "owos.h"

static const unsigned char alpha_tab[52] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,   4,   5,   6,   7,   8,   9,   10,  12,  13,
    15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255,
};
static const unsigned char beta_tab[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
};
static const unsigned char tc0_tab[52][3] = {
    {0, 0, 0},  {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},  {0, 0, 0},
    {0, 0, 0},  {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},  {0, 0, 1},
    {0, 0, 1},  {0, 0, 1},   {0, 0, 1},   {0, 1, 1},   {0, 1, 1},    {1, 1, 1},    {1, 1, 1},    {1, 1, 1},  {1, 1, 1},
    {1, 1, 2},  {1, 1, 2},   {1, 1, 2},   {1, 1, 2},   {1, 2, 3},    {1, 2, 3},    {2, 2, 3},    {2, 2, 4},  {2, 3, 4},
    {2, 3, 4},  {3, 3, 5},   {3, 4, 6},   {3, 4, 6},   {4, 5, 7},    {4, 5, 8},    {4, 6, 9},    {5, 7, 10}, {6, 8, 11},
    {6, 8, 13}, {7, 10, 14}, {8, 11, 16}, {9, 12, 18}, {10, 13, 20}, {11, 15, 23}, {13, 17, 25},
};

static int iabs(int v) { return v < 0 ? -v : v; }
static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Filter one line of eight samples straddling an edge. `q0` points at the first
 * sample past the edge; `step` is the sample stride (1 across a vertical edge,
 * plane-stride across a horizontal one). */
static void filter_line(unsigned char *q0p, int step, int bS, int alpha, int beta, int tc0, int chroma)
{
  int p0 = q0p[-step], p1 = q0p[-2 * step], p2 = q0p[-3 * step], p3 = q0p[-4 * step];
  int q0 = q0p[0], q1 = q0p[step], q2 = q0p[2 * step], q3 = q0p[3 * step];
  int ap, aq;
  if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta)
    return;
  ap = iabs(p2 - p0);
  aq = iabs(q2 - q0);
  if (bS < 4) {
    int tc = chroma ? tc0 + 1 : tc0 + (ap < beta) + (aq < beta);
    int delta = clip3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
    q0p[-step] = (unsigned char)clip8(p0 + delta);
    q0p[0] = (unsigned char)clip8(q0 - delta);
    if (!chroma && ap < beta)
      q0p[-2 * step] = (unsigned char)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1));
    if (!chroma && aq < beta)
      q0p[step] = (unsigned char)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1));
  } else {
    int strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
    if (!chroma) {
      if (ap < beta && strong) {
        q0p[-step] = (unsigned char)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        q0p[-2 * step] = (unsigned char)((p2 + p1 + p0 + q0 + 2) >> 2);
        q0p[-3 * step] = (unsigned char)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
      } else {
        q0p[-step] = (unsigned char)((2 * p1 + p0 + q1 + 2) >> 2);
      }
      if (aq < beta && strong) {
        q0p[0] = (unsigned char)((q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3);
        q0p[step] = (unsigned char)((q2 + q1 + q0 + p0 + 2) >> 2);
        q0p[2 * step] = (unsigned char)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
      } else {
        q0p[0] = (unsigned char)((2 * q1 + q0 + p1 + 2) >> 2);
      }
    } else {
      q0p[-step] = (unsigned char)((2 * p1 + p0 + q1 + 2) >> 2);
      q0p[0] = (unsigned char)((2 * q1 + q0 + p1 + 2) >> 2);
    }
  }
}

static int mv_differs(const short *a, const short *b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; }

/* Does the transform block containing 4x4 block g of mb carry coefficients? */
static int has_coefs(const h264_mb *mb, int g)
{
  if (mb->t8x8) {
    int q = ((g >> 3) << 3) | (((g & 3) >> 1) << 1); /* top-left 4x4 of the 8x8 holding g */
    return mb->nnz_l[q] || mb->nnz_l[q + 1] || mb->nnz_l[q + 4] || mb->nnz_l[q + 5];
  }
  return mb->nnz_l[g] > 0;
}

static int quad_of(int g) { return (g >> 3) * 2 + ((g & 3) >> 1); }

/* Inter boundary strength (0 or 1) from the references and vectors on each side. */
static int bs_motion(const h264_mot *mp, int pg, const h264_mot *mq, int qg)
{
  int p[2], q[2], np, nq, l;
  for (l = 0; l < 2; l++) {
    p[l] = mp->refidx[l][pg] >= 0 ? mp->refpic[l][quad_of(pg)] : -1;
    q[l] = mq->refidx[l][qg] >= 0 ? mq->refpic[l][quad_of(qg)] : -1;
  }
  np = (p[0] >= 0) + (p[1] >= 0);
  nq = (q[0] >= 0) + (q[1] >= 0);
  if (np != nq)
    return 1;
  if (np == 1) {
    int lp = p[0] >= 0 ? 0 : 1, lq = q[0] >= 0 ? 0 : 1;
    if (p[lp] != q[lq])
      return 1;
    return mv_differs(mp->mv[lp][pg], mq->mv[lq][qg]);
  }
  if (!((p[0] == q[0] && p[1] == q[1]) || (p[0] == q[1] && p[1] == q[0])))
    return 1;
  if (p[0] != p[1]) {
    if (p[0] == q[0])
      return mv_differs(mp->mv[0][pg], mq->mv[0][qg]) || mv_differs(mp->mv[1][pg], mq->mv[1][qg]);
    return mv_differs(mp->mv[0][pg], mq->mv[1][qg]) || mv_differs(mp->mv[1][pg], mq->mv[0][qg]);
  }
  return (mv_differs(mp->mv[0][pg], mq->mv[0][qg]) || mv_differs(mp->mv[1][pg], mq->mv[1][qg])) &&
         (mv_differs(mp->mv[0][pg], mq->mv[1][qg]) || mv_differs(mp->mv[1][pg], mq->mv[0][qg]));
}

/* Boundary strength between luma 4x4 block pg of macroblock mp and qg of mq. */
static int bs_of(struct h264_decoder *d, int paddr, int pg, int qaddr, int qg, int mb_boundary)
{
  const h264_mb *mp = &d->mbs[paddr], *mq = &d->mbs[qaddr];
  if (mp->intra || mq->intra)
    return mb_boundary ? 4 : 3;
  if (has_coefs(mp, pg) || has_coefs(mq, qg))
    return 2;
  return bs_motion(&d->cur->mot[paddr], pg, &d->cur->mot[qaddr], qg);
}

/* Filter all edges of one macroblock (vertical edges first, then horizontal).
 */
static void filter_mb(struct h264_decoder *d, int mbx, int mby)
{
  int cur_addr = mby * d->mb_w + mbx;
  h264_mb *cur = &d->mbs[cur_addr];
  int qpc = cur->qpy;
  int e, seg, row, dir;
  int cb_off = d->cur_pps->chroma_qp_index_offset;
  int cr_off = d->cur_pps->second_chroma_qp_index_offset;

  for (dir = 0; dir < 2; dir++) { /* 0 = vertical edges, 1 = horizontal */
    for (e = 0; e < 4; e++) {
      int is_mb_edge = (e == 0);
      h264_mb *nb = cur;
      int qpP = qpc;
      int nbx = mbx, nby = mby, paddr = cur_addr;
      if (is_mb_edge) {
        if (dir == 0) {
          nbx = mbx - 1;
        } else {
          nby = mby - 1;
        }
        nb = h264_mb_at(d, nbx, nby);
        if (!nb || nb->slice_id < 0 || (cur->dbk_idc == 2 && nb->slice_id != cur->slice_id))
          continue;
        qpP = nb->qpy;
        paddr = nby * d->mb_w + nbx;
      }
      for (seg = 0; seg < 4 && !(cur->t8x8 && (e & 1)); seg++) {
        int pg, qg, bS, qpav, ia, ib, alpha, beta;
        if (dir == 0) {
          qg = seg * 4 + e;
          pg = is_mb_edge ? seg * 4 + 3 : seg * 4 + e - 1;
        } else {
          qg = e * 4 + seg;
          pg = is_mb_edge ? 3 * 4 + seg : (e - 1) * 4 + seg;
        }
        bS = bs_of(d, paddr, pg, cur_addr, qg, is_mb_edge);
        if (bS == 0)
          continue;
        qpav = (qpP + qpc + 1) >> 1;
        ia = clip3(0, 51, qpav + cur->alpha_off);
        ib = clip3(0, 51, qpav + cur->beta_off);
        alpha = alpha_tab[ia];
        beta = beta_tab[ib];
        if (alpha == 0 || beta == 0)
          continue;
        for (row = 0; row < 4; row++) {
          int x, y;
          unsigned char *q0;
          if (dir == 0) {
            x = mbx * 16 + e * 4;
            y = mby * 16 + seg * 4 + row;
            q0 = d->cur->y + y * d->ystride + x;
            filter_line(q0, 1, bS, alpha, beta, (bS < 4 ? tc0_tab[ia][bS - 1] : 0), 0);
          } else {
            x = mbx * 16 + seg * 4 + row;
            y = mby * 16 + e * 4;
            q0 = d->cur->y + y * d->ystride + x;
            filter_line(q0, d->ystride, bS, alpha, beta, (bS < 4 ? tc0_tab[ia][bS - 1] : 0), 0);
          }
        }
      }

      /* Chroma: only the MB-boundary (e==0) and the mid (e==2) luma edges
       * map onto chroma edges (chroma-x/y 0 and 4). */
      if (e != 0 && e != 2)
        continue;
      {
        int ce = e / 2; /* chroma edge index 0 or 1        */
        int comp;
        for (comp = 0; comp < 2; comp++) {
          unsigned char *cpl = comp == 0 ? d->cur->cb : d->cur->cr;
          int off = comp == 0 ? cb_off : cr_off;
          int qpcP = h264_chroma_qp(qpP, off), qpcQ = h264_chroma_qp(qpc, off);
          int qpav = (qpcP + qpcQ + 1) >> 1;
          int ia = clip3(0, 51, qpav + cur->alpha_off);
          int ib = clip3(0, 51, qpav + cur->beta_off);
          int alpha = alpha_tab[ia], beta = beta_tab[ib];
          if (alpha == 0 || beta == 0)
            continue;
          for (seg = 0; seg < 8; seg++) {
            int lseg = seg / 2; /* luma bS segment for this chroma line */
            int pg, qg, bS, x, y;
            unsigned char *q0;
            if (dir == 0) {
              qg = lseg * 4 + e;
              pg = is_mb_edge ? lseg * 4 + 3 : lseg * 4 + e - 1;
            } else {
              qg = e * 4 + lseg;
              pg = is_mb_edge ? 3 * 4 + lseg : (e - 1) * 4 + lseg;
            }
            bS = bs_of(d, paddr, pg, cur_addr, qg, is_mb_edge);
            if (bS == 0)
              continue;
            if (dir == 0) {
              x = mbx * 8 + ce * 4;
              y = mby * 8 + seg;
              q0 = cpl + y * d->cstride + x;
              filter_line(q0, 1, bS, alpha, beta, (bS < 4 ? tc0_tab[ia][bS - 1] : 0), 1);
            } else {
              x = mbx * 8 + seg;
              y = mby * 8 + ce * 4;
              q0 = cpl + y * d->cstride + x;
              filter_line(q0, d->cstride, bS, alpha, beta, (bS < 4 ? tc0_tab[ia][bS - 1] : 0), 1);
            }
          }
        }
      }
    }
  }
}

void h264_deblock_picture(struct h264_decoder *d)
{
  int mbx, mby;
  for (mby = 0; mby < d->mb_h; mby++)
    for (mbx = 0; mbx < d->mb_w; mbx++)
      if (d->mbs[mby * d->mb_w + mbx].dbk_idc != 1)
        filter_mb(d, mbx, mby);
}
