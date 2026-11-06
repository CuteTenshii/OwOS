/* encode.c — MPEG-1 video (ISO/IEC 11172-2) encoder. See mpeg.h.
 *
 * The inverse of decode.c: RGB/YCbCr frames -> a conformant elementary stream.
 * Each frame is coded intra (I) or forward-predicted (P, integer-pel
 * full-search motion estimation); blocks go through a forward DCT, quantiser,
 * and the run/ level VLCs. Crucially the encoder *reconstructs* every frame
 * with the exact dequant + IDCT the decoder uses and predicts the next P frame
 * from that reconstruction, so encoder and decoder never drift out of sync.
 *
 * The output round-trips: ffmpeg's mpeg1video decoder — and this library's own
 * decoder — read it back. Rate control is a fixed quantiser (no VBV modelling);
 * B-picture encoding is not implemented (decode-only).
 */
#include "bits.h"
#include "mpeg.h"
#include "mpeg_tables.h"
#include "owos.h"

struct mpeg_encoder {
  int w, h, mb_w, mb_h, yw, yh, cw, ch;
  int fps_code, qscale, gop;
  int frame_count; /* pictures emitted so far                  */
  bitw bw;

  unsigned char intra_q[64], non_intra_q[64];

  /* padded planes: `in` = current input, `rec` = its reconstruction (next
   * frame's reference), `ref` = previous reconstruction. */
  unsigned char *pool;
  unsigned char *in_y, *in_cb, *in_cr;
  unsigned char *ref_y, *ref_cb, *ref_cr;
  unsigned char *rec_y, *rec_cb, *rec_cr;

  int pmv[2];     /* motion-vector predictor (integer pel)    */
  int dc_pred[3]; /* intra DC predictors                      */
};

/* ---- forward + inverse transforms (inverse mirrors decode.c exactly) ------
 */
static void fdct(const int in[64], double out[64])
{
  double g[64];
  int y, x, u, v;
  for (y = 0; y < 8; y++)
    for (u = 0; u < 8; u++) {
      double s = 0.0;
      for (x = 0; x < 8; x++)
        s += in[y * 8 + x] * mpeg_idct_basis[x][u];
      g[y * 8 + u] = s;
    }
  for (v = 0; v < 8; v++)
    for (u = 0; u < 8; u++) {
      double s = 0.0;
      for (y = 0; y < 8; y++)
        s += g[y * 8 + u] * mpeg_idct_basis[y][v];
      out[v * 8 + u] = s;
    }
}

static int dequant(const struct mpeg_encoder *e, int level, int r, int intra)
{
  int val;
  if (level == 0)
    return 0;
  if (intra)
    val = (2 * level * e->qscale * e->intra_q[r]) / 16;
  else {
    int s = (level > 0) ? 1 : -1;
    val = ((2 * level + s) * e->qscale * e->non_intra_q[r]) / 16;
  }
  if ((val & 1) == 0) {
    if (val > 0)
      val--;
    else if (val < 0)
      val++;
  }
  if (val > 2047)
    val = 2047;
  else if (val < -2048)
    val = -2048;
  return val;
}

/* Inverse DCT of `blk` (raster) into an 8x8 region; add to prediction (P) or
 * overwrite with a clamped pixel (intra). Identical arithmetic to decode.c. */
static void idct_out(const int blk[64], int add, unsigned char *out, int stride)
{
  double tmp[64];
  int v, x, y, u;
  for (v = 0; v < 8; v++)
    for (x = 0; x < 8; x++) {
      double s = 0.0;
      for (u = 0; u < 8; u++)
        s += blk[v * 8 + u] * mpeg_idct_basis[x][u];
      tmp[v * 8 + x] = s;
    }
  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x++) {
      double s = 0.0;
      int val;
      for (v = 0; v < 8; v++)
        s += tmp[v * 8 + x] * mpeg_idct_basis[y][v];
      val = (int)(s < 0 ? s - 0.5 : s + 0.5);
      if (add)
        val += out[y * stride + x];
      if (val < 0)
        val = 0;
      else if (val > 255)
        val = 255;
      out[y * stride + x] = (unsigned char)val;
    }
}

