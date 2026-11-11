/* slice.c — slice header parsing and the macroblock layer (H.264 7.3.3–7.3.5).
 *
 * This drives one slice end to end: it reads the header, opens/closes the
 * picture through the h264.c helpers, then walks the macroblocks — classifying
 * each into an intra (I_4x4/I_8x8/I_16x16/I_PCM) or inter (P/B) category,
 * parsing prediction modes, motion vectors and the coded_block_pattern, and
 * reconstructing it by dispatching to intra/inter prediction, residual decode
 * (CAVLC or CABAC), dequant and the inverse transform. Per-macroblock nnz / mode
 * state is stored for neighbours; motion is stored in the picture.
 */
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

/* Table 9-4: coded_block_pattern mapped Exp-Golomb (ChromaArrayType 1/2). */
static const unsigned char cbp_intra[48] = {
    47, 31, 15, 0,  23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46, 16, 3,  5,  10, 12, 19, 21, 26,
    28, 35, 37, 42, 44, 1,  2,  4,  8, 17, 18, 20, 24, 6,  9,  22, 25, 32, 33, 34, 36, 40, 38, 41,
};
static const unsigned char cbp_inter[48] = {
    0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13, 14, 6,  9,  31, 35, 37, 42, 44,
    33, 34, 36, 40, 39, 43, 45, 46, 17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41,
};

static int blk_mask(int bx4, int by4, int bw4, int bh4)
{
  int x, y, m = 0;
  for (y = 0; y < bh4; y++)
    for (x = 0; x < bw4; x++)
      m |= 1 << ((by4 + y) * 4 + bx4 + x);
  return m;
}

/* ---- neighbour derivation (CAVLC nC / CABAC coded_block_flag) -------------- */
static int nc_combine(int a, int b)
{
  if (a >= 0 && b >= 0)
    return (a + b + 1) >> 1;
  if (a >= 0)
    return a;
  if (b >= 0)
    return b;
  return 0;
}

static void nb_luma(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int blk, int *pA, int *pB)
{
  int bx = h264_blk_x[blk] >> 2, by = h264_blk_y[blk] >> 2, nA = -1, nB = -1;
  if (bx > 0)
    nA = cur->nnz_l[by * 4 + bx - 1];
  else if (h264_mb_avail(d, cur_addr, mbx - 1, mby))
    nA = h264_mb_at(d, mbx - 1, mby)->nnz_l[by * 4 + 3];
  if (by > 0)
    nB = cur->nnz_l[(by - 1) * 4 + bx];
  else if (h264_mb_avail(d, cur_addr, mbx, mby - 1))
    nB = h264_mb_at(d, mbx, mby - 1)->nnz_l[3 * 4 + bx];
  *pA = nA;
  *pB = nB;
}

static void nb_chroma(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int comp, int blk, int *pA,
                      int *pB)
{
  unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
  int bx = blk & 1, by = blk >> 1, nA = -1, nB = -1;
  if (bx > 0)
    nA = cn[by * 2 + bx - 1];
  else if (h264_mb_avail(d, cur_addr, mbx - 1, mby)) {
    h264_mb *m = h264_mb_at(d, mbx - 1, mby);
    nA = (comp == 0 ? m->nnz_cb : m->nnz_cr)[by * 2 + 1];
  }
  if (by > 0)
    nB = cn[(by - 1) * 2 + bx];
  else if (h264_mb_avail(d, cur_addr, mbx, mby - 1)) {
    h264_mb *m = h264_mb_at(d, mbx, mby - 1);
    nB = (comp == 0 ? m->nnz_cb : m->nnz_cr)[1 * 2 + bx];
  }
  *pA = nA;
  *pB = nB;
}

/* CABAC DC coded_block_flag of the left/above MB (-1 if unavailable); `bit` is
 * the cbf_dc bit (0 luma, 1 Cb, 2 Cr). */
static void nb_dc(struct h264_decoder *d, int cur_addr, int mbx, int mby, int bit, int *pA, int *pB)
{
  *pA = h264_mb_avail(d, cur_addr, mbx - 1, mby) ? (h264_mb_at(d, mbx - 1, mby)->cbf_dc >> bit) & 1 : -1;
  *pB = h264_mb_avail(d, cur_addr, mbx, mby - 1) ? (h264_mb_at(d, mbx, mby - 1)->cbf_dc >> bit) & 1 : -1;
}

/* Read one residual block of ctxBlockCat `cat` (0 Intra16x16 DC, 1 Intra16x16 AC, 2 luma 4x4, 3 chroma DC, 4 chroma
 * AC) with CAVLC or CABAC. Returns the coefficient count, or -1 on error. */
static int read_res(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int cat, int comp, int blk,
                    int *coeff)
{
  static const unsigned char maxn[5] = {16, 15, 16, 4, 15};
  int nA = -1, nB = -1, cabac = d->cur_pps->entropy_cabac, tc;
  if (!cabac && cat == 3)
    return h264_cavlc_block(&d->br, -1, 4, coeff);
  if (cabac && (cat == 0 || cat == 3))
    nb_dc(d, cur_addr, mbx, mby, cat == 0 ? 0 : 1 + comp, &nA, &nB);
  else if (cat == 4)
    nb_chroma(d, cur, cur_addr, mbx, mby, comp, blk, &nA, &nB);
  else
    nb_luma(d, cur, cur_addr, mbx, mby, blk, &nA, &nB);
  if (!cabac)
    return h264_cavlc_block(&d->br, nc_combine(nA, nB), maxn[cat], coeff);
  tc = h264_cabac_residual(&d->cabac, cat, (nA < 0 ? cur->intra : nA > 0) + 2 * (nB < 0 ? cur->intra : nB > 0),
                           maxn[cat], coeff);
  if (tc > 0 && (cat == 0 || cat == 3))
    cur->cbf_dc |= (unsigned char)(1 << (cat == 0 ? 0 : 1 + comp));
  return tc;
}

/* ---- residual reconstruction helpers --------------------------------------
 */
/* Decode + reconstruct one luma 4x4 block. `dcval` is the (already scaled) DC
 * to insert for Intra_16x16 (dc_sep=1), else the DC comes from the block
 * itself. */
