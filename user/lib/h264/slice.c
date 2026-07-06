/* slice.c — slice header parsing and the macroblock layer (H.264 7.3.3–7.3.5).
 *
 * This drives one slice end to end: it reads the header, opens/closes the picture
 * through the h264.c helpers, then walks the macroblocks — classifying each into
 * an intra (I_4x4/I_16x16/I_PCM) or inter (P) category, parsing prediction modes,
 * motion vectors and the coded_block_pattern, and reconstructing it by dispatching
 * to intra/inter prediction, CAVLC residual decode, dequant and the inverse
 * transform. Per-macroblock nnz / mode / motion state is stored for neighbours.
 */
#include "owos.h"
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"

/* Table 9-4: coded_block_pattern mapped Exp-Golomb (ChromaArrayType 1/2). */
static const unsigned char cbp_intra[48] = {
	47,31,15, 0,23,27,29,30, 7,11,13,14,39,43,45,46,16, 3, 5,10,12,19,21,26,28,
	35,37,42,44, 1, 2, 4, 8,17,18,20,24, 6, 9,22,25,32,33,34,36,40,38,41,
};
static const unsigned char cbp_inter[48] = {
	 0,16, 1, 2, 4, 8,32, 3, 5,10,12,15,47, 7,11,13,14, 6, 9,31,35,37,42,44,33,
	34,36,40,39,43,45,46,17,18,20,24,19,21,26,28,23,27,29,30,22,25,38,41,
};

/* ---- nnz neighbour derivation (nC) --------------------------------------- */
static int nc_combine(int a, int b)
{
	if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
	if (a >= 0) return a;
	if (b >= 0) return b;
	return 0;
}

static int nc_luma(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int blk)
{
	int bx = h264_blk_x[blk] >> 2, by = h264_blk_y[blk] >> 2, nA = -1, nB = -1;
	if (bx > 0) nA = cur->nnz_l[by * 4 + bx - 1];
	else if (h264_mb_avail(d, cur_addr, mbx - 1, mby)) nA = h264_mb_at(d, mbx - 1, mby)->nnz_l[by * 4 + 3];
	if (by > 0) nB = cur->nnz_l[(by - 1) * 4 + bx];
	else if (h264_mb_avail(d, cur_addr, mbx, mby - 1)) nB = h264_mb_at(d, mbx, mby - 1)->nnz_l[3 * 4 + bx];
	return nc_combine(nA, nB);
}

static int nc_chroma(struct h264_decoder *d, h264_mb *cur, int cur_addr, int mbx, int mby, int comp, int blk)
{
	unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
	int bx = blk & 1, by = blk >> 1, nA = -1, nB = -1;
	if (bx > 0) nA = cn[by * 2 + bx - 1];
	else if (h264_mb_avail(d, cur_addr, mbx - 1, mby)) {
		h264_mb *m = h264_mb_at(d, mbx - 1, mby);
		nA = (comp == 0 ? m->nnz_cb : m->nnz_cr)[by * 2 + 1];
	}
	if (by > 0) nB = cn[(by - 1) * 2 + bx];
	else if (h264_mb_avail(d, cur_addr, mbx, mby - 1)) {
		h264_mb *m = h264_mb_at(d, mbx, mby - 1);
		nB = (comp == 0 ? m->nnz_cb : m->nnz_cr)[1 * 2 + bx];
	}
	return nc_combine(nA, nB);
}

/* ---- residual reconstruction helpers -------------------------------------- */
/* Decode + reconstruct one luma 4x4 block. `dcval` is the (already scaled) DC to
 * insert for Intra_16x16 (dc_sep=1), else the DC comes from the block itself. */
