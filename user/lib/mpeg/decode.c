/* decode.c — MPEG-1 video (ISO/IEC 11172-2) decoder. See mpeg.h.
 *
 * The stream is a sequence of start-code-delimited layers: sequence header (dims
 * + quant matrices) -> group-of-pictures -> picture (I/P/B) -> slices -> macro-
 * blocks -> blocks. A macroblock is 16x16 luma + two 8x8 chroma (4:2:0); each
 * block is run/level VLC-coded DCT coefficients that dequant + IDCT turn into
 * pixels (intra) or a residual added to a motion-compensated prediction (P/B).
 *
 * Reconstruction runs into a padded frame store; the previous anchor (I or P)
 * is kept as the forward reference and the next anchor as the backward reference
 * so P and B pictures can be predicted. All parsing is bounds-checked — the
 * bitstream is untrusted and a fault must stay in this process.
 */
#include "owos.h"
#include "mpeg.h"
#include "bits.h"
#include "mpeg_tables.h"

#define SEQ_START   0xB3
#define GOP_START   0xB8
#define PIC_START   0x00
#define EXT_START   0xB5
#define USER_START  0xB2
#define SEQ_END     0xB7

typedef struct { unsigned char *y, *cb, *cr; } frame;

struct mpeg_decoder {
	const unsigned char *buf; unsigned len;
	bitr br;

	int width, height;
	int mb_w, mb_h;                 /* macroblocks across / down                */
	int yw, yh, cw, ch;             /* padded plane dims (mb_*·16 / ·8)          */

	unsigned char intra_q[64];      /* raster-order quantiser matrices          */
	unsigned char non_intra_q[64];

	frame cur, ref_fwd, ref_bwd;    /* frame store: current + two anchors       */
	frame last;                     /* the frame the accessors expose           */
	unsigned char *pool;            /* single allocation backing all three      */
	int have_fwd, have_bwd;

	int pic_type;
	int full_pel_fwd, f_code_fwd;
	int full_pel_bwd, f_code_bwd;

	/* per-slice / per-macroblock running state */
	int quant_scale;
	int dc_pred[3];                 /* intra DC predictors (Y, Cb, Cr), QF scale */
	int pmv_fwd[2], pmv_bwd[2];     /* motion-vector predictors [x,y], half-pel  */
	int b_use_fwd, b_use_bwd;       /* last MB's B prediction directions (skip)  */
	int have_pending;               /* ref_bwd holds a decoded, undisplayed anchor */
	int bwd_type;                   /* coding type of the pending future anchor  */
	int last_type;                  /* coding type of the frame the accessors show */

	short block[64];                /* one decoded block, raster order          */
	int   eos;                      /* hit sequence-end / ran out               */
};

/* ---- generic VLC match: find the {code,len} entry, reading bits as needed --- */
static int vlc_lookup(bitr *b, const unsigned short (*tab)[2], int n)
{
	unsigned code = 0; int len = 0, i;
	while (len < 17) {
		code = (code << 1) | br_get1(b); len++;
		for (i = 0; i < n; i++)
			if (tab[i][1] == (unsigned)len && tab[i][0] == code) return i;
	}
	return -1;
}

/* macroblock_type: match a {code,len,flags} table, return the flags (or -1). */
static int mbtype_lookup(bitr *b, const unsigned char (*tab)[3], int n)
{
	unsigned code = 0; int len = 0, i;
	while (len < 9) {
		code = (code << 1) | br_get1(b); len++;
		for (i = 0; i < n; i++)
			if (tab[i][1] == (unsigned)len && tab[i][0] == code) return tab[i][2];
	}
	return -1;
}

/* ---- DCT coefficient decode ---------------------------------------------- */
/* MPEG-1 escape level: 8-bit, with a second byte for the two forbidden values. */
static int esc_level(bitr *b)
{
	int v = (int)br_get(b, 8);
	if (v == 0)   return (int)br_get(b, 8);          /* 128..255 */
	if (v == 128) return (int)br_get(b, 8) - 256;    /* -256..-129 */
	if (v > 128)  return v - 256;                    /* -127..-1 */
	return v;                                         /* 1..127 */
}