static int recon_luma_block(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int blk, int coded,
                            int dc_sep, int dcval, int qp)
{
  int coeff[16], raster[16] = {0}, k, tc = 0;
  unsigned char *dst = d->cur->y + (mby * 16 + h264_blk_y[blk]) * d->ystride + mbx * 16 + h264_blk_x[blk];
  int rpos = (h264_blk_y[blk] >> 2) * 4 + (h264_blk_x[blk] >> 2);
  const int *ls = d->ls4[cur->intra ? 0 : 3][qp % 6];
  if (coded) {
    tc = read_res(d, cur, cur_addr, mbx, mby, dc_sep ? 1 : 2, 0, blk, coeff);
    if (tc < 0)
      return -1;
    for (k = 0; k < (dc_sep ? 15 : 16); k++)
      raster[h264_zigzag4[dc_sep ? k + 1 : k]] = coeff[k];
  }
  cur->nnz_l[rpos] = (unsigned char)tc;
  if (dc_sep) {
    raster[0] = dcval;
    h264_dequant_4x4(raster, qp, ls, 1);
  } else {
    h264_dequant_4x4(raster, qp, ls, 0);
  }
  if (coded || dc_sep)
    h264_idct4x4_add(raster, dst, d->ystride);
  return 0;
}

/* Decode + reconstruct one luma 8x8 block (transform_size_8x8_flag). */
static int recon_luma_8x8(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int b8, int coded,
                          int qp)
{
  int c64[64], r64[64] = {0}, k, i4, tc = 0, blk = b8 * 4;
  const unsigned char *zz = h264_zigzag8();
  unsigned char *dst = d->cur->y + (mby * 16 + (b8 >> 1) * 8) * d->ystride + mbx * 16 + (b8 & 1) * 8;
  int rpos0 = (b8 >> 1) * 8 + (b8 & 1) * 2;
  if (coded) {
    if (d->cur_pps->entropy_cabac) {
      tc = h264_cabac_residual(&d->cabac, 5, 0, 64, c64);
      for (k = 0; k < 64; k++)
        r64[zz[k]] = c64[k];
      cur->nnz_l[rpos0] = cur->nnz_l[rpos0 + 1] = cur->nnz_l[rpos0 + 4] = cur->nnz_l[rpos0 + 5] = (unsigned char)tc;
    } else {
      for (i4 = 0; i4 < 4; i4++) {
        int c16[16], n = read_res(d, cur, cur_addr, mbx, mby, 2, 0, blk + i4, c16);
        if (n < 0)
          return -1;
        for (k = 0; k < 16; k++)
          r64[zz[4 * k + i4]] = c16[k];
        cur->nnz_l[(h264_blk_y[blk + i4] >> 2) * 4 + (h264_blk_x[blk + i4] >> 2)] = (unsigned char)n;
        tc += n;
      }
    }
    h264_dequant_8x8(r64, qp, d->ls8[cur->intra ? 0 : 1][qp % 6]);
    h264_idct8x8_add(r64, dst, d->ystride);
  } else {
    cur->nnz_l[rpos0] = cur->nnz_l[rpos0 + 1] = cur->nnz_l[rpos0 + 4] = cur->nnz_l[rpos0 + 5] = 0;
  }
  return 0;
}

static int recon_chroma_block(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int comp, int blk,
                              int coded, int dcval, int qpc)
{
  int coeff[16], raster[16] = {0}, k, tc = 0;
  unsigned char *plane = comp == 0 ? d->cur->cb : d->cur->cr;
  int cx = (blk & 1) * 4, cy = (blk >> 1) * 4;
  unsigned char *dst = plane + (mby * 8 + cy) * d->cstride + mbx * 8 + cx;
  unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
  if (coded) {
    tc = read_res(d, cur, cur_addr, mbx, mby, 4, comp, blk, coeff);
    if (tc < 0)
      return -1;
    for (k = 0; k < 15; k++)
      raster[h264_zigzag4[k + 1]] = coeff[k];
  }
  cn[blk] = (unsigned char)tc;
  raster[0] = dcval;
  h264_dequant_4x4(raster, qpc, d->ls4[(cur->intra ? 0 : 3) + 1 + comp][qpc % 6], 1);
  h264_idct4x4_add(raster, dst, d->cstride);
  return 0;
}

/* ---- one macroblock -------------------------------------------------------
 */
struct mbinfo {
  int cat, intra, i16mode, chroma_mode, cbp, bmode;
};

/* An intra4x4 predicted-mode neighbour (8.3.1.1): its mode (2/DC if the
 * neighbour is not Intra NxN), and availability (0 also when constrained-intra
 * blocks it). */
static void i4_neighbor(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int is_left, int bx,
                        int by, int *avail, int *mode)
{
  h264_mb *m;
  int gi;
  if (is_left) {
    if (bx > 0) {
      *avail = 1;
      *mode = cur->i4mode[by * 4 + bx - 1];
      return;
    }
    if (!h264_mb_avail(d, cur_addr, mbx - 1, mby)) {
      *avail = 0;
      return;
    }
    m = h264_mb_at(d, mbx - 1, mby);
    gi = by * 4 + 3;
  } else {
    if (by > 0) {
      *avail = 1;
      *mode = cur->i4mode[(by - 1) * 4 + bx];
      return;
    }
    if (!h264_mb_avail(d, cur_addr, mbx, mby - 1)) {
      *avail = 0;
      return;
    }
    m = h264_mb_at(d, mbx, mby - 1);
    gi = 3 * 4 + bx;
  }
  if (d->cur_pps->constrained_intra_pred && !m->intra) {
    *avail = 0;
    return;
  }
  *avail = 1;
  *mode = (m->cat == MB_I_4x4 || m->cat == MB_I_8x8) ? m->i4mode[gi] : 2;
}

static int read_chroma_mode(struct h264_decoder *d, int cur_addr, int mbx, int mby)
{
  int a, b;
  if (!d->cur_pps->entropy_cabac)
    return (int)br_ue(&d->br);
  a = h264_mb_avail(d, cur_addr, mbx - 1, mby) && h264_mb_at(d, mbx - 1, mby)->chroma_mode != 0;
  b = h264_mb_avail(d, cur_addr, mbx, mby - 1) && h264_mb_at(d, mbx, mby - 1)->chroma_mode != 0;
  return h264_cabac_chroma_pred_mode(&d->cabac, a + b);
}