/* ---- VLC encoders (reverse of the shared tables) ------------------------- */
static void put_coeff(bitw *w, int run, int alevel, int sign)
{
  int i;
  for (i = 0; i < MPEG_DCT_NELEM; i++)
    if (mpeg_dct_run[i] == run && mpeg_dct_level[i] == alevel) {
      bw_put(w, mpeg_dct_vlc[i][0], mpeg_dct_vlc[i][1]);
      bw_put(w, sign, 1);
      return;
    }
  /* escape: 000001 + 6-bit run + the MPEG-1 (double-)escaped level */
  {
    int level = sign ? -alevel : alevel;
    bw_put(w, mpeg_dct_vlc[MPEG_DCT_ESC][0], mpeg_dct_vlc[MPEG_DCT_ESC][1]);
    bw_put(w, run, 6);
    if (level >= 1 && level <= 127)
      bw_put(w, level, 8);
    else if (level >= -127 && level <= -1)
      bw_put(w, level & 0xff, 8);
    else if (level >= 128 && level <= 255) {
      bw_put(w, 0, 8);
      bw_put(w, level, 8);
    } else {
      bw_put(w, 128, 8);
      bw_put(w, (level + 256) & 0xff, 8);
    }
  }
}
static void put_eob(bitw *w) { bw_put(w, mpeg_dct_vlc[MPEG_DCT_EOB][0], mpeg_dct_vlc[MPEG_DCT_EOB][1]); }

/* Encode one intra block: DC differential (per the luma/chroma DC-size VLC)
 * then the AC run/levels. `levels` is quantised, raster order; comp picks the
 * DC table and predictor. Returns nothing; also fills `rec_levels` for
 * reconstruction. */
static void put_intra_block(struct mpeg_encoder *e, const int levels[64], int comp)
{
  int cc = (comp < 4) ? 0 : (comp - 3);
  int dc = levels[0], diff = dc - e->dc_pred[cc], size = 0, i, run;
  unsigned mag = diff < 0 ? -diff : diff;
  const unsigned short (*dctab)[2] = comp < 4 ? mpeg_dc_lum : mpeg_dc_chroma;
  e->dc_pred[cc] = dc;
  while ((1u << size) <= mag)
    size++; /* bits to hold |diff|      */
  bw_put(&e->bw, dctab[size][0], dctab[size][1]);
  if (size > 0) {
    int v = diff >= 0 ? diff : diff + (1 << size) - 1;
    bw_put(&e->bw, v, size);
  }
  run = 0;
  for (i = 1; i < 64; i++) {
    int r = mpeg_zigzag[i], lv = levels[r];
    if (lv == 0) {
      run++;
      continue;
    }
    put_coeff(&e->bw, run, lv < 0 ? -lv : lv, lv < 0);
    run = 0;
  }
  put_eob(&e->bw);
}

static void put_inter_block(struct mpeg_encoder *e, const int levels[64])
{
  int i, run = 0, first = 1;
  for (i = 0; i < 64; i++) {
    int r = mpeg_zigzag[i], lv = levels[r];
    if (lv == 0) {
      run++;
      continue;
    }
    if (first && run == 0 && (lv == 1 || lv == -1)) { /* '1s' first-coeff code */
      bw_put(&e->bw, 1, 1);
      bw_put(&e->bw, lv < 0, 1);
    } else {
      put_coeff(&e->bw, run, lv < 0 ? -lv : lv, lv < 0);
    }
    run = 0;
    first = 0;
  }
  put_eob(&e->bw);
}

/* macroblock_type: emit the {code,len} for `flags` from a P/B or I table. */
static void put_mbtype(bitw *w, const unsigned char (*tab)[3], int n, int flags)
{
  int i;
  for (i = 0; i < n; i++)
    if (tab[i][2] == (unsigned)flags) {
      bw_put(w, tab[i][0], tab[i][1]);
      return;
    }
}

/* One motion-vector component: the value `d = mv - pred` (integer pel here). */
static void put_mv(bitw *w, int d, int f_code)
{
  int r_size = f_code - 1, a, code, residual, sign;
  if (d == 0) {
    bw_put(w, mpeg_motion[0][0], mpeg_motion[0][1]);
    return;
  }
  sign = d < 0;
  a = (d < 0 ? -d : d) - 1;
  code = (a >> r_size) + 1;
  residual = a & ((1 << r_size) - 1);
  bw_put(w, mpeg_motion[code][0], mpeg_motion[code][1]);
  bw_put(w, sign, 1);
  if (r_size > 0)
    bw_put(w, residual, r_size);
}

