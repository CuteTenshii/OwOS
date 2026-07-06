/* h264_priv.h — shared internal state for the H.264 decoder (see h264.h).
 *
 * The decoder is split across files that all operate on `struct h264_decoder`:
 *   nal.c        NAL extraction + RBSP unescape + SPS/PPS parsing
 *   slice.c      slice header + the macroblock layer loop (per-MB syntax)
 *   cavlc.c      CAVLC residual block decoding + nnz neighbour derivation
 *   transform.c  dequant + 4x4 inverse transform + DC Hadamard, residual add
 *   intra.c      intra 4x4 / 16x16 / chroma prediction
 *   inter.c      motion-vector prediction + luma/chroma sub-pel interpolation
 *   deblock.c    the in-loop deblocking filter
 *   h264.c       decoder lifetime, DPB, POC, output reordering, public API
 */
#ifndef OWOS_H264_PRIV_H
#define OWOS_H264_PRIV_H

#include "bits.h"

/* Edge padding around each reference plane so sub-pel motion compensation can
 * read a few samples outside the picture without a bounds test in the inner loop
 * (the planes are edge-extended after each frame is reconstructed). */
#define H264_BORDER 32

/* Normalised macroblock prediction category (independent of the coded mb_type). */
enum {
	MB_I_4x4 = 0,
	MB_I_16x16,
	MB_I_PCM,
	MB_P_16x16,
	MB_P_16x8,
	MB_P_8x16,
	MB_P_8x8,
	MB_SKIP,
};

/* Per-macroblock decoded state, kept for the whole picture so neighbours can be
 * consulted for prediction, CAVLC nC derivation and deblocking. Indices into the
 * 4x4 arrays are raster within the MB: idx = (y4 * 4) + x4, x4,y4 in 0..3. */
typedef struct {
	int           slice_id;      /* which slice decoded it (-1 = not yet)        */
	unsigned char cat;           /* MB_* category                               */
	unsigned char intra;         /* 1 if an intra MB                            */
	unsigned char i16mode;       /* Intra16x16 prediction mode (0..3)           */
	unsigned char chroma_mode;   /* intra chroma prediction mode (0..3)         */
	unsigned char cbp;           /* coded_block_pattern (luma low nibble)       */
	signed char   qpy;           /* luma QP used for this MB                    */
	signed char   i4mode[16];    /* per-4x4 intra mode (0..8), -1 if inter/none */
	unsigned char nnz_l[16];     /* per-4x4 luma TotalCoeff                     */
	unsigned char nnz_cb[4];     /* per-4x4 Cb TotalCoeff (2x2 grid)            */
	unsigned char nnz_cr[4];     /* per-4x4 Cr TotalCoeff                       */
	short         mv[16][2];     /* per-4x4 list-0 motion vector, quarter-pel   */
	signed char   refidx[16];    /* per-4x4 list-0 reference index (-1 = intra) */
	unsigned char field;         /* always 0 (frame-only decoding)              */
} h264_mb;

/* A decoded picture in the frame store / DPB. The plane pointers address sample
 * (0,0); H264_BORDER samples of edge-extended padding surround each plane. */
typedef struct {
	unsigned char *base;         /* single allocation backing all three planes  */
	unsigned char *y, *cb, *cr;  /* -> (0,0) of each plane                       */
	int  ystride, cstride;
	int  poc;                    /* picture order count (display order key)      */
	int  frame_num;
	int  slice_type;             /* type of the picture's first slice           */
	int  is_ref;                 /* held as a short-term reference               */
	int  needed_output;          /* decoded, not yet returned to the caller      */
	int  used;                   /* slot occupied                                */
} h264_pic;

/* Sequence parameter set (only the fields the baseline decoder consumes). */
typedef struct {
	int valid;
	int profile_idc, level_idc;
	int log2_max_frame_num;
	int poc_type;
	int log2_max_poc_lsb;
	int delta_poc_always_zero;
	int offset_for_non_ref_pic, offset_for_top_to_bottom;
	int num_ref_frames_in_poc_cycle;
	int offset_for_ref_frame[256];
	int max_num_ref_frames;
	int mb_w, mb_h;              /* picture width/height in macroblocks          */
	int frame_mbs_only;
	int direct_8x8_inference;
	int crop_l, crop_r, crop_t, crop_b;   /* in samples (already scaled)          */
} h264_sps;

/* Picture parameter set. */
typedef struct {
	int valid;
	int sps_id;
	int entropy_cabac;           /* 1 = CABAC (unsupported)                      */
	int bottom_field_poc_present;
	int num_slice_groups;        /* >1 unsupported                              */
	int num_ref_idx_l0_default, num_ref_idx_l1_default;
	int weighted_pred;
	int weighted_bipred_idc;
	int pic_init_qp;
	int chroma_qp_index_offset;
	int second_chroma_qp_index_offset;
	int deblock_control_present;
	int constrained_intra_pred;
	int redundant_pic_cnt_present;
	int transform_8x8;           /* 1 = 8x8 transform allowed (unsupported)      */
} h264_pps;

/* Parsed slice header + a little running state for the current slice. */
typedef struct {
	int first_mb;
	int slice_type;              /* mod-5 normalised (H264_SLICE_*)              */
	int pps_id;
	int frame_num;
	int idr;
	int idr_pic_id;
	int poc_lsb;
	int delta_poc_bottom;
	int delta_poc[2];
	int num_ref_idx_l0;
	int qp;                      /* SliceQPY                                     */
	int disable_deblock;         /* disable_deblocking_filter_idc                */
	int slice_alpha_off, slice_beta_off;
	int nal_ref_idc;
} h264_slice;