/* Parse the intra-prediction section (modes) for an I_NxN / I_16x16 MB. */
static void parse_intra_pred(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, struct mbinfo *mb)
{
  int i, step, n, cabac = d->cur_pps->entropy_cabac;
  if (mb->cat == MB_I_4x4 || mb->cat == MB_I_8x8) {
    n = mb->cat == MB_I_8x8 ? 4 : 16;
    step = mb->cat == MB_I_8x8 ? 2 : 1;
    for (i = 0; i < n; i++) {
      int bx, by, mode, availA, availB, modeA = 2, modeB = 2, pred, x, y;
      if (step == 2) {
        bx = (i & 1) * 2;
        by = (i >> 1) * 2;
      } else {
        bx = h264_blk_x[i] >> 2;
        by = h264_blk_y[i] >> 2;
      }
      i4_neighbor(d, cur, cur_addr, mbx, mby, 1, bx, by, &availA, &modeA);
      i4_neighbor(d, cur, cur_addr, mbx, mby, 0, bx, by, &availB, &modeB);
      pred = (!availA || !availB) ? 2 : (modeA < modeB ? modeA : modeB);
      if (cabac ? h264_cabac_decision(&d->cabac, 68) : br_u1(&d->br))
        mode = pred; /* prev_intra_pred_mode_flag */
      else {
        int rem = cabac ? h264_cabac_rem_intra4x4(&d->cabac) : (int)br_u(&d->br, 3);
        mode = rem < pred ? rem : rem + 1;
      }
      for (y = 0; y < step; y++)
        for (x = 0; x < step; x++)
          cur->i4mode[(by + y) * 4 + bx + x] = (signed char)mode;
    }
  } else { /* I_16x16 */
    for (i = 0; i < 16; i++)
      cur->i4mode[i] = 2;
  }
  mb->chroma_mode = read_chroma_mode(d, cur_addr, mbx, mby);
}

/* Decode + reconstruct the residual of an intra/inter MB. Prediction for
 * I_16x16, chroma and inter is already in the plane; I_4x4/I_8x8 luma is
 * predicted per block. */
static int recon_mb(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, struct mbinfo *mb, int qp)
{
  int i8, i4, blk, comp, dcv_luma[16] = {0};
  int cbp_luma = mb->cbp & 0x0f, cbp_chroma = (mb->cbp >> 4) & 3;
  int qpc[2];
  qpc[0] = h264_chroma_qp(qp, d->cur_pps->chroma_qp_index_offset);
  qpc[1] = h264_chroma_qp(qp, d->cur_pps->second_chroma_qp_index_offset);

  /* Luma DC for Intra_16x16. */
  if (mb->cat == MB_I_16x16) {
    int coeff[16], k;
    int tc = read_res(d, cur, cur_addr, mbx, mby, 0, 0, 0, coeff);
    if (tc < 0)
      return -1;
    for (k = 0; k < 16; k++)
      dcv_luma[h264_zigzag4[k]] = coeff[k];
    h264_hadamard4x4(dcv_luma);
    h264_dequant_luma_dc(dcv_luma, qp, d->ls4[0][qp % 6][0]);
  }

  /* Luma blocks in scan order. */
  for (i8 = 0; i8 < 4; i8++) {
    int gcoded = (cbp_luma >> i8) & 1;
    if (cur->t8x8) {
      if (mb->cat == MB_I_8x8)
        h264_intra8x8(d, mbx, mby, i8, cur->i4mode[(i8 >> 1) * 8 + (i8 & 1) * 2]);
      if (recon_luma_8x8(d, cur, cur_addr, mbx, mby, i8, gcoded, qp) != 0)
        return -1;
      continue;
    }
    for (i4 = 0; i4 < 4; i4++) {
      blk = i8 * 4 + i4;
      if (mb->cat == MB_I_4x4)
        h264_intra4x4(d, mbx, mby, blk, cur->i4mode[(h264_blk_y[blk] >> 2) * 4 + (h264_blk_x[blk] >> 2)]);
      if (mb->cat == MB_I_16x16) {
        int rpos = (h264_blk_y[blk] >> 2) * 4 + (h264_blk_x[blk] >> 2);
        if (recon_luma_block(d, cur, cur_addr, mbx, mby, blk, gcoded, 1, dcv_luma[rpos], qp) != 0)
          return -1;
      } else {
        if (recon_luma_block(d, cur, cur_addr, mbx, mby, blk, gcoded, 0, 0, qp) != 0)
          return -1;
      }
    }
  }

  /* Chroma DC (both components), then chroma AC. */
  {
    int dcv[2][4] = {{0}}, cc;
    if (cbp_chroma & 3) {
      for (cc = 0; cc < 2; cc++) {
        int coeff[4], tc = read_res(d, cur, cur_addr, mbx, mby, 3, cc, 0, coeff);
        if (tc < 0)
          return -1;
        dcv[cc][0] = coeff[0];
        dcv[cc][1] = coeff[1];
        dcv[cc][2] = coeff[2];
        dcv[cc][3] = coeff[3];
        h264_chroma_dc_transform(dcv[cc]);
        h264_dequant_chroma_dc(dcv[cc], qpc[cc], d->ls4[(cur->intra ? 0 : 3) + 1 + cc][qpc[cc] % 6][0]);
      }
    }
    for (comp = 0; comp < 2; comp++) {
      unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
      for (blk = 0; blk < 4; blk++) {
        int coded = (cbp_chroma == 2);
        if (!coded)
          cn[blk] = 0;
        if (recon_chroma_block(d, cur, cur_addr, mbx, mby, comp, blk, coded, dcv[comp][blk], qpc[comp]) != 0)
          return -1;
      }
    }
  }
  return 0;
}

/* B macroblock types 1..21: prediction mode (bit0 = list 0, bit1 = list 1) of
 * each of the two partitions, and whether the split is 16x8 (0) or 8x16 (1). */
static const unsigned char b_part_modes[9][2] = {{1, 1}, {2, 2}, {1, 2}, {2, 1}, {1, 3},
                                                 {2, 3}, {3, 1}, {3, 2}, {3, 3}};
/* B sub_mb_type 1..12: prediction mode and sub-partition shape (0 8x8, 1 8x4, 2 4x8, 3 4x4). */
static const unsigned char b_sub_mode[13] = {0, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3};
static const unsigned char b_sub_shape[13] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3};