/* ---- quantisation ---------------------------------------------------------
 */
static int q_intra_ac(const struct mpeg_encoder *e, double coef, int r)
{
  double q = coef * 8.0 / (e->qscale * e->intra_q[r]);
  int lv = (int)(q < 0 ? q - 0.5 : q + 0.5);
  if (lv > 255)
    lv = 255;
  else if (lv < -255)
    lv = -255;
  return lv;
}
static int q_inter(const struct mpeg_encoder *e, double coef, int r)
{
  double q = coef * 8.0 / (e->qscale * e->non_intra_q[r]);
  int lv = (int)q; /* truncate: a small deadzone */
  if (lv > 255)
    lv = 255;
  else if (lv < -255)
    lv = -255;
  return lv;
}

/* ---- block helpers --------------------------------------------------------
 */
/* Gather an 8x8 block of a plane into `blk` (ints). */
static void gather(const unsigned char *p, int stride, int bx, int by, int blk[64])
{
  int y, x;
  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x++)
      blk[y * 8 + x] = p[(by + y) * stride + bx + x];
}

/* Intra-code one 8x8 block: fdct -> quant -> emit -> reconstruct into `rec`. */
static void encode_intra_block(struct mpeg_encoder *e, const unsigned char *src, int sstride, unsigned char *rec,
                               int rstride, int bx, int by, int comp)
{
  int blk[64], levels[64], deq[64], i;
  double F[64];
  gather(src, sstride, bx, by, blk);
  fdct(blk, F);
  levels[0] = (int)(F[0] / 8.0 + (F[0] < 0 ? -0.5 : 0.5)); /* DC / intra_dc_mult */
  for (i = 1; i < 64; i++)
    levels[i] = q_intra_ac(e, F[i], i);
  put_intra_block(e, levels, comp);
  deq[0] = levels[0] * 8;
  for (i = 1; i < 64; i++)
    deq[i] = dequant(e, levels[i], i, 1);
  idct_out(deq, 0, rec + by * rstride + bx, rstride);
}

/* ---- motion estimation ----------------------------------------------------
 */
/* SAD of the 16x16 luma block at (mbx,mby) against the reference shifted by
 * (dx,dy) integer pixels, with edge clamping. */
static int sad16(const struct mpeg_encoder *e, int mbx, int mby, int dx, int dy)
{
  const unsigned char *cur = e->in_y, *ref = e->ref_y;
  int bx = mbx * 16, by = mby * 16, x, y, sad = 0;
  for (y = 0; y < 16; y++)
    for (x = 0; x < 16; x++) {
      int rx = bx + x + dx, ry = by + y + dy, d;
      if (rx < 0)
        rx = 0;
      else if (rx >= e->yw)
        rx = e->yw - 1;
      if (ry < 0)
        ry = 0;
      else if (ry >= e->yh)
        ry = e->yh - 1;
      d = cur[(by + y) * e->yw + bx + x] - ref[ry * e->yw + rx];
      sad += d < 0 ? -d : d;
    }
  return sad;
}

#define ME_RANGE 15
static void motion_search(const struct mpeg_encoder *e, int mbx, int mby, int *bestx, int *besty, int *bestsad)
{
  /* Constrain the search so the 16x16 block never references pixels outside the
   * reference frame: it keeps predictions unambiguous (no reliance on a
   * decoder's edge-extension policy), so the output is portable to any
   * conformant decoder. */
  int dx, dy, best = sad16(e, mbx, mby, 0, 0);
  int lox = -mbx * 16, hix = e->yw - 16 - mbx * 16;
  int loy = -mby * 16, hiy = e->yh - 16 - mby * 16;
  *bestx = 0;
  *besty = 0;
  for (dy = -ME_RANGE; dy <= ME_RANGE; dy++) {
    if (dy < loy || dy > hiy)
      continue;
    for (dx = -ME_RANGE; dx <= ME_RANGE; dx++) {
      int s;
      if ((dx == 0 && dy == 0) || dx < lox || dx > hix)
        continue;
      s = sad16(e, mbx, mby, dx, dy);
      if (s < best - 8) {
        best = s;
        *bestx = dx;
        *besty = dy;
      } /* bias to (0,0) */
    }
  }
  *bestsad = best;
}

/* Half-pel motion compensation, byte-for-byte identical to decode.c's mc_block,
 * so the encoder's reconstruction matches what a conformant decoder produces.
 * mv is in half-pel units. */