/* Read one run/level pair via the coefficient VLC (indices into mpeg_dct_vlc).
 * Returns the entry index; MPEG_DCT_EOB / MPEG_DCT_ESC are handled by the caller. */
static int dct_symbol(bitr *b)
{ return vlc_lookup(b, mpeg_dct_vlc, MPEG_DCT_NELEM + 2); }

/* Dequantise one signed coefficient (level) at raster position `r`. */
static int dequant(const struct mpeg_decoder *d, int level, int r, int intra)
{
	int val;
	if (level == 0) return 0;
	if (intra)
		val = (2 * level * d->quant_scale * d->intra_q[r]) / 16;
	else {
		int s = (level > 0) ? 1 : -1;
		val = ((2 * level + s) * d->quant_scale * d->non_intra_q[r]) / 16;
	}
	if ((val & 1) == 0) { if (val > 0) val--; else if (val < 0) val++; }   /* oddify */
	if (val > 2047) val = 2047; else if (val < -2048) val = -2048;
	return val;
}

/* Decode an intra block into d->block (raster). comp: 0-3 luma, 4 Cb, 5 Cr. */
static int decode_intra_block(struct mpeg_decoder *d, int comp)
{
	int cc = (comp < 4) ? 0 : (comp - 3);            /* DC predictor index       */
	int size, diff = 0, i;
	memset(d->block, 0, sizeof d->block);

	size = vlc_lookup(&d->br, comp < 4 ? mpeg_dc_lum : mpeg_dc_chroma, 12);
	if (size < 0) return -1;
	if (size > 0) {
		unsigned v = br_get(&d->br, size);
		diff = (v < (1u << (size - 1))) ? (int)v - (1 << size) + 1 : (int)v;
	}
	d->dc_pred[cc] += diff;
	d->block[0] = (short)(d->dc_pred[cc] * 8);       /* intra_dc_mult = 8        */

	for (i = 1;;) {
		int idx = dct_symbol(&d->br), run, level, r;
		if (idx < 0) return -1;
		if (idx == MPEG_DCT_EOB) break;
		if (idx == MPEG_DCT_ESC) { run = (int)br_get(&d->br, 6); level = esc_level(&d->br); }
		else { run = mpeg_dct_run[idx]; level = mpeg_dct_level[idx]; if (br_get1(&d->br)) level = -level; }
		i += run;
		if (i > 63) return -1;
		r = mpeg_zigzag[i];
		d->block[r] = (short)dequant(d, level, r, 1);
		i++;
	}
	return 0;
}

/* Decode a non-intra block into d->block (raster). The first coefficient uses the
 * '1s' special case (a leading 1 is run0/level±1, never EOB). */
static int decode_inter_block(struct mpeg_decoder *d)
{
	int i = 0, first = 1;
	memset(d->block, 0, sizeof d->block);
	for (;;) {
		int run, level, r;
		if (first && br_show(&d->br, 1) == 1) {
			br_skip(&d->br, 1);
			run = 0; level = br_get1(&d->br) ? -1 : 1;
		} else {
			int idx = dct_symbol(&d->br);
			if (idx < 0) return -1;
			if (idx == MPEG_DCT_EOB) break;
			if (idx == MPEG_DCT_ESC) { run = (int)br_get(&d->br, 6); level = esc_level(&d->br); }
			else { run = mpeg_dct_run[idx]; level = mpeg_dct_level[idx]; if (br_get1(&d->br)) level = -level; }
		}
		first = 0;
		i += run;
		if (i > 63) return -1;
		r = mpeg_zigzag[i];
		d->block[r] = (short)dequant(d, level, r, 0);
		i++;
	}
	return 0;
}