/* Classify the coded mb_type into a category; fill I_16x16 mode/cbp from it. */
static int classify(int slice_type, int mb_type, struct mbinfo *mb, int *is_pcm)
{
  int m;
  *is_pcm = 0;
  mb->i16mode = 0;
  mb->cbp = 0;
  mb->bmode = 0;
  if (slice_type == H264_SLICE_P && mb_type < 5) {
    static const unsigned char cats[5] = {MB_16x16, MB_16x8, MB_8x16, MB_8x8, MB_8x8};
    mb->intra = 0;
    mb->cat = cats[mb_type];
    mb->i16mode = (mb_type == 4); /* reuse as "ref0-forced" flag */
    return 0;
  }
  if (slice_type == H264_SLICE_B && mb_type < 23) {
    mb->intra = 0;
    if (mb_type == 0) {
      mb->cat = MB_DIRECT;
    } else if (mb_type <= 3) {
      mb->cat = MB_16x16;
      mb->bmode = mb_type; /* 1 L0, 2 L1, 3 Bi */
    } else if (mb_type == 22) {
      mb->cat = MB_8x8;
    } else {
      mb->cat = ((mb_type - 4) & 1) ? MB_8x16 : MB_16x8;
      mb->bmode = (b_part_modes[(mb_type - 4) >> 1][0]) | (b_part_modes[(mb_type - 4) >> 1][1] << 2);
    }
    return 0;
  }
  m = slice_type == H264_SLICE_P ? mb_type - 5 : slice_type == H264_SLICE_B ? mb_type - 23 : mb_type;
  mb->intra = 1;
  if (m == 0) {
    mb->cat = MB_I_4x4;
    return 0;
  }
  if (m == 25) {
    mb->cat = MB_I_PCM;
    *is_pcm = 1;
    return 0;
  }
  if (m >= 1 && m <= 24) {
    int e = m - 1;
    mb->cat = MB_I_16x16;
    mb->i16mode = e % 4;
    mb->cbp = (((e / 4) % 3) << 4) | ((e >= 12) ? 0x0f : 0);
    return 0;
  }
  return -1;
}

/* ref_idx under CAVLC is te(v): a single inverted bit when the active count is
 * 2, else ue(v); 0 bits when only one reference is active. CABAC codes it in
 * unary; ref8[] holds the indices already parsed for this MB's 8x8 quadrants
 * (this list), and quadrants predicted in direct mode do not count as > 0. */
static int read_ref_idx(struct h264_decoder *d, int cur_addr, int mbx, int mby, int list, const int *ref8, int bx4,
                        int by4, int num_refs)
{
  if (num_refs <= 1)
    return 0;
  if (d->cur_pps->entropy_cabac) {
    h264_mb *mA = h264_mb_avail(d, cur_addr, mbx - 1, mby) ? h264_mb_at(d, mbx - 1, mby) : 0;
    h264_mb *mB = h264_mb_avail(d, cur_addr, mbx, mby - 1) ? h264_mb_at(d, mbx, mby - 1) : 0;
    int a, b;
    if (bx4 > 0) {
      a = ref8[(by4 >> 1) * 2 + ((bx4 - 1) >> 1)] > 0;
    } else {
      int q = (by4 >> 1) * 2 + 1;
      a = mA && !((mA->direct8 >> q) & 1) && d->cur->mot[cur_addr - 1].refidx[list][by4 * 4 + 3] > 0;
    }
    if (by4 > 0) {
      b = ref8[((by4 - 1) >> 1) * 2 + (bx4 >> 1)] > 0;
    } else {
      int q = 2 + (bx4 >> 1);
      b = mB && !((mB->direct8 >> q) & 1) && d->cur->mot[cur_addr - d->mb_w].refidx[list][12 + bx4] > 0;
    }
    return h264_cabac_ref_idx(&d->cabac, a + 2 * b);
  }
  if (num_refs == 2)
    return 1 - (int)br_u1(&d->br);
  return (int)br_ue(&d->br);
}

/* mvd of one partition: se(v) pairs under CAVLC; CABAC contexts come from the
 * neighbouring partitions' |mvd|, which is recorded here for the partitions after it. */
static void read_mvd(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int list, int bx4, int by4,
                     int bw4, int bh4, int *dx, int *dy)
{
  h264_mb *mA, *mB;
  int c, x, y, v[2];
  if (!d->cur_pps->entropy_cabac) {
    *dx = br_se(&d->br);
    *dy = br_se(&d->br);
    return;
  }
  mA = h264_mb_avail(d, cur_addr, mbx - 1, mby) ? h264_mb_at(d, mbx - 1, mby) : 0;
  mB = h264_mb_avail(d, cur_addr, mbx, mby - 1) ? h264_mb_at(d, mbx, mby - 1) : 0;
  for (c = 0; c < 2; c++) {
    int a = bx4 > 0 ? cur->mvd[list][by4 * 4 + bx4 - 1][c] : mA ? mA->mvd[list][by4 * 4 + 3][c] : 0;
    int b = by4 > 0 ? cur->mvd[list][(by4 - 1) * 4 + bx4][c] : mB ? mB->mvd[list][12 + bx4][c] : 0;
    v[c] = h264_cabac_mvd(&d->cabac, c, a + b);
  }
  for (c = 0; c < 2; c++) {
    int m = v[c] < 0 ? -v[c] : v[c];
    for (y = 0; y < bh4; y++)
      for (x = 0; x < bw4; x++)
        cur->mvd[list][(by4 + y) * 4 + bx4 + x][c] = (unsigned char)(m > 255 ? 255 : m);
  }
  *dx = v[0];
  *dy = v[1];
}

/* A reference unit (the span one ref_idx covers) and its motion sub-partitions. */
struct subpart {
  int bx, by, w, h;
};
struct refunit {
  int bx, by, w, h;
  int mode; /* bit0 list 0, bit1 list 1; 0 = direct */
  int nsub;
  struct subpart sub[4];
};
static const struct {
  int n;
  int off[4][4];
} sub_geom[4] = {
    {1, {{0, 0, 2, 2}}},                                           /* 8x8 */
    {2, {{0, 0, 2, 1}, {0, 1, 2, 1}}},                             /* 8x4 */
    {2, {{0, 0, 1, 2}, {1, 0, 1, 2}}},                             /* 4x8 */
    {4, {{0, 0, 1, 1}, {1, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}}}, /* 4x4 */
};

/* Parse + derive the motion of a non-intra, non-skip MB (not B_Direct_16x16).
 * Returns the noSubMbPartSizeLessThan8x8 flag (>= 0) or -1 on error. */