static void mc_pred(unsigned char *dst, int dstride, const unsigned char *src, int sw, int sh, int bx, int by, int mvx,
                    int mvy, int bw, int bh)
{
  int ix = bx + (mvx >> 1), iy = by + (mvy >> 1), fx = mvx & 1, fy = mvy & 1, x, y;
  for (y = 0; y < bh; y++)
    for (x = 0; x < bw; x++) {
      int x0 = ix + x, y0 = iy + y, x1 = x0 + fx, y1 = y0 + fy, p;
      if (x0 < 0)
        x0 = 0;
      else if (x0 >= sw)
        x0 = sw - 1;
      if (y0 < 0)
        y0 = 0;
      else if (y0 >= sh)
        y0 = sh - 1;
      if (x1 < 0)
        x1 = 0;
      else if (x1 >= sw)
        x1 = sw - 1;
      if (y1 < 0)
        y1 = 0;
      else if (y1 >= sh)
        y1 = sh - 1;
      if (!fx && !fy)
        p = src[y0 * sw + x0];
      else if (fx && !fy)
        p = (src[y0 * sw + x0] + src[y0 * sw + x1] + 1) >> 1;
      else if (!fx && fy)
        p = (src[y0 * sw + x0] + src[y1 * sw + x0] + 1) >> 1;
      else
        p = (src[y0 * sw + x0] + src[y0 * sw + x1] + src[y1 * sw + x0] + src[y1 * sw + x1] + 2) >> 2;
      dst[y * dstride + x] = (unsigned char)p;
    }
}

/* Write the (half-pel) prediction for macroblock (mbx,mby) into the
 * reconstruction planes; coded residuals are then added on top. `hmvx`/`hmvy`
 * are the luma motion in half-pel units; chroma uses mv/2 exactly as the
 * decoder does. */
static void build_pred(struct mpeg_encoder *e, int mbx, int mby, int hmvx, int hmvy)
{
  int cmvx = hmvx / 2, cmvy = hmvy / 2;
  mc_pred(e->rec_y + (mby * 16) * e->yw + mbx * 16, e->yw, e->ref_y, e->yw, e->yh, mbx * 16, mby * 16, hmvx, hmvy, 16,
          16);
  mc_pred(e->rec_cb + (mby * 8) * e->cw + mbx * 8, e->cw, e->ref_cb, e->cw, e->ch, mbx * 8, mby * 8, cmvx, cmvy, 8, 8);
  mc_pred(e->rec_cr + (mby * 8) * e->cw + mbx * 8, e->cw, e->ref_cr, e->cw, e->ch, mbx * 8, mby * 8, cmvx, cmvy, 8, 8);
}

/* Quantise one 8x8 inter residual (src - pred, where pred is already in rec)
 * into `levels`. Returns 1 if any coefficient is nonzero. Does not modify rec.
 */
static int quant_inter_block(struct mpeg_encoder *e, const unsigned char *src, int sstride, const unsigned char *rec,
                             int rstride, int bx, int by, int levels[64])
{
  int res[64], i, any = 0, y, x;
  double F[64];
  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x++)
      res[y * 8 + x] = (int)src[(by + y) * sstride + bx + x] - rec[(by + y) * rstride + bx + x];
  fdct(res, F);
  for (i = 0; i < 64; i++) {
    levels[i] = q_inter(e, F[i], i);
    if (levels[i])
      any = 1;
  }
  return any;
}

/* Add a coded inter block's reconstructed residual on top of the prediction. */
static void recon_inter_block(struct mpeg_encoder *e, const int levels[64], unsigned char *rec, int rstride, int bx,
                              int by)
{
  int deq[64], i;
  for (i = 0; i < 64; i++)
    deq[i] = dequant(e, levels[i], i, 0);
  idct_out(deq, 1, rec + by * rstride + bx, rstride);
}

/* ---- headers --------------------------------------------------------------
 */
static void start_code(bitw *w, int id)
{
  bw_align(w);
  bw_put(w, 1, 24);
  bw_put(w, id, 8);
}

static int fps_to_code(int num, int den)
{
  int r = den ? (num + den / 2) / den : 25;
  if (r <= 23)
    return 1;
  if (r == 24)
    return 2;
  if (r == 25)
    return 3;
  if (r <= 29)
    return 4;
  if (r == 30)
    return 5;
  if (r <= 50)
    return 6;
  return 5;
}