static int recon_luma_block(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                            int mbx, int mby, int blk, int coded, int dc_sep, int dcval, int qp)
{
	int coeff[16], raster[16] = {0}, k, tc = 0;
	unsigned char *dst = d->cur->y + (mby * 16 + h264_blk_y[blk]) * d->ystride + mbx * 16 + h264_blk_x[blk];
	int rpos = (h264_blk_y[blk] >> 2) * 4 + (h264_blk_x[blk] >> 2);
	if (coded) {
		int nC = nc_luma(d, cur, cur_addr, mbx, mby, blk);
		tc = h264_cavlc_block(&d->br, nC, dc_sep ? 15 : 16, coeff);
		if (tc < 0) return -1;
		for (k = 0; k < (dc_sep ? 15 : 16); k++)
			raster[h264_zigzag4[dc_sep ? k + 1 : k]] = coeff[k];
	}
	cur->nnz_l[rpos] = (unsigned char)tc;
	if (dc_sep) { raster[0] = dcval; h264_dequant_4x4(raster, qp, 1); }
	else        { h264_dequant_4x4(raster, qp, 0); }
	if (coded || dc_sep) h264_idct4x4_add(raster, dst, d->ystride);
	return 0;
}

static int recon_chroma_block(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                              int mbx, int mby, int comp, int blk, int coded, int dcval, int qpc)
{
	int coeff[16], raster[16] = {0}, k, tc = 0;
	unsigned char *plane = comp == 0 ? d->cur->cb : d->cur->cr;
	int cx = (blk & 1) * 4, cy = (blk >> 1) * 4;
	unsigned char *dst = plane + (mby * 8 + cy) * d->cstride + mbx * 8 + cx;
	unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
	if (coded) {
		int nC = nc_chroma(d, cur, cur_addr, mbx, mby, comp, blk);
		tc = h264_cavlc_block(&d->br, nC, 15, coeff);
		if (tc < 0) return -1;
		for (k = 0; k < 15; k++) raster[h264_zigzag4[k + 1]] = coeff[k];
	}
	cn[blk] = (unsigned char)tc;
	raster[0] = dcval;
	h264_dequant_4x4(raster, qpc, 1);
	h264_idct4x4_add(raster, dst, d->cstride);
	return 0;
}

/* ---- one macroblock ------------------------------------------------------- */
struct mbinfo { int cat, intra, i16mode, chroma_mode, cbp; };

/* An intra4x4 predicted-mode neighbour (8.3.1.1): its mode (2/DC if the neighbour
 * is not Intra_4x4), and availability (0 also when constrained-intra blocks it). */
static void i4_neighbor(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                        int mbx, int mby, int is_left, int bx, int by, int *avail, int *mode)
{
	h264_mb *m;
	int gi;
	if (is_left) {
		if (bx > 0) { *avail = 1; *mode = cur->i4mode[by * 4 + bx - 1]; return; }
		if (!h264_mb_avail(d, cur_addr, mbx - 1, mby)) { *avail = 0; return; }
		m = h264_mb_at(d, mbx - 1, mby); gi = by * 4 + 3;
	} else {
		if (by > 0) { *avail = 1; *mode = cur->i4mode[(by - 1) * 4 + bx]; return; }
		if (!h264_mb_avail(d, cur_addr, mbx, mby - 1)) { *avail = 0; return; }
		m = h264_mb_at(d, mbx, mby - 1); gi = 3 * 4 + bx;
	}
	if (d->cur_pps->constrained_intra_pred && !m->intra) { *avail = 0; return; }
	*avail = 1;
	*mode = (m->intra && m->cat == MB_I_4x4) ? m->i4mode[gi] : 2;
}

/* Parse the intra-prediction section (modes) for an I_NxN / I_16x16 MB. */
static void parse_intra_pred(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                             int mbx, int mby, struct mbinfo *mb)
{
	int i;
	if (mb->cat == MB_I_4x4) {
		for (i = 0; i < 16; i++) {
			int bx = h264_blk_x[i] >> 2, by = h264_blk_y[i] >> 2, mode;
			int availA, availB, modeA = 2, modeB = 2, pred;
			i4_neighbor(d, cur, cur_addr, mbx, mby, 1, bx, by, &availA, &modeA);
			i4_neighbor(d, cur, cur_addr, mbx, mby, 0, bx, by, &availB, &modeB);
			pred = (!availA || !availB) ? 2 : (modeA < modeB ? modeA : modeB);
			if (br_u1(&d->br)) mode = pred;                      /* prev_intra4x4_pred_mode_flag */
			else { int rem = (int)br_u(&d->br, 3); mode = rem < pred ? rem : rem + 1; }
			cur->i4mode[by * 4 + bx] = (signed char)mode;
		}
		mb->chroma_mode = (int)br_ue(&d->br);
	} else {                                                     /* I_16x16 */
		for (i = 0; i < 16; i++) cur->i4mode[i] = 2;
		mb->chroma_mode = (int)br_ue(&d->br);
	}
}