/* ---- IDCT ----------------------------------------------------------------- */
/* Separable inverse DCT of d->block into `out` (8x8, row stride `stride`),
 * adding to any existing prediction there when `add` is set (P/B residual) or
 * overwriting with a clamped pixel value (intra). */
static void idct_add(const short blk[64], int add, unsigned char *out, int stride)
{
	double tmp[64];
	int v, x, y, u;
	for (v = 0; v < 8; v++)
		for (x = 0; x < 8; x++) {
			double s = 0.0;
			for (u = 0; u < 8; u++) s += blk[v * 8 + u] * mpeg_idct_basis[x][u];
			tmp[v * 8 + x] = s;
		}
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++) {
			double s = 0.0;
			int val;
			for (v = 0; v < 8; v++) s += tmp[v * 8 + x] * mpeg_idct_basis[y][v];
			val = (int)(s < 0 ? s - 0.5 : s + 0.5);
			if (add) val += out[y * stride + x];
			if (val < 0) val = 0; else if (val > 255) val = 255;
			out[y * stride + x] = (unsigned char)val;
		}
}

/* ---- motion compensation -------------------------------------------------- */
/* Copy a half-pel-interpolated `bw`x`bh` block from `src` (a padded plane) at
 * integer base (bx,by) plus half-pel motion (mvx,mvy) into `dst`. When `avg`,
 * average into dst (for bidirectional prediction) instead of overwriting. */
static void mc_block(unsigned char *dst, int dstride,
                     const unsigned char *src, int sstride, int sw, int sh,
                     int bx, int by, int mvx, int mvy, int bw, int bh, int avg)
{
	int ix = bx + (mvx >> 1), iy = by + (mvy >> 1);
	int fx = mvx & 1, fy = mvy & 1, x, y;
	for (y = 0; y < bh; y++) {
		for (x = 0; x < bw; x++) {
			int x0 = ix + x, y0 = iy + y, x1 = x0 + fx, y1 = y0 + fy, p;
			if (x0 < 0) x0 = 0; else if (x0 >= sw) x0 = sw - 1;
			if (y0 < 0) y0 = 0; else if (y0 >= sh) y0 = sh - 1;
			if (x1 < 0) x1 = 0; else if (x1 >= sw) x1 = sw - 1;
			if (y1 < 0) y1 = 0; else if (y1 >= sh) y1 = sh - 1;
			if (!fx && !fy) p = src[y0 * sstride + x0];
			else if (fx && !fy) p = (src[y0 * sstride + x0] + src[y0 * sstride + x1] + 1) >> 1;
			else if (!fx && fy) p = (src[y0 * sstride + x0] + src[y1 * sstride + x0] + 1) >> 1;
			else p = (src[y0 * sstride + x0] + src[y0 * sstride + x1] +
			          src[y1 * sstride + x0] + src[y1 * sstride + x1] + 2) >> 2;
			if (avg) p = (dst[y * dstride + x] + p + 1) >> 1;
			dst[y * dstride + x] = (unsigned char)p;
		}
	}
}

/* Predict a whole macroblock at (mbx,mby) from reference `r` with luma motion
 * (mvx,mvy) in half-pel units; chroma uses mv/2. `avg` averages into cur. */
static void predict_mb(struct mpeg_decoder *d, const frame *r, int mbx, int mby,
                       int mvx, int mvy, int avg)
{
	int cmvx = mvx / 2, cmvy = mvy / 2;
	mc_block(d->cur.y + (mby * 16) * d->yw + mbx * 16, d->yw, r->y, d->yw, d->yw, d->yh,
	         mbx * 16, mby * 16, mvx, mvy, 16, 16, avg);
	mc_block(d->cur.cb + (mby * 8) * d->cw + mbx * 8, d->cw, r->cb, d->cw, d->cw, d->ch,
	         mbx * 8, mby * 8, cmvx, cmvy, 8, 8, avg);
	mc_block(d->cur.cr + (mby * 8) * d->cw + mbx * 8, d->cw, r->cr, d->cw, d->cw, d->ch,
	         mbx * 8, mby * 8, cmvx, cmvy, 8, 8, avg);
}