static int decode_inter_motion(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, struct mbinfo *mb)
{
  struct refunit ru[4];
  int nru = 0, nosub8 = 1, b = d->sh.slice_type == H264_SLICE_B, cabac = d->cur_pps->entropy_cabac;
  int list, i, s, ref[2][4], ref8[2][4], direct_mask = 0;
  int ref0_forced = !b && mb->i16mode;

  memset(ref, 0, sizeof ref);
  memset(ref8, 0, sizeof ref8);
  if (mb->cat == MB_8x8) {
    for (i = 0; i < 4; i++) {
      int st = b ? (cabac ? h264_cabac_sub_mb_type_b(&d->cabac) : (int)br_ue(&d->br))
                 : (cabac ? h264_cabac_sub_mb_type(&d->cabac) : (int)br_ue(&d->br));
      int shape, mode;
      if (st > (b ? 12 : 3))
        return -1;
      if (b) {
        mode = b_sub_mode[st];
        shape = b_sub_shape[st];
      } else {
        mode = 1;
        shape = st;
      }
      ru[i].bx = (i & 1) * 2;
      ru[i].by = (i >> 1) * 2;
      ru[i].w = ru[i].h = 2;
      ru[i].mode = mode;
      if (mode == 0) {
        direct_mask |= 1 << i;
        ru[i].nsub = 0;
        if (!d->cur_sps->direct_8x8_inference)
          nosub8 = 0;
      } else {
        ru[i].nsub = sub_geom[shape].n;
        if (ru[i].nsub > 1)
          nosub8 = 0;
        for (s = 0; s < ru[i].nsub; s++) {
          ru[i].sub[s].bx = ru[i].bx + sub_geom[shape].off[s][0];
          ru[i].sub[s].by = ru[i].by + sub_geom[shape].off[s][1];
          ru[i].sub[s].w = sub_geom[shape].off[s][2];
          ru[i].sub[s].h = sub_geom[shape].off[s][3];
        }
      }
    }
    nru = 4;
  } else {
    static const int geo[3][2][4] = {
        {{0, 0, 4, 4}, {0, 0, 0, 0}}, {{0, 0, 4, 2}, {0, 2, 4, 2}}, {{0, 0, 2, 4}, {2, 0, 2, 4}}};
    int shape = mb->cat == MB_16x16 ? 0 : mb->cat == MB_16x8 ? 1 : 2;
    nru = shape ? 2 : 1;
    for (i = 0; i < nru; i++) {
      ru[i].bx = geo[shape][i][0];
      ru[i].by = geo[shape][i][1];
      ru[i].w = geo[shape][i][2];
      ru[i].h = geo[shape][i][3];
      ru[i].mode = b ? (shape ? (mb->bmode >> (2 * i)) & 3 : mb->bmode) : 1;
      ru[i].nsub = 1;
      ru[i].sub[0].bx = ru[i].bx;
      ru[i].sub[0].by = ru[i].by;
      ru[i].sub[0].w = ru[i].w;
      ru[i].sub[0].h = ru[i].h;
    }
  }

  d->mb_done = 0;
  if (direct_mask)
    h264_direct_motion(d, mbx, mby, direct_mask);
  cur->direct8 = (unsigned char)direct_mask;

  for (list = 0; list < 2; list++)
    for (i = 0; i < nru; i++) {
      int x, y;
      if (!(ru[i].mode & (1 << list)))
        continue;
      ref[list][i] = ref0_forced ? 0
                                 : read_ref_idx(d, cur_addr, mbx, mby, list, ref8[list], ru[i].bx, ru[i].by,
                                                d->sh.num_ref_idx[list]);
      if (ref[list][i] >= d->sh.num_ref_idx[list] || d->br.overrun)
        return -1;
      for (y = ru[i].by >> 1; y < (ru[i].by + ru[i].h) >> 1; y++)
        for (x = ru[i].bx >> 1; x < (ru[i].bx + ru[i].w) >> 1; x++)
          ref8[list][y * 2 + x] = ref[list][i];
    }

  for (list = 0; list < 2; list++) {
    d->mb_done = 0;
    for (i = 0; i < nru; i++) {
      if (ru[i].mode == 0) {
        d->mb_done |= blk_mask(ru[i].bx, ru[i].by, ru[i].w, ru[i].h);
        continue;
      }
      if (!(ru[i].mode & (1 << list))) {
        h264_store_motion(d, mbx, mby, list, ru[i].bx, ru[i].by, ru[i].w, ru[i].h, -1, 0, 0);
        d->mb_done |= blk_mask(ru[i].bx, ru[i].by, ru[i].w, ru[i].h);
        continue;
      }
      for (s = 0; s < ru[i].nsub; s++) {
        const struct subpart *sp = &ru[i].sub[s];
        int mvx, mvy, dx, dy;
        h264_pred_mv(d, mbx, mby, list, sp->bx, sp->by, sp->w * 4, sp->h * 4, ref[list][i], &mvx, &mvy);
        read_mvd(d, cur, cur_addr, mbx, mby, list, sp->bx, sp->by, sp->w, sp->h, &dx, &dy);
        h264_store_motion(d, mbx, mby, list, sp->bx, sp->by, sp->w, sp->h, ref[list][i], mvx + dx, mvy + dy);
        d->mb_done |= blk_mask(sp->bx, sp->by, sp->w, sp->h);
      }
    }
  }
  return d->br.overrun ? -1 : nosub8;
}

static int decode_pcm(struct h264_decoder *d, h264_mb *cur, int mbx, int mby)
{
  int x, y;
  br_align(&d->br);
  for (y = 0; y < 16; y++)
    for (x = 0; x < 16; x++)
      d->cur->y[(mby * 16 + y) * d->ystride + mbx * 16 + x] = (unsigned char)br_u(&d->br, 8);
  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x++)
      d->cur->cb[(mby * 8 + y) * d->cstride + mbx * 8 + x] = (unsigned char)br_u(&d->br, 8);
  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x++)
      d->cur->cr[(mby * 8 + y) * d->cstride + mbx * 8 + x] = (unsigned char)br_u(&d->br, 8);
  {
    int i;
    for (i = 0; i < 16; i++)
      cur->nnz_l[i] = 16;
    for (i = 0; i < 4; i++) {
      cur->nnz_cb[i] = 16;
      cur->nnz_cr[i] = 16;
    }
  }
  cur->qpy = 0;
  if (d->cur_pps->entropy_cabac) {
    cur->cbp = 0x2f;
    cur->cbf_dc = 7;
    d->cabac.dqp_nz = 0;
    h264_cabac_start(&d->cabac);
  }
  return d->br.overrun ? -1 : 0;
}