/* Decode + reconstruct the residual of an intra/inter MB. Prediction for I_16x16,
 * chroma and inter is already in the plane; I_4x4 luma is predicted per block. */
static int recon_mb(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                    int mbx, int mby, struct mbinfo *mb, int qp)
{
	int i8, i4, blk, comp, dcv_luma[16] = {0};
	int cbp_luma = mb->cbp & 0x0f, cbp_chroma = (mb->cbp >> 4) & 3;
	int qpc = h264_chroma_qp(qp, d->cur_pps->chroma_qp_index_offset);

	/* Luma DC for Intra_16x16. */
	if (mb->cat == MB_I_16x16) {
		int coeff[16], k, nC = nc_luma(d, cur, cur_addr, mbx, mby, 0);
		int tc = h264_cavlc_block(&d->br, nC, 16, coeff);
		if (tc < 0) return -1;
		for (k = 0; k < 16; k++) dcv_luma[h264_zigzag4[k]] = coeff[k];
		h264_hadamard4x4(dcv_luma);
		h264_dequant_luma_dc(dcv_luma, qp);
	}

	/* Luma blocks in scan order. */
	for (i8 = 0; i8 < 4; i8++) {
		int gcoded = (cbp_luma >> i8) & 1;
		for (i4 = 0; i4 < 4; i4++) {
			blk = i8 * 4 + i4;
			if (mb->cat == MB_I_4x4)
				h264_intra4x4(d, mbx, mby, blk, cur->i4mode[(h264_blk_y[blk]>>2)*4 + (h264_blk_x[blk]>>2)]);
			if (mb->cat == MB_I_16x16) {
				int rpos = (h264_blk_y[blk] >> 2) * 4 + (h264_blk_x[blk] >> 2);
				if (recon_luma_block(d, cur, cur_addr, mbx, mby, blk, gcoded, 1, dcv_luma[rpos], qp) != 0) return -1;
			} else {
				if (recon_luma_block(d, cur, cur_addr, mbx, mby, blk, gcoded, 0, 0, qp) != 0) return -1;
			}
		}
	}

	/* Chroma DC (both components), then chroma AC. */
	{
		int dcv[2][4] = {{0}}, cc;
		if (cbp_chroma & 3) {
			for (cc = 0; cc < 2; cc++) {
				int coeff[4], tc = h264_cavlc_block(&d->br, -1, 4, coeff);
				if (tc < 0) return -1;
				dcv[cc][0] = coeff[0]; dcv[cc][1] = coeff[1];
				dcv[cc][2] = coeff[2]; dcv[cc][3] = coeff[3];
				h264_chroma_dc_transform(dcv[cc]);
				h264_dequant_chroma_dc(dcv[cc], qpc);
			}
		}
		for (comp = 0; comp < 2; comp++) {
			unsigned char *cn = comp == 0 ? cur->nnz_cb : cur->nnz_cr;
			for (blk = 0; blk < 4; blk++) {
				int coded = (cbp_chroma == 2);
				if (!coded) cn[blk] = 0;
				if (recon_chroma_block(d, cur, cur_addr, mbx, mby, comp, blk, coded, dcv[comp][blk], qpc) != 0)
					return -1;
			}
		}
	}
	return 0;
}

