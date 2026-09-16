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
    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},  {0, 0, 0},   {0, 0, 0},   {0, 0, 0},
    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},    {0, 0, 0},  {0, 0, 0},   {0, 0, 0},   {0, 0, 0},
    {0, 0, 0},    {0, 0, 1},    {0, 0, 1},    {0, 0, 1},    {0, 1, 1},  {0, 1, 1},   {1, 1, 1},   {1, 1, 1},
    {1, 1, 1},    {1, 1, 1},    {1, 1, 2},    {1, 1, 2},    {1, 1, 2},  {1, 1, 2},   {1, 2, 3},   {1, 2, 3},
    {2, 2, 3},    {2, 2, 4},    {2, 3, 4},    {2, 3, 4},    {3, 3, 5},  {3, 4, 6},   {3, 4, 6},   {4, 5, 7},
    {4, 5, 8},    {4, 6, 9},    {5, 7, 10},   {6, 8, 11},   {6, 8, 13}, {7, 10, 14}, {8, 11, 16}, {9, 12, 18},
    {10, 13, 20}, {11, 15, 23}, {13, 17, 25}, {13, 17, 25},
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

/* Boundary strength between luma 4x4 block p and q. */
static int bs_of(h264_mb *mp, int pg, h264_mb *mq, int qg, int mb_boundary)
{
  if (mp->intra || mq->intra)
    return mb_boundary ? 4 : 3;
  if (mp->nnz_l[pg] > 0 || mq->nnz_l[qg] > 0)
    return 2;
  if (mp->refidx[pg] != mq->refidx[qg])
    return 1;
  if (iabs(mp->mv[pg][0] - mq->mv[qg][0]) >= 4 || iabs(mp->mv[pg][1] - mq->mv[qg][1]) >= 4)
    return 1;
  return 0;
}

/* Filter all edges of one macroblock (vertical edges first, then horizontal).
 */
static void filter_mb(struct h264_decoder *d, int mbx, int mby)
{
  h264_mb *cur = &d->mbs[mby * d->mb_w + mbx];
  int cur_addr = mby * d->mb_w + mbx;
  int qpc = cur->qpy;
  int e, seg, row, dir;
  int cb_off = d->cur_pps->chroma_qp_index_offset;
  int cr_off = d->cur_pps->second_chroma_qp_index_offset;
  int no_slice_edges = (d->sh.disable_deblock == 2);

  for (dir = 0; dir < 2; dir++) { /* 0 = vertical edges, 1 = horizontal */
    for (e = 0; e < 4; e++) {
      int is_mb_edge = (e == 0);
      h264_mb *nb = cur;
      int qpP = qpc;
      int nbx = mbx, nby = mby;
      if (is_mb_edge) {
        if (dir == 0) {
          nbx = mbx - 1;
        } else {
          nby = mby - 1;
        }
        if (!h264_mb_avail(d, cur_addr, nbx, nby))
          continue;
        if (no_slice_edges && d->mbs[nby * d->mb_w + nbx].slice_id != cur->slice_id)
          continue;
        nb = &d->mbs[nby * d->mb_w + nbx];
        qpP = nb->qpy;
      }
      for (seg = 0; seg < 4; seg++) {
        int pg, qg, bS, qpav, ia, ib, alpha, beta;
        if (dir == 0) {
          qg = seg * 4 + e;
          pg = is_mb_edge ? seg * 4 + 3 : seg * 4 + e - 1;
        } else {
          qg = e * 4 + seg;
          pg = is_mb_edge ? 3 * 4 + seg : (e - 1) * 4 + seg;
        }
        bS = bs_of(nb, pg, cur, qg, is_mb_edge);
        if (bS == 0)
          continue;
        qpav = (qpP + qpc + 1) >> 1;
        ia = clip3(0, 51, qpav + d->sh.slice_alpha_off);
        ib = clip3(0, 51, qpav + d->sh.slice_beta_off);
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
          int ia = clip3(0, 51, qpav + d->sh.slice_alpha_off);
          int ib = clip3(0, 51, qpav + d->sh.slice_beta_off);
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
            bS = bs_of(nb, pg, cur, qg, is_mb_edge);
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
  if (d->sh.disable_deblock == 1)
    return; /* whole picture unfiltered */
  for (mby = 0; mby < d->mb_h; mby++)
    for (mbx = 0; mbx < d->mb_w; mbx++)
      filter_mb(d, mbx, mby);
}