/* ---- one motion vector component (MPEG-1) --------------------------------- */
static int decode_mv_comp(struct mpeg_decoder *d, int f_code, int *pmv)
{
	int idx = vlc_lookup(&d->br, mpeg_motion, 17);   /* |motion_code|            */
	int code, r_size = f_code - 1, f = 1 << r_size, delta, val;
	if (idx < 0) return -1;
	code = idx;
	if (code != 0 && br_get1(&d->br)) code = -code;
	if (code == 0) delta = 0;
	else {
		int residual = r_size > 0 ? (int)br_get(&d->br, r_size) : 0;
		delta = ((code > 0 ? code - 1 : -code - 1) << r_size) + residual + 1;
		if (code < 0) delta = -delta;
	}
	val = *pmv + delta;
	if (val < -16 * f) val += 32 * f; else if (val >= 16 * f) val -= 32 * f;
	*pmv = val;
	return val;
}

/* ---- reconstruct one coded/intra macroblock's blocks ---------------------- */
static void put_blocks(struct mpeg_decoder *d, int mbx, int mby, int cbp, int intra)
{
	static const int bx[4] = {0, 8, 0, 8}, by[4] = {0, 0, 8, 8};
	int comp;
	for (comp = 0; comp < 6; comp++) {
		unsigned char *dst; int stride;
		int coded = (cbp >> (5 - comp)) & 1;
		if (comp < 4) { dst = d->cur.y + (mby * 16 + by[comp]) * d->yw + mbx * 16 + bx[comp]; stride = d->yw; }
		else if (comp == 4) { dst = d->cur.cb + (mby * 8) * d->cw + mbx * 8; stride = d->cw; }
		else { dst = d->cur.cr + (mby * 8) * d->cw + mbx * 8; stride = d->cw; }

		if (intra) {
			if (decode_intra_block(d, comp) != 0) return;
			idct_add(d->block, 0, dst, stride);
		} else if (coded) {
			if (decode_inter_block(d) != 0) return;
			idct_add(d->block, 1, dst, stride);
		}
	}
}