/* Classify the coded mb_type into a category; fill I_16x16 mode/cbp from it. */
static int classify(int slice_type, int mb_type, struct mbinfo *mb, int *is_pcm, int *p_parts)
{
	int m;
	*is_pcm = 0; *p_parts = 0;
	mb->i16mode = 0; mb->cbp = 0;
	if (slice_type == H264_SLICE_P && mb_type < 5) {
		mb->intra = 0;
		switch (mb_type) {
		case 0: mb->cat = MB_P_16x16; *p_parts = 1; break;
		case 1: mb->cat = MB_P_16x8;  *p_parts = 2; break;
		case 2: mb->cat = MB_P_8x16;  *p_parts = 2; break;
		case 3: mb->cat = MB_P_8x8;   *p_parts = 4; break;
		case 4: mb->cat = MB_P_8x8;   *p_parts = 4; break;   /* P_8x8ref0 */
		}
		mb->i16mode = (mb_type == 4);   /* reuse as "ref0-forced" flag */
		return 0;
	}
	m = (slice_type == H264_SLICE_P) ? mb_type - 5 : mb_type;   /* intra table */
	mb->intra = 1;
	if (m == 0) { mb->cat = MB_I_4x4; return 0; }
	if (m == 25) { mb->cat = MB_I_PCM; *is_pcm = 1; return 0; }
	if (m >= 1 && m <= 24) {
		int e = m - 1;
		mb->cat = MB_I_16x16;
		mb->i16mode = e % 4;
		mb->cbp = (((e / 4) % 3) << 4) | ((e >= 12) ? 0x0f : 0);
		return 0;
	}
	return -1;
}

static void store_inter_mv(h264_mb *cur, int bx4, int by4, int bw4, int bh4, int mvx, int mvy, int ref)
{
	int x, y;
	for (y = 0; y < bh4; y++) for (x = 0; x < bw4; x++) {
		int idx = (by4 + y) * 4 + bx4 + x;
		cur->mv[idx][0] = (short)mvx; cur->mv[idx][1] = (short)mvy;
		cur->refidx[idx] = (signed char)ref;
	}
}

/* ref_idx_l0 is te(v): a single inverted bit when the active count is 2, else
 * ue(v); 0 bits when only one reference is active. */
static int read_ref_idx(struct h264_decoder *d, int num_refs)
{
	if (num_refs <= 1) return 0;
	if (num_refs == 2) return 1 - (int)br_u1(&d->br);
	return (int)br_ue(&d->br);
}

/* One sub-block of a P_8x8 sub_mb_type: offsets/size in 4x4 units. */
struct subgeom { int n; int off[4][4]; };
static const struct subgeom sub_geom[4] = {
	{ 1, { {0,0,2,2} } },                                  /* 8x8 */
	{ 2, { {0,0,2,1}, {0,1,2,1} } },                       /* 8x4 */
	{ 2, { {0,0,1,2}, {1,0,1,2} } },                       /* 4x8 */
	{ 4, { {0,0,1,1}, {1,0,1,1}, {0,1,1,1}, {1,1,1,1} } }, /* 4x4 */
};

