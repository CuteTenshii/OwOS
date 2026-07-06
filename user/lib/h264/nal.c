/* nal.c — NAL payload de-escaping (RBSP extraction) and SPS/PPS parsing.
 *
 * A NAL unit's payload replaces every 00 00 00/01/02/03 byte-pattern with
 * 00 00 03 xx so the payload can never be mistaken for a start code. h264_rbsp()
 * copies a NAL into the decoder's scratch buffer, dropping those emulation-
 * prevention 03 bytes once, so the bit reader downstream sees clean RBSP. The
 * SPS/PPS parsers then read only the fields the baseline decoder consumes and
 * reject (H264_EUNSUP) any tool outside Constrained Baseline / Baseline CAVLC.
 */
#include "owos.h"
#include "h264.h"
#include "h264_priv.h"

/* Copy `nal`/`nlen` into d->rbsp, removing emulation-prevention bytes. Returns
 * the RBSP length (0 on allocation failure). */
unsigned h264_rbsp(struct h264_decoder *d, const unsigned char *nal, unsigned nlen)
{
	unsigned i, o = 0, zeros = 0;
	if (nlen + 1 > d->rbsp_cap) {
		unsigned nc = nlen + 16;
		unsigned char *nb = (unsigned char *)realloc(d->rbsp, nc);
		if (!nb) return 0;
		d->rbsp = nb; d->rbsp_cap = nc;
	}
	for (i = 0; i < nlen; i++) {
		unsigned char c = nal[i];
		if (zeros >= 2 && c == 3 && i + 1 < nlen && nal[i + 1] <= 3) {
			zeros = 0;            /* drop this 03, keep scanning from the next byte */
			continue;
		}
		d->rbsp[o++] = c;
		zeros = (c == 0) ? zeros + 1 : 0;
	}
	return o;
}

/* Skip the scaling-list syntax we don't support; presence is a hard reject. */
static int reject_scaling(bitr *b, int count)
{
	(void)b; (void)count;
	return H264_EUNSUP;
}

int h264_parse_sps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len)
{
	bitr b;
	h264_sps s;
	int sps_id, i, high = 0, crop_unit_x, crop_unit_y;
	memset(&s, 0, sizeof s);
	br_init(&b, rbsp, len);

	s.profile_idc = (int)br_u(&b, 8);
	br_u(&b, 8);                       /* constraint flags + reserved            */
	s.level_idc   = (int)br_u(&b, 8);
	sps_id = (int)br_ue(&b);
	if (sps_id < 0 || sps_id > 31) return H264_EFORMAT;

	switch (s.profile_idc) {
	case 100: case 110: case 122: case 244: case 44:
	case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
		high = 1; break;
	default: high = 0; break;
	}
	if (high) {
		int chroma_fmt = (int)br_ue(&b);
		if (chroma_fmt == 3) br_u(&b, 1);          /* separate_colour_plane      */
		if (chroma_fmt != 1) return H264_EUNSUP;    /* 4:2:0 only                 */
		if (br_ue(&b) != 0) return H264_EUNSUP;     /* bit_depth_luma_minus8      */
		if (br_ue(&b) != 0) return H264_EUNSUP;     /* bit_depth_chroma_minus8    */
		br_u(&b, 1);                                /* qpprime_y_zero_transform   */
		if (br_u(&b, 1)) return reject_scaling(&b, 8);   /* seq_scaling_matrix    */
	}

	s.log2_max_frame_num = (int)br_ue(&b) + 4;
	s.poc_type = (int)br_ue(&b);
	if (s.poc_type == 0) {
		s.log2_max_poc_lsb = (int)br_ue(&b) + 4;
	} else if (s.poc_type == 1) {
		s.delta_poc_always_zero = (int)br_u(&b, 1);
		s.offset_for_non_ref_pic = br_se(&b);
		s.offset_for_top_to_bottom = br_se(&b);
		s.num_ref_frames_in_poc_cycle = (int)br_ue(&b);
		if (s.num_ref_frames_in_poc_cycle > 255) return H264_EFORMAT;
		for (i = 0; i < s.num_ref_frames_in_poc_cycle; i++)
			s.offset_for_ref_frame[i] = br_se(&b);
	} else if (s.poc_type != 2) {
		return H264_EFORMAT;
	}

	s.max_num_ref_frames = (int)br_ue(&b);
	br_u(&b, 1);                                    /* gaps_in_frame_num allowed  */
	s.mb_w = (int)br_ue(&b) + 1;
	{
		int map_units = (int)br_ue(&b) + 1;
		s.frame_mbs_only = (int)br_u(&b, 1);
		if (!s.frame_mbs_only) return H264_EUNSUP;   /* no interlaced / fields    */
		s.mb_h = map_units;
	}
	s.direct_8x8_inference = (int)br_u(&b, 1);
	if (br_u(&b, 1)) {                               /* frame_cropping_flag        */
		s.crop_l = (int)br_ue(&b);
		s.crop_r = (int)br_ue(&b);
		s.crop_t = (int)br_ue(&b);
		s.crop_b = (int)br_ue(&b);
	}
	/* vui_parameters and the rest are ignored. */

	if (b.overrun) return H264_EFORMAT;
	if (s.mb_w < 1 || s.mb_h < 1 || s.mb_w > 543 || s.mb_h > 543) return H264_EFORMAT;

	/* 4:2:0: crop units are 2 luma samples in each dimension (frame-only). */
	crop_unit_x = 2; crop_unit_y = 2;
	s.crop_l *= crop_unit_x; s.crop_r *= crop_unit_x;
	s.crop_t *= crop_unit_y; s.crop_b *= crop_unit_y;

	s.valid = 1;
	d->sps[sps_id] = s;
	return H264_OK;
}