/* ---- slice ---------------------------------------------------------------- */
static int decode_slice(struct mpeg_decoder *d, int slice_v)
{
	int prev_addr = (slice_v - 1) * d->mb_w - 1;
	int i;

	d->quant_scale = (int)br_get(&d->br, 5);
	while (br_get1(&d->br)) br_get(&d->br, 8);        /* extra_bit_slice          */

	d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 128;
	d->pmv_fwd[0] = d->pmv_fwd[1] = d->pmv_bwd[0] = d->pmv_bwd[1] = 0;

	for (;;) {
		int inc = 0, addr, mbx, mby, flags, cbp, intra;
		if (br_at_start_code(&d->br)) break;

		for (;;) {                                    /* macroblock_address_increment */
			int a = vlc_lookup(&d->br, mpeg_addr_inc, 36);
			if (a < 0) return -1;
			if (a == 34) continue;                    /* stuffing                 */
			if (a == 33) { inc += 33; continue; }     /* escape                   */
			inc += a + 1; break;
		}
		addr = prev_addr + inc;

		/* Macroblocks strictly skipped between prev+1 and addr-1. A P skip is a
		 * zero-motion copy that resets the MV predictor (so a *skip* clears it,
		 * but consecutive coded MBs keep carrying it); a B skip repeats the
		 * previous macroblock's prediction mode and motion vectors. */
		if (d->pic_type == MPEG_PIC_P && addr > prev_addr + 1)
			d->pmv_fwd[0] = d->pmv_fwd[1] = 0;
		for (i = prev_addr + 1; i < addr; i++) {
			int sx = i % d->mb_w, sy = i / d->mb_w;
			if (sx < 0 || sy < 0 || sy >= d->mb_h) continue;
			if (d->pic_type == MPEG_PIC_P && d->have_fwd)
				predict_mb(d, &d->ref_fwd, sx, sy, 0, 0, 0);
			else if (d->pic_type == MPEG_PIC_B) {
				if (d->have_fwd && d->b_use_fwd) predict_mb(d, &d->ref_fwd, sx, sy, d->pmv_fwd[0], d->pmv_fwd[1], 0);
				if (d->have_bwd && d->b_use_bwd) predict_mb(d, &d->ref_bwd, sx, sy, d->pmv_bwd[0], d->pmv_bwd[1], d->b_use_fwd);
			}
			d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 128;
		}
		prev_addr = addr;
		if (addr < 0 || addr >= d->mb_w * d->mb_h) return -1;
		mbx = addr % d->mb_w; mby = addr / d->mb_w;

		/* macroblock_type */
		if (d->pic_type == MPEG_PIC_I) flags = mbtype_lookup(&d->br, mpeg_mbtype_i, 2);
		else if (d->pic_type == MPEG_PIC_P) flags = mbtype_lookup(&d->br, mpeg_mbtype_p, 7);
		else flags = mbtype_lookup(&d->br, mpeg_mbtype_b, 11);
		if (flags < 0) return -1;
		intra = flags & MBF_INTRA;

		if (flags & MBF_QUANT) d->quant_scale = (int)br_get(&d->br, 5);

		if (intra) {
			d->pmv_fwd[0] = d->pmv_fwd[1] = d->pmv_bwd[0] = d->pmv_bwd[1] = 0;
		} else {
			d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 128;
		}

		/* motion vectors + prediction (P/B) */
		if (!intra) {
			int fmx = 0, fmy = 0, bmx = 0, bmy = 0;
			if (d->pic_type == MPEG_PIC_P) {
				/* Every non-intra P macroblock is forward-predicted; a missing
				 * motion_forward flag means a zero motion vector, not "no MC". */
				if (flags & MBF_FORWARD) {
					fmx = decode_mv_comp(d, d->f_code_fwd, &d->pmv_fwd[0]);
					fmy = decode_mv_comp(d, d->f_code_fwd, &d->pmv_fwd[1]);
					if (d->full_pel_fwd) { fmx <<= 1; fmy <<= 1; }
				} else { d->pmv_fwd[0] = d->pmv_fwd[1] = 0; }
				if (d->have_fwd) predict_mb(d, &d->ref_fwd, mbx, mby, fmx, fmy, 0);
			} else {                                  /* B picture               */
				d->b_use_fwd = (flags & MBF_FORWARD) != 0;
				d->b_use_bwd = (flags & MBF_BACKWARD) != 0;
				if (flags & MBF_FORWARD) {
					fmx = decode_mv_comp(d, d->f_code_fwd, &d->pmv_fwd[0]);
					fmy = decode_mv_comp(d, d->f_code_fwd, &d->pmv_fwd[1]);
					if (d->full_pel_fwd) { fmx <<= 1; fmy <<= 1; }
				}
				if (flags & MBF_BACKWARD) {
					bmx = decode_mv_comp(d, d->f_code_bwd, &d->pmv_bwd[0]);
					bmy = decode_mv_comp(d, d->f_code_bwd, &d->pmv_bwd[1]);
					if (d->full_pel_bwd) { bmx <<= 1; bmy <<= 1; }
				}
				if ((flags & MBF_FORWARD) && d->have_fwd)
					predict_mb(d, &d->ref_fwd, mbx, mby, fmx, fmy, 0);
				if ((flags & MBF_BACKWARD) && d->have_bwd)
					predict_mb(d, &d->ref_bwd, mbx, mby, bmx, bmy, (flags & MBF_FORWARD) ? 1 : 0);
			}
		}

		if (flags & MBF_PATTERN) cbp = vlc_lookup(&d->br, mpeg_cbp, 64), cbp = (cbp < 0) ? 0 : cbp;
		else cbp = intra ? 0x3F : 0;

		put_blocks(d, mbx, mby, cbp, intra);
	}
	return 0;
}