static void write_seq_header(struct mpeg_encoder *e)
{
  start_code(&e->bw, 0xB3);
  bw_put(&e->bw, e->w, 12);
  bw_put(&e->bw, e->h, 12);
  bw_put(&e->bw, 1, 4); /* aspect_ratio 1:1 pel                */
  bw_put(&e->bw, e->fps_code, 4);
  bw_put(&e->bw, 0x3FFFF, 18); /* bit_rate: unspecified/variable      */
  bw_put(&e->bw, 1, 1);        /* marker                              */
  bw_put(&e->bw, 112, 10);     /* vbv_buffer_size                     */
  bw_put(&e->bw, 0, 1);        /* constrained_parameters              */
  bw_put(&e->bw, 0, 1);        /* load_intra_quantiser (use default)  */
  bw_put(&e->bw, 0, 1);        /* load_non_intra_quantiser            */
}

/* ---- per-frame encode -----------------------------------------------------
 */
static void encode_frame_internal(struct mpeg_encoder *e, int is_i)
{
  static const int bx4[4] = {0, 8, 0, 8}, by4[4] = {0, 0, 8, 8};
  int f_code = 3; /* half-pel MVs (full_pel off)          */
  int row, col, comp;

  if (is_i)
    write_seq_header(e); /* a fresh seq header at each I frame   */

  start_code(&e->bw, 0x00);                   /* picture start                       */
  bw_put(&e->bw, e->frame_count & 0x3FF, 10); /* temporal_reference          */
  bw_put(&e->bw, is_i ? MPEG_PIC_I : MPEG_PIC_P, 3);
  bw_put(&e->bw, 0xFFFF, 16); /* vbv_delay                           */
  if (!is_i) {
    bw_put(&e->bw, 0, 1);
    bw_put(&e->bw, f_code, 3);
  } /* full_pel off + f_code */
  bw_put(&e->bw, 0, 1); /* extra_bit_picture                   */

  for (row = 0; row < e->mb_h; row++) {
    int prev_col = -1;
    start_code(&e->bw, row + 1); /* slice, vertical position = row+1     */
    bw_put(&e->bw, e->qscale, 5);
    bw_put(&e->bw, 0, 1); /* extra_bit_slice                      */
    e->pmv[0] = e->pmv[1] = 0;
    e->dc_pred[0] = e->dc_pred[1] = e->dc_pred[2] = 128;

    for (col = 0; col < e->mb_w; col++) {
      int mbx = col, mby = row;
      if (is_i) {
        bw_put(&e->bw, mpeg_addr_inc[col - prev_col - 1][0], mpeg_addr_inc[col - prev_col - 1][1]);
        put_mbtype(&e->bw, mpeg_mbtype_i, 2, MBF_INTRA);
        for (comp = 0; comp < 6; comp++) {
          if (comp < 4)
            encode_intra_block(e, e->in_y, e->yw, e->rec_y, e->yw, mbx * 16 + bx4[comp], mby * 16 + by4[comp], comp);
          else if (comp == 4)
            encode_intra_block(e, e->in_cb, e->cw, e->rec_cb, e->cw, mbx * 8, mby * 8, comp);
          else
            encode_intra_block(e, e->in_cr, e->cw, e->rec_cr, e->cw, mbx * 8, mby * 8, comp);
        }
        prev_col = col;
      } else {
        int mvx, mvy, hmvx, hmvy, sad, cbp = 0, first_last, any = 0, mdx, mdy;
        int levels[6][64];

        motion_search(e, mbx, mby, &mvx, &mvy, &sad);
        hmvx = mvx * 2;
        hmvy = mvy * 2; /* integer-pel ME -> even half-pel MV */
        first_last = (col == 0 || col == e->mb_w - 1);

        /* Prediction into rec; residual-quantise each block for cbp. */
        build_pred(e, mbx, mby, hmvx, hmvy);
        for (comp = 0; comp < 4; comp++)
          if (quant_inter_block(e, e->in_y, e->yw, e->rec_y, e->yw, mbx * 16 + bx4[comp], mby * 16 + by4[comp],
                                levels[comp])) {
            cbp |= 1 << (5 - comp);
            any = 1;
          }
        if (quant_inter_block(e, e->in_cb, e->cw, e->rec_cb, e->cw, mbx * 8, mby * 8, levels[4])) {
          cbp |= 1 << 1;
          any = 1;
        }
        if (quant_inter_block(e, e->in_cr, e->cw, e->rec_cr, e->cw, mbx * 8, mby * 8, levels[5])) {
          cbp |= 1 << 0;
          any = 1;
        }

        if (!any && mvx == 0 && mvy == 0 && !first_last)
          continue; /* skippable: the next increment covers it */

        bw_put(&e->bw, mpeg_addr_inc[col - prev_col - 1][0], mpeg_addr_inc[col - prev_col - 1][1]);
        if (col - prev_col > 1)
          e->pmv[0] = e->pmv[1] = 0; /* skips reset the predictor */
        mdx = hmvx - e->pmv[0];
        mdy = hmvy - e->pmv[1];
        put_mbtype(&e->bw, mpeg_mbtype_p, 7, cbp ? (MBF_FORWARD | MBF_PATTERN) : MBF_FORWARD);
        put_mv(&e->bw, mdx, f_code);
        put_mv(&e->bw, mdy, f_code);
        if (cbp)
          bw_put(&e->bw, mpeg_cbp[cbp][0], mpeg_cbp[cbp][1]);
        e->pmv[0] = hmvx;
        e->pmv[1] = hmvy;

        /* Emit + reconstruct the coded blocks (rec already holds pred). */
        for (comp = 0; comp < 6; comp++) {
          if (!((cbp >> (5 - comp)) & 1))
            continue;
          put_inter_block(e, levels[comp]);
          if (comp < 4)
            recon_inter_block(e, levels[comp], e->rec_y, e->yw, mbx * 16 + bx4[comp], mby * 16 + by4[comp]);
          else if (comp == 4)
            recon_inter_block(e, levels[comp], e->rec_cb, e->cw, mbx * 8, mby * 8);
          else
            recon_inter_block(e, levels[comp], e->rec_cr, e->cw, mbx * 8, mby * 8);
        }
        prev_col = col;
      }
    }
  }
}