struct h264_decoder {
	/* Annex-B front end: the whole elementary stream. */
	const unsigned char *buf;
	unsigned             len;
	unsigned             scan;   /* byte cursor for the Annex-B NAL iterator     */

	/* A scratch RBSP buffer reused for every NAL (grows as needed). */
	unsigned char *rbsp;
	unsigned       rbsp_cap;

	int   annexb;                /* 1 = Annex-B stream, 0 = avcC/AU front end     */
	int   nal_length_size;       /* avcC front end: NAL length prefix width       */

	h264_sps sps[32];
	h264_pps pps[256];
	h264_sps *cur_sps;
	h264_pps *cur_pps;

	int mb_w, mb_h;
	int width, height;           /* cropped display dimensions                    */
	int ystride, cstride;
	int yw, yh, cw, ch;          /* coded plane dims (mb_*·16 / ·8)               */

	/* Frame store / decoded picture buffer. */
	h264_pic *dpb;               /* array of DPB_SIZE slots                        */
	int       dpb_size;
	h264_pic *cur;               /* the picture being decoded                     */
	h264_pic *out;               /* the picture the accessors expose              */
	int       out_type;

	/* Per-picture macroblock info (mb_w * mb_h). */
	h264_mb *mbs;

	/* Reference list 0 for the current slice (pointers into dpb). */
	h264_pic *ref0[32];
	int       nref0;

	/* POC state (type 0). */
	int prev_poc_msb, prev_poc_lsb;
	int prev_frame_num, prev_frame_num_offset;

	int have_pic;                /* a picture is open (cur set)                   */
	int cur_frame_num;
	int cur_poc;
	int cur_is_idr;
	int cur_slice_type;
	int eos;

	/* Current-slice working state. */
	h264_slice sh;
	int   slice_count;           /* slices decoded into the current picture        */
	int   cur_slice_id;          /* id stamped on this slice's macroblocks          */
	int   cur_is_ref;            /* will the current picture become a reference?     */
	bitr  br;
};

/* ---- cross-file entry points --------------------------------------------- */

/* nal.c */
int  h264_parse_sps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len);
int  h264_parse_pps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len);
unsigned h264_rbsp(struct h264_decoder *d, const unsigned char *nal, unsigned nlen);

/* slice.c: decode one slice NAL. Parses the header, closes/opens a picture as
 * needed (via the h264.c helpers below), decodes the macroblock layer, and sets
 * *emitted to 1 if a finished picture became ready in d->out. */
int  h264_decode_slice_dispatch(struct h264_decoder *d, const unsigned char *rbsp,
                                unsigned len, int nal_type, int nal_ref_idc, int *emitted);

/* h264.c: picture/DPB helpers called from slice.c. */
int  h264_ensure_geometry(struct h264_decoder *d, h264_pps *pps);
int  h264_close_open_picture(struct h264_decoder *d);
int  h264_start_picture(struct h264_decoder *d, const h264_slice *sh, int nal_ref_idc);
void h264_build_ref_list0(struct h264_decoder *d);

/* cavlc.c: decode one residual block, filling coeff[0..max-1] with the levels in
 * scan order (scan position 0 = lowest frequency). `max` is maxNumCoeff (16, 15
 * or 4). `nC` selects the coeff_token table (-1 = chroma DC). Returns TotalCoeff,
 * or -1 on error. */
int  h264_cavlc_block(bitr *b, int nC, int max, int *coeff /*[max]*/);

/* transform.c */
void h264_idct4x4_add(const int blk[16], unsigned char *dst, int stride);
void h264_hadamard4x4(int dc[16]);                /* luma DC transform in place    */
void h264_chroma_dc_transform(int dc[4]);         /* 2x2 chroma DC (in place)      */
void h264_dequant_4x4(int blk[16], int qp, int is_dc_removed);
void h264_dequant_luma_dc(int dc[16], int qp);
void h264_dequant_chroma_dc(int dc[4], int qpc);

/* intra.c */
void h264_intra4x4(struct h264_decoder *d, int mbx, int mby, int blk, int mode);
void h264_intra16x16(struct h264_decoder *d, int mbx, int mby, int mode);
void h264_intra_chroma(struct h264_decoder *d, int mbx, int mby, int mode);

/* inter.c */
void h264_pred_mv(struct h264_decoder *d, int mbx, int mby, int bx4, int by4,
                  int pw, int ph, int refidx, int *mvx, int *mvy);
void h264_skip_mv(struct h264_decoder *d, int mbx, int mby, int *mvx, int *mvy);
void h264_inter_pred(struct h264_decoder *d, int mbx, int mby,
                     int bx, int by, int bw, int bh,
                     h264_pic *ref, int mvx, int mvy);

/* deblock.c */
void h264_deblock_picture(struct h264_decoder *d);

/* h264.c helpers used by slice.c */
h264_mb *h264_mb_at(struct h264_decoder *d, int mbx, int mby);   /* NULL if OOB   */
int      h264_mb_avail(struct h264_decoder *d, int cur_mbaddr, int mbx, int mby);
int      h264_chroma_qp(int qpy, int offset);

/* The luma 4x4 block scan: block index (0..15) -> pixel offset within the MB. */
extern const unsigned char h264_blk_x[16];
extern const unsigned char h264_blk_y[16];

#endif /* OWOS_H264_PRIV_H */