/* ---- picture -------------------------------------------------------------- */
/* Read the picture header (already past the 0x00 start-code id): picture type and
 * the forward/backward motion ranges. Reference setup happens between this and
 * decode_pic_slices so a P/B picture predicts from the right anchors. */
static int read_pic_header(struct mpeg_decoder *d)
{
	br_get(&d->br, 10);                                /* temporal_reference       */
	d->pic_type = (int)br_get(&d->br, 3);
	br_get(&d->br, 16);                                /* vbv_delay                */
	d->full_pel_fwd = d->f_code_fwd = d->full_pel_bwd = d->f_code_bwd = 0;
	if (d->pic_type == MPEG_PIC_P || d->pic_type == MPEG_PIC_B) {
		d->full_pel_fwd = (int)br_get1(&d->br);
		d->f_code_fwd = (int)br_get(&d->br, 3);
	}
	if (d->pic_type == MPEG_PIC_B) {
		d->full_pel_bwd = (int)br_get1(&d->br);
		d->f_code_bwd = (int)br_get(&d->br, 3);
	}
	while (br_get1(&d->br)) br_get(&d->br, 8);         /* extra_information_picture */
	if (d->pic_type < MPEG_PIC_I || d->pic_type > MPEG_PIC_B) return MPEG_EUNSUP;
	return MPEG_OK;
}

static int decode_pic_slices(struct mpeg_decoder *d)
{
	int code;
	for (;;) {
		code = br_next_start(&d->br);
		if (code < 0) { d->eos = 1; break; }
		if (code >= 0x01 && code <= 0xAF) {
			br_get(&d->br, 8);                         /* consume slice id         */
			if (decode_slice(d, code) != 0) return MPEG_EFORMAT;
		} else {
			/* next picture / GOP / sequence-end: rewind to the start code so the
			 * caller's loop sees it, then stop this picture. */
			d->br.pos -= 3; d->br.bit = 0;
			break;
		}
	}
	return MPEG_OK;
}

/* ---- headers -------------------------------------------------------------- */
static void load_quant(struct mpeg_decoder *d)
{
	memcpy(d->intra_q, mpeg_default_intra, 64);
	memcpy(d->non_intra_q, mpeg_default_non_intra, 64);
}

static int read_seq_header(struct mpeg_decoder *d)
{
	int i;
	d->width  = (int)br_get(&d->br, 12);
	d->height = (int)br_get(&d->br, 12);
	br_get(&d->br, 4);                                 /* aspect_ratio             */
	br_get(&d->br, 4);                                 /* frame_rate_code          */
	br_get(&d->br, 18);                                /* bit_rate                 */
	br_get(&d->br, 1);                                 /* marker                   */
	br_get(&d->br, 10);                                /* vbv_buffer_size          */
	br_get(&d->br, 1);                                 /* constrained_parameters   */
	if (d->width < 1 || d->height < 1 || d->width > 4095 || d->height > 4095)
		return MPEG_EFORMAT;

	if (br_get1(&d->br)) {                             /* load_intra_quantiser     */
		for (i = 0; i < 64; i++) d->intra_q[mpeg_zigzag[i]] = (unsigned char)br_get(&d->br, 8);
	}
	if (br_get1(&d->br)) {                             /* load_non_intra_quantiser */
		for (i = 0; i < 64; i++) d->non_intra_q[mpeg_zigzag[i]] = (unsigned char)br_get(&d->br, 8);
	}
	return MPEG_OK;
}