/* ---- public API -----------------------------------------------------------
 */
static void copy_pad(unsigned char *dst, int dw, int dh, const unsigned char *src, int sw, int sh)
{
  int y, x;
  for (y = 0; y < dh; y++) {
    int sy = y < sh ? y : sh - 1;
    for (x = 0; x < dw; x++) {
      int sx = x < sw ? x : sw - 1;
      dst[y * dw + x] = src[sy * sw + sx];
    }
  }
}

static int emit_frame(struct mpeg_encoder *e)
{
  int is_i = (e->gop <= 1) || (e->frame_count % e->gop == 0);
  unsigned char *t;
  encode_frame_internal(e, is_i);
  if (e->bw.oom)
    return MPEG_ENOMEM;
  /* this frame's reconstruction becomes the next frame's reference */
  t = e->ref_y;
  e->ref_y = e->rec_y;
  e->rec_y = t;
  t = e->ref_cb;
  e->ref_cb = e->rec_cb;
  e->rec_cb = t;
  t = e->ref_cr;
  e->ref_cr = e->rec_cr;
  e->rec_cr = t;
  e->frame_count++;
  return MPEG_OK;
}

int mpeg_encode_frame_yuv(mpeg_encoder *e, const unsigned char *y, const unsigned char *cb, const unsigned char *cr)
{
  int cwi, chi;
  if (!e || !y || !cb || !cr)
    return MPEG_EFORMAT;
  cwi = (e->w + 1) / 2;
  chi = (e->h + 1) / 2;
  copy_pad(e->in_y, e->yw, e->yh, y, e->w, e->h);
  copy_pad(e->in_cb, e->cw, e->ch, cb, cwi, chi);
  copy_pad(e->in_cr, e->cw, e->ch, cr, cwi, chi);
  return emit_frame(e);
}