/* Parse + motion-compensate a P macroblock's partitions. */
static int decode_p_inter(struct h264_decoder *d, h264_mb *cur, int cur_addr,
                          int mbx, int mby, struct mbinfo *mb, int p_parts)
{
	int num_refs = d->sh.num_ref_idx_l0, part;
	int ref0_forced = mb->i16mode;   /* set for P_8x8ref0 */

	if (mb->cat == MB_P_8x8) {
		int sub[4], ref[4], p, s;
		for (p = 0; p < 4; p++) { sub[p] = (int)br_ue(&d->br); if (sub[p] > 3) return -1; }
		for (p = 0; p < 4; p++)
			ref[p] = (!ref0_forced) ? read_ref_idx(d, num_refs) : 0;
		for (p = 0; p < 4; p++) {
			int p8x = (p & 1) * 2, p8y = (p >> 1) * 2;     /* 8x8 base in 4x4 units */
			const struct subgeom *g = &sub_geom[sub[p]];
			for (s = 0; s < g->n; s++) {
				int sbx = p8x + g->off[s][0], sby = p8y + g->off[s][1];
				int sw = g->off[s][2], sh = g->off[s][3];
				int mvx, mvy;
				h264_pred_mv(d, mbx, mby, sbx, sby, sw * 4, sh * 4, ref[p], &mvx, &mvy);
				mvx += br_se(&d->br); mvy += br_se(&d->br);
				store_inter_mv(cur, sbx, sby, sw, sh, mvx, mvy, ref[p]);
				h264_inter_pred(d, mbx, mby, sbx * 4, sby * 4, sw * 4, sh * 4,
				                d->ref0[ref[p] < d->nref0 ? ref[p] : 0], mvx, mvy);
			}
		}
		return 0;
	}

	/* macroblock_prediction: all ref_idx_l0 first, then all mvd_l0. */
	{
		int pref[2], part2;
		for (part2 = 0; part2 < p_parts; part2++) pref[part2] = read_ref_idx(d, num_refs);
		for (part = 0; part < p_parts; part++) {
			int pw = 16, ph = 16, bx4 = 0, by4 = 0, ref = pref[part], mvx, mvy;
			if (mb->cat == MB_P_16x8)      { pw = 16; ph = 8;  by4 = part * 2; }
			else if (mb->cat == MB_P_8x16) { pw = 8;  ph = 16; bx4 = part * 2; }
			h264_pred_mv(d, mbx, mby, bx4, by4, pw, ph, ref, &mvx, &mvy);
			mvx += br_se(&d->br); mvy += br_se(&d->br);
			store_inter_mv(cur, bx4, by4, pw / 4, ph / 4, mvx, mvy, ref);
			h264_inter_pred(d, mbx, mby, bx4 * 4, by4 * 4, pw, ph,
			                d->ref0[ref < d->nref0 ? ref : 0], mvx, mvy);
		}
	}
	return 0;
}

static int decode_pcm(struct h264_decoder *d, h264_mb *cur, int mbx, int mby)
{
	int x, y;
	br_align(&d->br);
	for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
		d->cur->y[(mby * 16 + y) * d->ystride + mbx * 16 + x] = (unsigned char)br_u(&d->br, 8);
	for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
		d->cur->cb[(mby * 8 + y) * d->cstride + mbx * 8 + x] = (unsigned char)br_u(&d->br, 8);
	for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
		d->cur->cr[(mby * 8 + y) * d->cstride + mbx * 8 + x] = (unsigned char)br_u(&d->br, 8);
	{ int i; for (i = 0; i < 16; i++) cur->nnz_l[i] = 16; for (i = 0; i < 4; i++) { cur->nnz_cb[i] = 16; cur->nnz_cr[i] = 16; } }
	cur->qpy = 0;
	return d->br.overrun ? -1 : 0;
}

static void decode_p_skip(struct h264_decoder *d, int cur_addr, int mbx, int mby, int *qp)
{
	h264_mb *cur = &d->mbs[cur_addr];
	int mvx, mvy;
	cur->slice_id = d->cur_slice_id;
	cur->intra = 0; cur->cat = MB_SKIP; cur->cbp = 0; cur->qpy = (signed char)*qp;
	h264_skip_mv(d, mbx, mby, &mvx, &mvy);
	store_inter_mv(cur, 0, 0, 4, 4, mvx, mvy, 0);
	h264_inter_pred(d, mbx, mby, 0, 0, 16, 16, d->ref0[0], mvx, mvy);
}