/* Allocate the padded frame store once dimensions are known. */
static int alloc_frames(struct mpeg_decoder *d)
{
	unsigned ysz, csz, total;
	unsigned char *p;
	d->mb_w = (d->width + 15) / 16;
	d->mb_h = (d->height + 15) / 16;
	d->yw = d->mb_w * 16; d->yh = d->mb_h * 16;
	d->cw = d->mb_w * 8;  d->ch = d->mb_h * 8;
	ysz = (unsigned)d->yw * d->yh;
	csz = (unsigned)d->cw * d->ch;
	total = (ysz + 2 * csz) * 3;
	p = (unsigned char *)malloc(total);
	if (!p) return MPEG_ENOMEM;
	memset(p, 0, total);
	d->pool = p;
	d->cur.y = p;                       d->cur.cb = p + ysz;           d->cur.cr = p + ysz + csz;
	p += ysz + 2 * csz;
	d->ref_fwd.y = p;                   d->ref_fwd.cb = p + ysz;       d->ref_fwd.cr = p + ysz + csz;
	p += ysz + 2 * csz;
	d->ref_bwd.y = p;                   d->ref_bwd.cb = p + ysz;       d->ref_bwd.cr = p + ysz + csz;
	return MPEG_OK;
}

/* ---- public API ----------------------------------------------------------- */
int mpeg_decoder_open(const unsigned char *buf, unsigned len, mpeg_decoder **out)
{
	struct mpeg_decoder *d;
	int code, rc;
	if (out) *out = 0;
	if (!buf || len < 4 || !out) return MPEG_EBADSIG;

	d = (struct mpeg_decoder *)calloc(1, sizeof *d);
	if (!d) return MPEG_ENOMEM;
	d->buf = buf; d->len = len;
	br_init(&d->br, buf, len);
	load_quant(d);

	/* find the first sequence header */
	for (;;) {
		code = br_next_start(&d->br);
		if (code < 0) { free(d); return MPEG_EBADSIG; }
		if (code == SEQ_START) { br_get(&d->br, 8); break; }
		br_get(&d->br, 8);
	}
	rc = read_seq_header(d);
	if (rc != MPEG_OK) { free(d); return rc; }
	rc = alloc_frames(d);
	if (rc != MPEG_OK) { free(d); return rc; }

	*out = d;
	return MPEG_OK;
}

void mpeg_decoder_close(mpeg_decoder *d)
{ if (d) { free(d->pool); free(d); } }

int mpeg_width(const mpeg_decoder *d)  { return d ? d->width : 0; }
int mpeg_height(const mpeg_decoder *d) { return d ? d->height : 0; }
int mpeg_frame_type(const mpeg_decoder *d) { return d ? d->last_type : 0; }

/* MPEG stores anchors (I/P) in decode order, ahead of the B pictures that display
 * between them, so decode order != display order. The reorder rule: a B picture
 * displays as soon as it is decoded; a newly decoded anchor is held back one step
 * (in ref_bwd) and only displayed when the *next* anchor arrives — at which point
 * it slides to ref_fwd and the arriving anchor takes ref_bwd. The final held
 * anchor is flushed at end of stream. This yields display order for both plain
 * I/P/P streams and I/B/B/P streams. */