static void mb_begin(struct h264_decoder *d, h264_mb *cur)
{
  cur->slice_id = d->cur_slice_id;
  cur->dbk_idc = (unsigned char)d->sh.disable_deblock;
  cur->alpha_off = (signed char)d->sh.slice_alpha_off;
  cur->beta_off = (signed char)d->sh.slice_beta_off;
}

static void motion_done(struct h264_decoder *d, int mbx, int mby)
{
  d->mb_done = 0xffff;
  h264_mb_refpics(d, mbx, mby);
}

/* P_Skip / B_Skip: inferred motion, no residual. */
static void decode_skip(struct h264_decoder *d, int cur_addr, int mbx, int mby, int *qp)
{
  h264_mb *cur = &d->mbs[cur_addr];
  mb_begin(d, cur);
  cur->intra = 0;
  cur->cat = MB_SKIP;
  cur->cbp = 0;
  cur->qpy = (signed char)*qp;
  d->mb_done = 0;
  if (d->sh.slice_type == H264_SLICE_B) {
    h264_direct_motion(d, mbx, mby, 0xf);
    cur->direct8 = 0xf;
  } else {
    int mvx, mvy;
    h264_skip_mv(d, mbx, mby, &mvx, &mvy);
    h264_store_motion(d, mbx, mby, 0, 0, 0, 4, 4, 0, mvx, mvy);
  }
  motion_done(d, mbx, mby);
  h264_predict_mb(d, mbx, mby);
}

static int cabac_skip(struct h264_decoder *d, int cur_addr, int mbx, int mby)
{
  int a = h264_mb_avail(d, cur_addr, mbx - 1, mby) && h264_mb_at(d, mbx - 1, mby)->cat != MB_SKIP;
  int b = h264_mb_avail(d, cur_addr, mbx, mby - 1) && h264_mb_at(d, mbx, mby - 1)->cat != MB_SKIP;
  return h264_cabac_decision(&d->cabac, (d->sh.slice_type == H264_SLICE_B ? 24 : 11) + a + b);
}

static int read_mb_type(struct h264_decoder *d, int cur_addr, int mbx, int mby)
{
  int a, b;
  if (!d->cur_pps->entropy_cabac)
    return (int)br_ue(&d->br);
  if (d->sh.slice_type == H264_SLICE_P)
    return h264_cabac_mb_type_p(&d->cabac);
  if (d->sh.slice_type == H264_SLICE_B) {
    h264_mb *mA = h264_mb_avail(d, cur_addr, mbx - 1, mby) ? h264_mb_at(d, mbx - 1, mby) : 0;
    h264_mb *mB = h264_mb_avail(d, cur_addr, mbx, mby - 1) ? h264_mb_at(d, mbx, mby - 1) : 0;
    a = mA && mA->cat != MB_SKIP && mA->cat != MB_DIRECT;
    b = mB && mB->cat != MB_SKIP && mB->cat != MB_DIRECT;
    return h264_cabac_mb_type_b(&d->cabac, a + b);
  }
  a = h264_mb_avail(d, cur_addr, mbx - 1, mby) && h264_mb_at(d, mbx - 1, mby)->cat != MB_I_4x4 &&
      h264_mb_at(d, mbx - 1, mby)->cat != MB_I_8x8;
  b = h264_mb_avail(d, cur_addr, mbx, mby - 1) && h264_mb_at(d, mbx, mby - 1)->cat != MB_I_4x4 &&
      h264_mb_at(d, mbx, mby - 1)->cat != MB_I_8x8;
  return h264_cabac_mb_type_i(&d->cabac, a + b);
}

/* coded_block_pattern of an I_NxN or inter macroblock, or -1 on a bad code. */
static int read_cbp(struct h264_decoder *d, int cur_addr, int mbx, int mby, int intra)
{
  h264_mb *mA, *mB;
  int b8, luma = 0, chroma = 0, a, b;
  if (!d->cur_pps->entropy_cabac) {
    int code = (int)br_ue(&d->br);
    return code >= 48 ? -1 : intra ? cbp_intra[code] : cbp_inter[code];
  }
  mA = h264_mb_avail(d, cur_addr, mbx - 1, mby) ? h264_mb_at(d, mbx - 1, mby) : 0;
  mB = h264_mb_avail(d, cur_addr, mbx, mby - 1) ? h264_mb_at(d, mbx, mby - 1) : 0;
  for (b8 = 0; b8 < 4; b8++) {
    a = (b8 & 1) ? !((luma >> (b8 - 1)) & 1) : mA && !((mA->cbp >> (b8 + 1)) & 1);
    b = (b8 & 2) ? !((luma >> (b8 - 2)) & 1) : mB && !((mB->cbp >> (b8 + 2)) & 1);
    luma |= h264_cabac_decision(&d->cabac, 73 + a + 2 * b) << b8;
  }
  a = mA && (mA->cbp >> 4) != 0;
  b = mB && (mB->cbp >> 4) != 0;
  if (h264_cabac_decision(&d->cabac, 77 + a + 2 * b)) {
    a = mA && (mA->cbp >> 4) == 2;
    b = mB && (mB->cbp >> 4) == 2;
    chroma = 1 + h264_cabac_decision(&d->cabac, 77 + 4 + a + 2 * b);
  }
  return luma | chroma << 4;
}

static int read_t8x8(struct h264_decoder *d, int cur_addr, int mbx, int mby)
{
  int a, b;
  if (!d->cur_pps->entropy_cabac)
    return (int)br_u1(&d->br);
  a = h264_mb_avail(d, cur_addr, mbx - 1, mby) && h264_mb_at(d, mbx - 1, mby)->t8x8;
  b = h264_mb_avail(d, cur_addr, mbx, mby - 1) && h264_mb_at(d, mbx, mby - 1)->t8x8;
  return h264_cabac_decision(&d->cabac, 399 + a + b);
}

