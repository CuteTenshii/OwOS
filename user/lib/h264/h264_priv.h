/* h264_priv.h — shared internal state for the H.264 decoder (see h264.h).
 *
 * The decoder is split across files that all operate on `struct h264_decoder`:
 *   nal.c        NAL extraction + RBSP unescape + SPS/PPS parsing
 *   slice.c      slice header + the macroblock layer loop (per-MB syntax)
 *   cavlc.c      CAVLC residual block decoding
 *   cabac.c      CABAC engine, context init, syntax-element binarisations
 *   transform.c  dequant + 4x4/8x8 inverse transforms, DC Hadamards
 *   intra.c      intra 4x4 / 8x8 / 16x16 / chroma prediction
 *   inter.c      motion-vector prediction, direct modes, motion compensation
 *   deblock.c    the in-loop deblocking filter
 *   h264.c       decoder lifetime, POC, reference marking and lists, DPB
 *                output ordering, public API
 */
#ifndef OWOS_H264_PRIV_H
#define OWOS_H264_PRIV_H

#include "bits.h"

/* Edge padding around each reference plane so sub-pel motion compensation can
 * read a few samples outside the picture without a bounds test in the inner
 * loop (the planes are edge-extended after each frame is reconstructed). */
#define H264_BORDER 32
#define H264_MAX_REFS 32

/* Normalised macroblock prediction category (independent of the coded mb_type).
 */
enum {
  MB_I_4x4 = 0,
  MB_I_8x8,
  MB_I_16x16,
  MB_I_PCM,
  MB_16x16,
  MB_16x8,
  MB_8x16,
  MB_8x8,
  MB_DIRECT, /* B_Direct_16x16 */
  MB_SKIP,   /* P_Skip / B_Skip */
};

/* Per-macroblock decoded state, kept for the whole picture so neighbours can be
 * consulted for prediction, CAVLC nC derivation and deblocking. Indices into
 * the 4x4 arrays are raster within the MB: idx = (y4 * 4) + x4, x4,y4 in 0..3.
 */
typedef struct {
  int slice_id;                    /* which slice decoded it (-1 = not yet)        */
  unsigned char cat;               /* MB_* category                               */
  unsigned char intra;             /* 1 if an intra MB                            */
  unsigned char i16mode;           /* Intra16x16 prediction mode (0..3)           */
  unsigned char chroma_mode;       /* intra chroma prediction mode (0..3)         */
  unsigned char cbp;               /* coded_block_pattern (luma low nibble)       */
  unsigned char t8x8;              /* transform_size_8x8_flag                     */
  unsigned char direct8;           /* 8x8 quadrants predicted in direct mode      */
  signed char qpy;                 /* luma QP used for this MB                    */
  signed char i4mode[16];          /* per-4x4 intra mode (0..8); 8x8 modes replicated */
  unsigned char nnz_l[16];         /* per-4x4 luma TotalCoeff                     */
  unsigned char nnz_cb[4];         /* per-4x4 Cb TotalCoeff (2x2 grid)            */
  unsigned char nnz_cr[4];         /* per-4x4 Cr TotalCoeff                       */
  unsigned char dbk_idc;           /* disable_deblocking_filter_idc of its slice  */
  signed char alpha_off, beta_off; /* slice deblocking offsets (already doubled) */
  unsigned char cbf_dc;            /* CABAC coded_block_flag: bit0 luma DC, 1/2 Cb/Cr DC */
  unsigned char mvd[2][16][2];     /* per-list per-4x4 |mvd| (saturated), CABAC contexts */
} h264_mb;

/* Motion of one macroblock, kept per picture: the current picture's neighbours
 * predict from it, deblocking compares it, and a later B picture reads it as the
 * co-located data for direct prediction. */
typedef struct {
  short mv[2][16][2];        /* per list, per 4x4, quarter-pel               */
  signed char refidx[2][16]; /* per list, per 4x4; -1 = list unused          */
  int refpic[2][4];          /* per list, per 8x8: pic_id of the reference   */
} h264_mot;

/* A decoded picture in the frame store / DPB. The plane pointers address sample
 * (0,0); H264_BORDER samples of edge-extended padding surround each plane. */