int mpeg_encode_frame_rgb(mpeg_encoder *e, const unsigned char *rgb)
{
  int x, y, cwi, chi, cx, cy;
  if (!e || !rgb)
    return MPEG_EFORMAT;
  cwi = (e->w + 1) / 2;
  chi = (e->h + 1) / 2;
  /* luma into the padded plane (BT.601), edge-replicated */
  for (y = 0; y < e->yh; y++) {
    int sy = y < e->h ? y : e->h - 1;
    for (x = 0; x < e->yw; x++) {
      int sx = x < e->w ? x : e->w - 1;
      const unsigned char *p = rgb + (sy * e->w + sx) * 3;
      int Y = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
      e->in_y[y * e->yw + x] = (unsigned char)Y;
    }
  }
  /* chroma: average each 2x2 RGB block, then edge-replicate to the padded plane
   */
  for (cy = 0; cy < e->ch; cy++)
    for (cx = 0; cx < e->cw; cx++) {
      int scx = cx < cwi ? cx : cwi - 1, scy = cy < chi ? cy : chi - 1;
      int sr = 0, sg = 0, sb = 0, n = 0, dy, dx, U, V;
      for (dy = 0; dy < 2; dy++)
        for (dx = 0; dx < 2; dx++) {
          int px = scx * 2 + dx, py = scy * 2 + dy;
          if (px >= e->w)
            px = e->w - 1;
          if (py >= e->h)
            py = e->h - 1;
          {
            const unsigned char *p = rgb + (py * e->w + px) * 3;
            sr += p[0];
            sg += p[1];
            sb += p[2];
            n++;
          }
        }
      sr /= n;
      sg /= n;
      sb /= n;
      U = ((-43 * sr - 85 * sg + 128 * sb) >> 8) + 128;
      V = ((128 * sr - 107 * sg - 21 * sb) >> 8) + 128;
      if (U < 0)
        U = 0;
      else if (U > 255)
        U = 255;
      if (V < 0)
        V = 0;
      else if (V > 255)
        V = 255;
      e->in_cb[cy * e->cw + cx] = (unsigned char)U;
      e->in_cr[cy * e->cw + cx] = (unsigned char)V;
    }
  return emit_frame(e);
}

int mpeg_encoder_open(int w, int h, int fps_num, int fps_den, int qscale, int gop, mpeg_encoder **out)
{
  struct mpeg_encoder *e;
  unsigned ysz, csz, one;
  unsigned char *p;
  if (out)
    *out = 0;
  if (!out || w < 16 || h < 16 || w > 4095 || h > 4095)
    return MPEG_EFORMAT;

  e = (struct mpeg_encoder *)calloc(1, sizeof *e);
  if (!e)
    return MPEG_ENOMEM;
  e->w = w;
  e->h = h;
  e->mb_w = (w + 15) / 16;
  e->mb_h = (h + 15) / 16;
  e->yw = e->mb_w * 16;
  e->yh = e->mb_h * 16;
  e->cw = e->mb_w * 8;
  e->ch = e->mb_h * 8;
  e->qscale = (qscale < 1) ? 1 : (qscale > 31) ? 31 : qscale;
  e->gop = gop < 1 ? 1 : gop;
  e->fps_code = fps_to_code(fps_num, fps_den);
  memcpy(e->intra_q, mpeg_default_intra, 64);
  memcpy(e->non_intra_q, mpeg_default_non_intra, 64);

  ysz = (unsigned)e->yw * e->yh;
  csz = (unsigned)e->cw * e->ch;
  one = ysz + 2 * csz;
  p = (unsigned char *)malloc(one * 3);
  if (!p) {
    free(e);
    return MPEG_ENOMEM;
  }
  memset(p, 0, one * 3);
  e->pool = p;
  e->in_y = p;
  e->in_cb = p + ysz;
  e->in_cr = p + ysz + csz;
  p += one;
  e->ref_y = p;
  e->ref_cb = p + ysz;
  e->ref_cr = p + ysz + csz;
  p += one;
  e->rec_y = p;
  e->rec_cb = p + ysz;
  e->rec_cr = p + ysz + csz;

  if (bw_init(&e->bw) != 0) {
    free(e->pool);
    free(e);
    return MPEG_ENOMEM;
  }
  *out = e;
  return MPEG_OK;
}

int mpeg_encoder_finish(mpeg_encoder *e, unsigned char **out, unsigned *outlen)
{
  if (!e || !out || !outlen)
    return MPEG_EFORMAT;
  start_code(&e->bw, 0xB7); /* sequence_end_code                   */
  if (e->bw.oom)
    return MPEG_ENOMEM;
  *out = e->bw.buf;
  *outlen = e->bw.len;
  e->bw.buf = 0; /* ownership transferred to caller     */
  return MPEG_OK;
}

void mpeg_encoder_close(mpeg_encoder *e)
{
  if (!e)
    return;
  free(e->pool);
  free(e->bw.buf);
  free(e);
}