int mpeg_decode_frame(mpeg_decoder *d)
{
	int code, rc;
	if (!d || (d->eos && !d->have_pending)) return MPEG_EEOF;

	for (;;) {
		code = br_next_start(&d->br);
		if (code < 0) {                                /* end of stream           */
			d->eos = 1;
			if (d->have_pending) {                     /* flush the last anchor   */
				d->last = d->ref_bwd; d->last_type = d->bwd_type; d->have_pending = 0;
				return MPEG_OK;
			}
			return MPEG_EEOF;
		}
		br_get(&d->br, 8);                             /* consume the start-code id */

		if (code == SEQ_START) { rc = read_seq_header(d); if (rc != MPEG_OK) return rc; continue; }
		if (code == GOP_START) { br_get(&d->br, 27); continue; }  /* time_code + flags */
		if (code == EXT_START || code == USER_START) continue;    /* skip to next SC   */
		if (code == SEQ_END) { d->eos = 1; if (d->have_pending) { d->last = d->ref_bwd; d->last_type = d->bwd_type; d->have_pending = 0; return MPEG_OK; } return MPEG_EEOF; }
		if (code != PIC_START) continue;

		rc = read_pic_header(d);
		if (rc != MPEG_OK) return rc;

		if (d->pic_type == MPEG_PIC_B) {
			/* Decode into cur using ref_fwd (past) + ref_bwd (future); show it. */
			rc = decode_pic_slices(d);
			if (rc != MPEG_OK) return rc;
			d->last = d->cur; d->last_type = MPEG_PIC_B;
			return MPEG_OK;
		}

		/* An anchor (I or P). */
		if (!d->have_fwd) {                            /* the very first anchor    */
			frame t;
			rc = decode_pic_slices(d);
			if (rc != MPEG_OK) return rc;
			t = d->ref_fwd; d->ref_fwd = d->cur; d->cur = t;   /* cur -> past      */
			d->have_fwd = 1;
			d->last = d->ref_fwd; d->last_type = d->pic_type;
			return MPEG_OK;
		}

		if (d->have_pending) {
			/* Promote the pending future to past (display it), decode the new
			 * anchor predicting from it, and hold the new one as the future. */
			frame t = d->ref_fwd; d->ref_fwd = d->ref_bwd; d->ref_bwd = t;  /* swap */
			int shown_type = d->bwd_type;
			rc = decode_pic_slices(d);
			if (rc != MPEG_OK) return rc;
			t = d->ref_bwd; d->ref_bwd = d->cur; d->cur = t;   /* new anchor -> future */
			d->have_bwd = 1;
			d->bwd_type = d->pic_type;
			d->last = d->ref_fwd; d->last_type = shown_type;   /* the promoted anchor */
			return MPEG_OK;
		}

		/* No pending future yet (just after an anchor was shown): decode this
		 * anchor as the future and keep going to fetch the frame to display now. */
		{
			frame t;
			rc = decode_pic_slices(d);
			if (rc != MPEG_OK) return rc;
			t = d->ref_bwd; d->ref_bwd = d->cur; d->cur = t;
			d->have_bwd = 1;
			d->bwd_type = d->pic_type;
			d->have_pending = 1;
			continue;
		}
	}
}

const unsigned char *mpeg_plane_y(const mpeg_decoder *d, int *stride)
{ if (stride) *stride = d ? d->yw : 0; return d ? d->last.y : 0; }
const unsigned char *mpeg_plane_cb(const mpeg_decoder *d, int *stride)
{ if (stride) *stride = d ? d->cw : 0; return d ? d->last.cb : 0; }
const unsigned char *mpeg_plane_cr(const mpeg_decoder *d, int *stride)
{ if (stride) *stride = d ? d->cw : 0; return d ? d->last.cr : 0; }

int mpeg_frame_rgb(const mpeg_decoder *d, unsigned char *rgb)
{
	int x, y;
	if (!d || !rgb) return MPEG_EFORMAT;
	for (y = 0; y < d->height; y++)
		for (x = 0; x < d->width; x++) {
			int Y = d->last.y[y * d->yw + x];
			int U = d->last.cb[(y / 2) * d->cw + (x / 2)] - 128;
			int V = d->last.cr[(y / 2) * d->cw + (x / 2)] - 128;
			int r = Y + ((91881 * V) >> 16);
			int g = Y - ((22554 * U + 46802 * V) >> 16);
			int b = Y + ((116130 * U) >> 16);
			unsigned char *o = rgb + (y * d->width + x) * 3;
			o[0] = r < 0 ? 0 : r > 255 ? 255 : r;
			o[1] = g < 0 ? 0 : g > 255 ? 255 : g;
			o[2] = b < 0 ? 0 : b > 255 ? 255 : b;
		}
	return MPEG_OK;
}