static int decode_mb(struct h264_decoder *d, int cur_addr, int mbx, int mby, int *qp)
{
  h264_mb *cur = &d->mbs[cur_addr];
  struct mbinfo mb;
  int mb_type, is_pcm = 0, nosub8 = 1;
  const h264_pps *pps = d->cur_pps;
  memset(&mb, 0, sizeof mb);
  mb_begin(d, cur);
  mb_type = read_mb_type(d, cur_addr, mbx, mby);
  d->mb_done = 0;

  if (classify(d->sh.slice_type, mb_type, &mb, &is_pcm) != 0)
    return -1;
  cur->cat = (unsigned char)mb.cat;
  cur->intra = (unsigned char)mb.intra;
  cur->chroma_mode = 0;

  if (is_pcm)
    return decode_pcm(d, cur, mbx, mby);

  if (mb.intra) {
    if (mb.cat == MB_I_4x4 && pps->transform_8x8 && read_t8x8(d, cur_addr, mbx, mby)) {
      mb.cat = MB_I_8x8;
      cur->cat = MB_I_8x8;
      cur->t8x8 = 1;
    }
    parse_intra_pred(d, cur, cur_addr, mbx, mby, &mb);
    cur->chroma_mode = (unsigned char)mb.chroma_mode;
  } else if (mb.cat == MB_DIRECT) {
    h264_direct_motion(d, mbx, mby, 0xf);
    cur->direct8 = 0xf;
    motion_done(d, mbx, mby);
  } else {
    nosub8 = decode_inter_motion(d, cur, cur_addr, mbx, mby, &mb);
    if (nosub8 < 0)
      return -1;
    motion_done(d, mbx, mby);
  }
  if (!mb.intra)
    h264_predict_mb(d, mbx, mby);

  /* coded_block_pattern for I_NxN and inter MBs (I_16x16 already has it from mb_type). */
  if (mb.cat != MB_I_16x16) {
    mb.cbp = read_cbp(d, cur_addr, mbx, mby, mb.intra);
    if (mb.cbp < 0)
      return -1;
    if (!mb.intra && (mb.cbp & 15) && pps->transform_8x8 && nosub8 &&
        (mb.cat != MB_DIRECT || d->cur_sps->direct_8x8_inference))
      cur->t8x8 = (unsigned char)read_t8x8(d, cur_addr, mbx, mby);
  }
  cur->cbp = (unsigned char)mb.cbp;

  /* Intra_16x16 and chroma prediction (whole-MB) before residual. */
  if (mb.cat == MB_I_16x16)
    h264_intra16x16(d, mbx, mby, mb.i16mode);
  if (mb.intra)
    h264_intra_chroma(d, mbx, mby, mb.chroma_mode);

  /* mb_qp_delta present when there are coded coefficients or Intra_16x16. */
  if (mb.cbp != 0 || mb.cat == MB_I_16x16) {
    int delta = d->cur_pps->entropy_cabac ? h264_cabac_qp_delta(&d->cabac) : br_se(&d->br);
    d->cabac.dqp_nz = delta != 0;
    *qp = (*qp + delta + 52) % 52;
  } else {
    d->cabac.dqp_nz = 0;
  }
  cur->qpy = (signed char)*qp;

  if (recon_mb(d, cur, cur_addr, mbx, mby, &mb, *qp) != 0)
    return -1;
  return d->br.overrun ? -1 : 0;
}

/* ---- slice header + dispatch ----------------------------------------------
 */
static int parse_pred_weights(struct h264_decoder *d, h264_slice *sh, int nlists)
{
  int l, i, c;
  sh->luma_log2_denom = (int)br_ue(&d->br);
  sh->chroma_log2_denom = (int)br_ue(&d->br);
  if (sh->luma_log2_denom > 7 || sh->chroma_log2_denom > 7)
    return -1;
  for (l = 0; l < nlists; l++)
    for (i = 0; i < sh->num_ref_idx[l]; i++) {
      sh->lw[l][i] = (short)(1 << sh->luma_log2_denom);
      sh->lo[l][i] = 0;
      for (c = 0; c < 2; c++) {
        sh->cw[l][i][c] = (short)(1 << sh->chroma_log2_denom);
        sh->co[l][i][c] = 0;
      }
      if (br_u1(&d->br)) {
        sh->lw[l][i] = (short)br_se(&d->br);
        sh->lo[l][i] = (short)br_se(&d->br);
      }
      if (br_u1(&d->br))
        for (c = 0; c < 2; c++) {
          sh->cw[l][i][c] = (short)br_se(&d->br);
          sh->co[l][i][c] = (short)br_se(&d->br);
        }
    }
  return 0;
}