int h264_parse_pps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len)
{
	bitr b;
	h264_pps p;
	int pps_id;
	memset(&p, 0, sizeof p);
	br_init(&b, rbsp, len);

	pps_id = (int)br_ue(&b);
	if (pps_id < 0 || pps_id > 255) return H264_EFORMAT;
	p.sps_id = (int)br_ue(&b);
	if (p.sps_id < 0 || p.sps_id > 31 || !d->sps[p.sps_id].valid) return H264_EFORMAT;

	p.entropy_cabac = (int)br_u(&b, 1);
	p.bottom_field_poc_present = (int)br_u(&b, 1);
	p.num_slice_groups = (int)br_ue(&b) + 1;
	if (p.num_slice_groups > 1) return H264_EUNSUP;   /* no FMO                    */

	p.num_ref_idx_l0_default = (int)br_ue(&b) + 1;
	p.num_ref_idx_l1_default = (int)br_ue(&b) + 1;
	p.weighted_pred = (int)br_u(&b, 1);
	p.weighted_bipred_idc = (int)br_u(&b, 2);
	p.pic_init_qp = (int)br_se(&b) + 26;
	br_se(&b);                                        /* pic_init_qs_minus26       */
	p.chroma_qp_index_offset = br_se(&b);
	p.deblock_control_present = (int)br_u(&b, 1);
	p.constrained_intra_pred = (int)br_u(&b, 1);
	p.redundant_pic_cnt_present = (int)br_u(&b, 1);
	p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;

	if (br_more_rbsp_data(&b)) {
		p.transform_8x8 = (int)br_u(&b, 1);
		if (br_u(&b, 1)) return reject_scaling(&b, 8);   /* pic_scaling_matrix    */
		p.second_chroma_qp_index_offset = br_se(&b);
	}

	if (b.overrun) return H264_EFORMAT;
	if (p.entropy_cabac) return H264_EUNSUP;          /* CAVLC only                */
	if (p.transform_8x8) return H264_EUNSUP;          /* no 8x8 transform          */

	p.valid = 1;
	d->pps[pps_id] = p;
	return H264_OK;
}