static int decode_mb(struct h264_decoder *d, int cur_addr, int mbx, int mby, int *qp)
{
	h264_mb *cur = &d->mbs[cur_addr];
	struct mbinfo mb;
	int mb_type = (int)br_ue(&d->br);
	int is_pcm = 0, p_parts = 0;
	memset(&mb, 0, sizeof mb);
	cur->slice_id = d->cur_slice_id;
	{ int i; for (i = 0; i < 16; i++) cur->refidx[i] = -1; }

	if (classify(d->sh.slice_type, mb_type, &mb, &is_pcm, &p_parts) != 0) return -1;
	cur->cat = (unsigned char)mb.cat;
	cur->intra = (unsigned char)mb.intra;
	cur->chroma_mode = 0;

	if (is_pcm) { cur->intra = 1; return decode_pcm(d, cur, mbx, mby); }

	if (mb.intra) {
		parse_intra_pred(d, cur, cur_addr, mbx, mby, &mb);
		cur->chroma_mode = (unsigned char)mb.chroma_mode;
	} else {
		if (decode_p_inter(d, cur, cur_addr, mbx, mby, &mb, p_parts) != 0) return -1;
	}

	/* coded_block_pattern for I_4x4 and P (I_16x16 already has it from mb_type). */
	if (mb.cat == MB_I_4x4 || (!mb.intra)) {
		int code = (int)br_ue(&d->br);
		if (code >= 48) return -1;
		mb.cbp = mb.intra ? cbp_intra[code] : cbp_inter[code];
	}
	cur->cbp = (unsigned char)mb.cbp;

	/* Intra_16x16 and chroma prediction (whole-MB) before residual. */
	if (mb.cat == MB_I_16x16) h264_intra16x16(d, mbx, mby, mb.i16mode);
	if (mb.intra) h264_intra_chroma(d, mbx, mby, mb.chroma_mode);

	/* mb_qp_delta present when there are coded coefficients or Intra_16x16. */
	if (mb.cbp != 0 || mb.cat == MB_I_16x16) {
		int delta = br_se(&d->br);
		*qp = (*qp + delta + 52) % 52;
	}
	cur->qpy = (signed char)*qp;

	if (recon_mb(d, cur, cur_addr, mbx, mby, &mb, *qp) != 0) return -1;
	return d->br.overrun ? -1 : 0;
}

/* ---- slice header + dispatch ---------------------------------------------- */
static int parse_slice_header(struct h264_decoder *d, int nal_type, int nal_ref_idc)
{
	h264_slice *sh = &d->sh;
	h264_sps *s; h264_pps *p;
	int pps_id, rc;
	memset(sh, 0, sizeof *sh);
	sh->idr = (nal_type == 5);
	sh->nal_ref_idc = nal_ref_idc;

	sh->first_mb = (int)br_ue(&d->br);
	sh->slice_type = (int)br_ue(&d->br) % 5;
	pps_id = (int)br_ue(&d->br);
	if (pps_id < 0 || pps_id > 255 || !d->pps[pps_id].valid) return H264_EFORMAT;
	p = &d->pps[pps_id];
	rc = h264_ensure_geometry(d, p);
	if (rc != H264_OK) return rc;
	s = d->cur_sps;

	sh->frame_num = (int)br_u(&d->br, s->log2_max_frame_num);
	if (sh->idr) sh->idr_pic_id = (int)br_ue(&d->br);
	if (s->poc_type == 0) {
		sh->poc_lsb = (int)br_u(&d->br, s->log2_max_poc_lsb);
		if (p->bottom_field_poc_present) sh->delta_poc_bottom = br_se(&d->br);
	} else if (s->poc_type == 1 && !s->delta_poc_always_zero) {
		sh->delta_poc[0] = br_se(&d->br);
		if (p->bottom_field_poc_present) sh->delta_poc[1] = br_se(&d->br);
	}
	if (p->redundant_pic_cnt_present) br_ue(&d->br);

	sh->num_ref_idx_l0 = p->num_ref_idx_l0_default;
	if (sh->slice_type == H264_SLICE_P || sh->slice_type == H264_SLICE_SP) {
		if (br_u1(&d->br)) sh->num_ref_idx_l0 = (int)br_ue(&d->br) + 1;   /* override */
		/* ref_pic_list_modification_flag_l0 */
		if (br_u1(&d->br)) {
			for (;;) {
				int idc = (int)br_ue(&d->br);
				if (idc == 3) break;
				if (idc == 0 || idc == 1) br_ue(&d->br);       /* abs_diff_pic_num_minus1 */
				else if (idc == 2) br_ue(&d->br);              /* long_term_pic_num       */
				if (d->br.overrun) return H264_EFORMAT;
			}
		}
	}
	if (p->weighted_pred && (sh->slice_type == H264_SLICE_P || sh->slice_type == H264_SLICE_SP)) {
		/* pred_weight_table: luma_log2_weight_denom, chroma_..., then per-ref. */
		int luma_denom, chroma_denom, i;
		luma_denom = (int)br_ue(&d->br);
		chroma_denom = (int)br_ue(&d->br);
		(void)luma_denom; (void)chroma_denom;
		for (i = 0; i < sh->num_ref_idx_l0; i++) {
			if (br_u1(&d->br)) { br_se(&d->br); br_se(&d->br); }        /* luma weight/offset */
			if (br_u1(&d->br)) { br_se(&d->br); br_se(&d->br); br_se(&d->br); br_se(&d->br); }
		}
		return H264_EUNSUP;   /* weighted prediction not applied in reconstruction */
	}
	if (nal_ref_idc) {
		if (sh->idr) { br_u1(&d->br); br_u1(&d->br); }
		else if (br_u1(&d->br)) {                              /* adaptive_ref_pic_marking */
			for (;;) {
				int op = (int)br_ue(&d->br);
				if (op == 0) break;
				if (op == 1 || op == 3) br_ue(&d->br);
				if (op == 2) br_ue(&d->br);
				if (op == 3 || op == 6) br_ue(&d->br);
				if (op == 4) br_ue(&d->br);
				if (d->br.overrun) return H264_EFORMAT;
			}
		}
	}

	sh->qp = p->pic_init_qp + br_se(&d->br);
	if (p->deblock_control_present) {
		sh->disable_deblock = (int)br_ue(&d->br);
		if (sh->disable_deblock != 1) {
			sh->slice_alpha_off = 2 * br_se(&d->br);
			sh->slice_beta_off  = 2 * br_se(&d->br);
		}
	}
	if (d->br.overrun) return H264_EFORMAT;
	if (sh->slice_type == H264_SLICE_B) return H264_EUNSUP;
	return H264_OK;
}