typedef struct {
  unsigned char *base;        /* single allocation backing all three planes  */
  unsigned char *y, *cb, *cr; /* -> (0,0) of each plane                       */
  int ystride, cstride;
  h264_mot *mot;
  int poc; /* picture order count (display order key)      */
  int frame_num;
  int pic_id;       /* unique per decoded picture                    */
  int slice_type;   /* type of the picture's first slice            */
  int ref;          /* 0 unused for reference, 1 short-term, 2 long-term */
  int long_idx;     /* LongTermFrameIdx when ref == 2                */
  int out;          /* 0 not waiting, 1 awaiting output, 2 queued for the caller */
  int used;         /* slot occupied                                */
  int non_existing; /* inserted for a frame_num gap; never output    */
} h264_pic;

/* Scaling lists as parsed (zig-zag order); resolved by h264_scaling_prepare(). */
typedef struct {
  unsigned char present[8];     /* list i was transmitted                        */
  unsigned char use_default[8]; /* transmitted as "use the default list"        */
  unsigned char list[6][16];    /* 4x4 lists: Intra Y/Cb/Cr, Inter Y/Cb/Cr       */
  unsigned char list8[2][64];   /* 8x8 lists: Intra Y, Inter Y                   */
} h264_scaling;

/* Sequence parameter set. */
typedef struct {
  int valid;
  int profile_idc, level_idc, constraint_flags;
  int log2_max_frame_num;
  int poc_type;
  int log2_max_poc_lsb;
  int delta_poc_always_zero;
  int offset_for_non_ref_pic, offset_for_top_to_bottom;
  int num_ref_frames_in_poc_cycle;
  int offset_for_ref_frame[256];
  int max_num_ref_frames;
  int gaps_allowed;
  int mb_w, mb_h; /* picture width/height in macroblocks          */
  int frame_mbs_only;
  int direct_8x8_inference;
  int crop_l, crop_r, crop_t, crop_b; /* in samples (already scaled)          */
  int has_scaling;                    /* seq_scaling_matrix_present_flag */
  h264_scaling scaling;
  int num_reorder_frames;      /* VUI bitstream_restriction, -1 if absent */
  int max_dec_frame_buffering; /* likewise */
} h264_sps;

/* Picture parameter set. */
typedef struct {
  int valid;
  int sps_id;
  int entropy_cabac;
  int bottom_field_poc_present;
  int num_slice_groups; /* >1 unsupported                              */
  int num_ref_idx_default[2];
  int weighted_pred;
  int weighted_bipred_idc;
  int pic_init_qp;
  int chroma_qp_index_offset;
  int second_chroma_qp_index_offset;
  int deblock_control_present;
  int constrained_intra_pred;
  int redundant_pic_cnt_present;
  int transform_8x8;
  int has_scaling; /* pic_scaling_matrix_present_flag */
  h264_scaling scaling;
} h264_pps;

/* Parsed slice header + a little running state for the current slice. */
typedef struct {
  int first_mb;
  int slice_type; /* mod-5 normalised (H264_SLICE_*)              */
  int pps_id;
  int frame_num;
  int idr;
  int idr_pic_id;
  int poc_lsb;
  int delta_poc_bottom;
  int delta_poc[2];
  int direct_spatial;
  int num_ref_idx[2];
  int cabac_init_idc;
  int qp;              /* SliceQPY                                     */
  int disable_deblock; /* disable_deblocking_filter_idc                */
  int slice_alpha_off, slice_beta_off;
  int nal_ref_idc;

  int rm_n[2]; /* ref_pic_list_modification: operations per list */
  unsigned char rm_idc[2][H264_MAX_REFS + 1];
  int rm_val[2][H264_MAX_REFS + 1];

  int luma_log2_denom, chroma_log2_denom; /* pred_weight_table */
  short lw[2][H264_MAX_REFS], lo[2][H264_MAX_REFS];
  short cw[2][H264_MAX_REFS][2], co[2][H264_MAX_REFS][2];
  unsigned char lw_flag[2][H264_MAX_REFS], cw_flag[2][H264_MAX_REFS];

  int no_output_of_prior; /* dec_ref_pic_marking */
  int long_term_ref_flag;
  int adaptive_marking;
  int mmco_n;
  unsigned char mmco_op[68];
  int mmco_a[68], mmco_b[68];
} h264_slice;

/* CABAC arithmetic decoder (9.3): range/offset registers and one
 * (pStateIdx << 1 | valMPS) byte per context. */
typedef struct {
  bitr *br;
  unsigned range, offset;
  int dqp_nz; /* previous MB in decoding order had a non-zero mb_qp_delta */
  unsigned char st[460];
} h264_cabac;