static int parse_slice_header(struct h264_decoder *d, int nal_type, int nal_ref_idc)
{
  h264_slice *sh = &d->sh;
  h264_sps *s;
  h264_pps *p;
  int pps_id, rc, l, nlists;
  memset(sh, 0, sizeof *sh);
  sh->idr = (nal_type == 5);
  sh->nal_ref_idc = nal_ref_idc;

  sh->first_mb = (int)(br_ue(&d->br) & 0x7fffffff);
  sh->slice_type = (int)(br_ue(&d->br) % 5);
  pps_id = (int)br_ue(&d->br);
  if (pps_id < 0 || pps_id > 255 || !d->pps[pps_id].valid)
    return H264_EFORMAT;
  if (sh->slice_type == H264_SLICE_SP || sh->slice_type == H264_SLICE_SI)
    return H264_EUNSUP;
  p = &d->pps[pps_id];
  rc = h264_ensure_geometry(d, p);
  if (rc != H264_OK)
    return rc;
  s = d->cur_sps;
  if (sh->first_mb >= d->mb_w * d->mb_h)
    return H264_EFORMAT;

  sh->frame_num = (int)br_u(&d->br, s->log2_max_frame_num);
  if (sh->idr)
    sh->idr_pic_id = (int)br_ue(&d->br);
  if (s->poc_type == 0) {
    sh->poc_lsb = (int)br_u(&d->br, s->log2_max_poc_lsb);
    if (p->bottom_field_poc_present)
      sh->delta_poc_bottom = br_se(&d->br);
  } else if (s->poc_type == 1 && !s->delta_poc_always_zero) {
    sh->delta_poc[0] = br_se(&d->br);
    if (p->bottom_field_poc_present)
      sh->delta_poc[1] = br_se(&d->br);
  }
  if (p->redundant_pic_cnt_present && br_ue(&d->br) != 0)
    return H264_EAGAIN; /* redundant slice: the primary copy is decoded instead */

  nlists = sh->slice_type == H264_SLICE_B ? 2 : sh->slice_type == H264_SLICE_P ? 1 : 0;
  if (sh->slice_type == H264_SLICE_B)
    sh->direct_spatial = (int)br_u1(&d->br);
  for (l = 0; l < nlists; l++)
    sh->num_ref_idx[l] = p->num_ref_idx_default[l];
  if (nlists && br_u1(&d->br))
    for (l = 0; l < nlists; l++)
      sh->num_ref_idx[l] = (int)br_ue(&d->br) + 1;
  for (l = 0; l < nlists; l++)
    if (sh->num_ref_idx[l] < 1 || sh->num_ref_idx[l] > H264_MAX_REFS)
      return H264_EFORMAT;

  for (l = 0; l < nlists; l++)
    if (br_u1(&d->br)) { /* ref_pic_list_modification_flag_lX */
      for (;;) {
        int idc = (int)br_ue(&d->br);
        if (idc == 3)
          break;
        if (idc > 2 || sh->rm_n[l] >= H264_MAX_REFS || d->br.overrun)
          return H264_EFORMAT;
        sh->rm_idc[l][sh->rm_n[l]] = (unsigned char)idc;
        sh->rm_val[l][sh->rm_n[l]] = (int)(br_ue(&d->br) & 0xfffff);
        sh->rm_n[l]++;
      }
    }
  if ((p->weighted_pred && sh->slice_type == H264_SLICE_P) ||
      (p->weighted_bipred_idc == 1 && sh->slice_type == H264_SLICE_B))
    if (parse_pred_weights(d, sh, nlists) != 0)
      return H264_EFORMAT;

  if (nal_ref_idc) {
    if (sh->idr) {
      sh->no_output_of_prior = (int)br_u1(&d->br);
      sh->long_term_ref_flag = (int)br_u1(&d->br);
    } else if (br_u1(&d->br)) { /* adaptive_ref_pic_marking */
      sh->adaptive_marking = 1;
      for (;;) {
        int op = (int)br_ue(&d->br);
        if (op == 0)
          break;
        if (op > 6 || sh->mmco_n >= 66 || d->br.overrun)
          return H264_EFORMAT;
        sh->mmco_op[sh->mmco_n] = (unsigned char)op;
        sh->mmco_a[sh->mmco_n] = 0;
        sh->mmco_b[sh->mmco_n] = 0;
        if (op == 1 || op == 3)
          sh->mmco_a[sh->mmco_n] = (int)(br_ue(&d->br) & 0xfffff); /* difference_of_pic_nums_minus1 */
        if (op == 2)
          sh->mmco_a[sh->mmco_n] = (int)(br_ue(&d->br) & 0xfffff); /* long_term_pic_num */
        if (op == 3)
          sh->mmco_b[sh->mmco_n] = (int)(br_ue(&d->br) & 0xfffff); /* long_term_frame_idx */
        if (op == 6)
          sh->mmco_a[sh->mmco_n] = (int)(br_ue(&d->br) & 0xfffff); /* long_term_frame_idx */
        if (op == 4)
          sh->mmco_a[sh->mmco_n] = (int)(br_ue(&d->br) & 0xfffff); /* max_long_term_frame_idx_plus1 */
        sh->mmco_n++;
      }
    }
  }

  if (p->entropy_cabac && sh->slice_type != H264_SLICE_I) {
    sh->cabac_init_idc = (int)br_ue(&d->br);
    if (sh->cabac_init_idc > 2)
      return H264_EFORMAT;
  }
  sh->qp = p->pic_init_qp + br_se(&d->br);
  if (p->deblock_control_present) {
    sh->disable_deblock = (int)br_ue(&d->br);
    if (sh->disable_deblock != 1) {
      sh->slice_alpha_off = 2 * br_se(&d->br);
      sh->slice_beta_off = 2 * br_se(&d->br);
    }
  }
  if (d->br.overrun || sh->qp < 0 || sh->qp > 51)
    return H264_EFORMAT;
  return H264_OK;
}

int h264_decode_slice_dispatch(struct h264_decoder *d, const unsigned char *rbsp, unsigned len, int nal_type,
                               int nal_ref_idc, int *emitted)
{
  int rc, total, addr, qp, cabac, b;
  *emitted = 0;
  br_init(&d->br, rbsp, len);
  rc = parse_slice_header(d, nal_type, nal_ref_idc);
  if (rc == H264_EAGAIN)
    return H264_OK;
  if (rc != H264_OK)
    return rc;

  if (d->sh.first_mb == 0) {
    *emitted = h264_close_open_picture(d);
    rc = h264_start_picture(d, &d->sh, nal_ref_idc);
    if (rc != H264_OK)
      return rc;
    d->slice_count = 0;
  } else if (!d->have_pic) {
    return H264_EFORMAT;
  }
  d->cur_slice_id = d->slice_count++;
  h264_scaling_prepare(d);

  if (d->sh.slice_type != H264_SLICE_I) {
    rc = h264_build_ref_lists(d);
    if (rc != H264_OK)
      return rc;
    h264_slice_weights(d);
  }
  b = d->sh.slice_type == H264_SLICE_B;
  if (b && !d->reflist[1][0])
    return H264_EFORMAT;

  total = d->mb_w * d->mb_h;
  addr = d->sh.first_mb;
  qp = d->sh.qp;
  cabac = d->cur_pps->entropy_cabac;

  if (cabac) {
    h264_cabac_init(d);
    while (addr < total) {
      int mbx = addr % d->mb_w, mby = addr / d->mb_w;
      d->mbs[addr].slice_id = d->cur_slice_id;
      if (d->sh.slice_type != H264_SLICE_I && cabac_skip(d, addr, mbx, mby)) {
        d->cabac.dqp_nz = 0;
        decode_skip(d, addr, mbx, mby, &qp);
      } else if (decode_mb(d, addr, mbx, mby, &qp) != 0) {
        return H264_EFORMAT;
      }
      addr++;
      if (h264_cabac_terminate(&d->cabac))
        break;
    }
    return d->br.overrun ? H264_EFORMAT : H264_OK;
  }

  if (d->sh.slice_type == H264_SLICE_I) {
    while (addr < total) {
      if (decode_mb(d, addr, addr % d->mb_w, addr / d->mb_w, &qp) != 0)
        return H264_EFORMAT;
      addr++;
      if (!br_more_rbsp_data(&d->br))
        break;
    }
  } else { /* P / B */
    int more = 1;
    while (more && addr < total) {
      int skip = (int)br_ue(&d->br);
      while (skip-- > 0 && addr < total) {
        d->mbs[addr].slice_id = d->cur_slice_id;
        decode_skip(d, addr, addr % d->mb_w, addr / d->mb_w, &qp);
        addr++;
      }
      if (!br_more_rbsp_data(&d->br) || addr >= total)
        break;
      if (decode_mb(d, addr, addr % d->mb_w, addr / d->mb_w, &qp) != 0)
        return H264_EFORMAT;
      addr++;
      more = br_more_rbsp_data(&d->br);
    }
  }
  return H264_OK;
}