int h264_decode_slice_dispatch(struct h264_decoder *d, const unsigned char *rbsp,
                               unsigned len, int nal_type, int nal_ref_idc, int *emitted)
{
	int rc, total, addr, qp;
	*emitted = 0;
	br_init(&d->br, rbsp, len);
	rc = parse_slice_header(d, nal_type, nal_ref_idc);
	if (rc != H264_OK) return rc;

	if (d->sh.first_mb == 0) {
		*emitted = h264_close_open_picture(d);
		rc = h264_start_picture(d, &d->sh, nal_ref_idc);
		if (rc != H264_OK) return rc;
		d->slice_count = 0;
	} else if (!d->have_pic) {
		return H264_EFORMAT;
	}
	d->cur_slice_id = d->slice_count++;

	if (d->sh.slice_type == H264_SLICE_P) h264_build_ref_list0(d);
	if (d->sh.slice_type == H264_SLICE_P && d->nref0 == 0) return H264_EFORMAT;

	total = d->mb_w * d->mb_h;
	addr = d->sh.first_mb;
	qp = d->sh.qp;

	if (d->sh.slice_type == H264_SLICE_I) {
		while (addr < total) {
			if (decode_mb(d, addr, addr % d->mb_w, addr / d->mb_w, &qp) != 0) return H264_EFORMAT;
			addr++;
			if (!br_more_rbsp_data(&d->br)) break;
		}
	} else {   /* P */
		int more = 1;
		while (more && addr < total) {
			int skip = (int)br_ue(&d->br);
			while (skip-- > 0 && addr < total) {
				decode_p_skip(d, addr, addr % d->mb_w, addr / d->mb_w, &qp);
				addr++;
			}
			if (!br_more_rbsp_data(&d->br)) break;
			if (addr >= total) break;
			if (decode_mb(d, addr, addr % d->mb_w, addr / d->mb_w, &qp) != 0) return H264_EFORMAT;
			addr++;
			more = br_more_rbsp_data(&d->br);
		}
	}
	return H264_OK;
}