struct h264_decoder {
  /* Annex-B front end: the whole elementary stream. */
  const unsigned char *buf;
  unsigned len;
  unsigned scan; /* byte cursor for the Annex-B NAL iterator     */

  /* A scratch RBSP buffer reused for every NAL (grows as needed). */
  unsigned char *rbsp;
  unsigned rbsp_cap;

  int annexb;          /* 1 = Annex-B stream, 0 = avcC/AU front end     */
  int nal_length_size; /* avcC front end: NAL length prefix width       */

  h264_sps sps[32];
  h264_pps pps[256];
  h264_sps *cur_sps;
  h264_pps *cur_pps;

  int mb_w, mb_h;
  int width, height; /* cropped display dimensions                    */
  int ystride, cstride;
  int yw, yh, cw, ch; /* coded plane dims (mb_*·16 / ·8)               */

  /* Frame store / decoded picture buffer. */
  h264_pic *dpb; /* array of dpb_size slots                        */
  int dpb_size;  /* slots allocated (capacity + current + spare)   */
  int dpb_cap;   /* frames the stream may keep (reorder window)    */
  h264_pic *cur; /* the picture being decoded                     */
  h264_pic *out; /* the picture the accessors expose              */
  int out_type;
  h264_pic *outq[H264_MAX_REFS + 2]; /* pictures queued for the caller, display order */
  int outq_n;
  int pic_counter;

  /* Per-picture macroblock info (mb_w * mb_h). */
  h264_mb *mbs;

  /* Reference lists for the current slice (pointers into dpb). */
  h264_pic *reflist[2][H264_MAX_REFS + 2];
  int nref[2];
  short implicit_w1[H264_MAX_REFS][H264_MAX_REFS]; /* implicit bi-pred weight of list 1 */
  int mb_done;                                     /* 4x4 blocks of the current MB whose motion is final */

  /* POC / frame_num / reference marking state. */
  int prev_poc_msb, prev_poc_lsb;
  int prev_frame_num, prev_frame_num_offset, prev_ref_frame_num;
  int cur_poc_msb, cur_top_poc, cur_bot_poc, cur_fn_offset;
  int max_long_idx; /* MaxLongTermFrameIdx, -1 = no long-term frame indices */
  int mmco5;        /* the picture just marked carried memory_management_control_operation 5 */
  int seq_started;  /* a picture of this coded video sequence has been decoded */

  int have_pic; /* a picture is open (cur set)                   */
  int cur_frame_num;
  int cur_poc;
  int cur_is_idr;
  int cur_slice_type;
  int eos;
  h264_slice pic_sh; /* first slice header of the open picture (reference marking) */

  /* Current-slice working state. */
  h264_slice sh;
  int slice_count;  /* slices decoded into the current picture        */
  int cur_slice_id; /* id stamped on this slice's macroblocks          */
  int cur_is_ref;   /* will the current picture become a reference?     */
  bitr br;
  h264_cabac cabac;

  /* Dequantisation: LevelScale = weight * normAdjust per list / qp%6 / position. */
  int ls4[6][6][16];
  int ls8[2][6][64];
};

/* ---- cross-file entry points --------------------------------------------- */

/* nal.c */
int h264_parse_sps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len);
int h264_parse_pps(struct h264_decoder *d, const unsigned char *rbsp, unsigned len);
unsigned h264_rbsp(struct h264_decoder *d, const unsigned char *nal, unsigned nlen);

/* slice.c: decode one slice NAL. Parses the header, closes/opens a picture as
 * needed (via the h264.c helpers below), decodes the macroblock layer, and sets
 * *emitted to 1 if a finished picture became ready for output. */
int h264_decode_slice_dispatch(struct h264_decoder *d, const unsigned char *rbsp, unsigned len, int nal_type,
                               int nal_ref_idc, int *emitted);

/* h264.c: picture/DPB helpers called from slice.c. */
int h264_ensure_geometry(struct h264_decoder *d, h264_pps *pps);
int h264_close_open_picture(struct h264_decoder *d);
int h264_start_picture(struct h264_decoder *d, const h264_slice *sh, int nal_ref_idc);
int h264_build_ref_lists(struct h264_decoder *d);

/* cavlc.c: decode one residual block, filling coeff[0..max-1] with the levels
 * in scan order (scan position 0 = lowest frequency). `max` is maxNumCoeff (16,
 * 15 or 4). `nC` selects the coeff_token table (-1 = chroma DC). Returns
 * TotalCoeff, or -1 on error. */
int h264_cavlc_block(bitr *b, int nC, int max, int *coeff /*[max]*/);

/* cabac.c: engine + the context-coded syntax elements (9.3). The caller derives
 * every neighbour-dependent ctxIdxInc; the *_start calls consume the 9-bit
 * offset at the current bit position. */
void h264_cabac_init(struct h264_decoder *d);
void h264_cabac_start(h264_cabac *c);
int h264_cabac_decision(h264_cabac *c, int ctx);
int h264_cabac_terminate(h264_cabac *c);
int h264_cabac_mb_type_i(h264_cabac *c, int inc); /* I slice: 0..25 */
int h264_cabac_mb_type_p(h264_cabac *c);          /* P slice: 0..3, or 5 + intra type */
int h264_cabac_mb_type_b(h264_cabac *c, int inc); /* B slice: 0..22, or 23 + intra type */
int h264_cabac_sub_mb_type(h264_cabac *c);        /* P: 0..3 */
int h264_cabac_sub_mb_type_b(h264_cabac *c);      /* B: 0..12 */
int h264_cabac_ref_idx(h264_cabac *c, int inc);
int h264_cabac_mvd(h264_cabac *c, int comp, int sum); /* sum = |mvdA| + |mvdB| */
int h264_cabac_qp_delta(h264_cabac *c);
int h264_cabac_chroma_pred_mode(h264_cabac *c, int inc);
int h264_cabac_rem_intra4x4(h264_cabac *c);
/* cat: 0 Intra16x16 DC, 1 Intra16x16 AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC, 5 luma 8x8 */
int h264_cabac_residual(h264_cabac *c, int cat, int cbf_inc, int max, int *coeff);

/* transform.c: `ls` is the LevelScale row for the block's qP%6 (raster order). */
void h264_idct4x4_add(const int blk[16], unsigned char *dst, int stride);
void h264_idct8x8_add(const int blk[64], unsigned char *dst, int stride);
void h264_hadamard4x4(int dc[16]);        /* luma DC transform in place    */
void h264_chroma_dc_transform(int dc[4]); /* 2x2 chroma DC (in place)      */
void h264_dequant_4x4(int blk[16], int qp, const int *ls, int dc_removed);
void h264_dequant_8x8(int blk[64], int qp, const int *ls);
void h264_dequant_luma_dc(int dc[16], int qp, int ls00);
void h264_dequant_chroma_dc(int dc[4], int qpc, int ls00);
void h264_scaling_prepare(struct h264_decoder *d);
const unsigned char *h264_zigzag8(void); /* 8x8 frame scan -> raster */

/* intra.c */
void h264_intra4x4(struct h264_decoder *d, int mbx, int mby, int blk, int mode);
void h264_intra8x8(struct h264_decoder *d, int mbx, int mby, int blk8, int mode);
void h264_intra16x16(struct h264_decoder *d, int mbx, int mby, int mode);
void h264_intra_chroma(struct h264_decoder *d, int mbx, int mby, int mode);

/* inter.c */
void h264_pred_mv(struct h264_decoder *d, int mbx, int mby, int list, int bx4, int by4, int pw, int ph, int refidx,
                  int *mvx, int *mvy);
void h264_skip_mv(struct h264_decoder *d, int mbx, int mby, int *mvx, int *mvy);
void h264_direct_motion(struct h264_decoder *d, int mbx, int mby, int quad_mask);
void h264_store_motion(struct h264_decoder *d, int mbx, int mby, int list, int bx4, int by4, int bw4, int bh4,
                       int refidx, int mvx, int mvy);
void h264_predict_mb(struct h264_decoder *d, int mbx, int mby);
void h264_slice_weights(struct h264_decoder *d);
void h264_mb_refpics(struct h264_decoder *d, int mbx,
                     int mby); /* record reference picture ids once the motion is final */

/* deblock.c */
void h264_deblock_picture(struct h264_decoder *d);

/* h264.c helpers used by slice.c */
h264_mb *h264_mb_at(struct h264_decoder *d, int mbx, int mby); /* NULL if OOB */
int h264_mb_avail(struct h264_decoder *d, int cur_mbaddr, int mbx, int mby);
int h264_chroma_qp(int qpy, int offset);

/* The luma 4x4 block scan: block index (0..15) -> pixel offset within the MB.
 */
extern const unsigned char h264_blk_x[16];
extern const unsigned char h264_blk_y[16];

#endif /* OWOS_H264_PRIV_H */
