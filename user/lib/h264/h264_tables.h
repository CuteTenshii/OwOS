/* h264_tables.h — the constant tables for H.264 CAVLC entropy decoding, the 4x4
 * scan, and dequantisation. The CAVLC VLC tables (coeff_token, total_zeros,
 * run_before) are hand-designed prefix codes from the standard; these are copied
 * verbatim from FFmpeg's libavcodec/h264_cavlc.c so the transcription is exact.
 *
 * coeff_token_{len,bits}[group][TotalCoeff*4 + TrailingOnes]: `group` selects the
 * table by nC (0: 0<=nC<2, 1: 2<=nC<4, 2: 4<=nC<8; nC>=8 is a fixed 6-bit code
 * handled in code; nC==-1 chroma DC uses the chroma_dc_coeff_token_* tables). A
 * len of 0 marks an absent entry.
 */
#ifndef OWOS_H264_TABLES_H
#define OWOS_H264_TABLES_H

/* 4x4 zig-zag scan: scan index -> raster position (row*4 + col) inside a 4x4. */
static const unsigned char h264_zigzag4[16] =
	{ 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

static const unsigned char h264_coeff_token_len[3][4*17] = {
{
	 1, 0, 0, 0,
	 6, 2, 0, 0,     8, 6, 3, 0,     9, 8, 7, 5,    10, 9, 8, 6,
	11,10, 9, 7,    13,11,10, 8,    13,13,11, 9,    13,13,13,10,
	14,14,13,11,    14,14,14,13,    15,15,14,14,    15,15,15,14,
	16,15,15,15,    16,16,16,15,    16,16,16,16,    16,16,16,16,
},
{
	 2, 0, 0, 0,
	 6, 2, 0, 0,     6, 5, 3, 0,     7, 6, 6, 4,     8, 6, 6, 4,
	 8, 7, 7, 5,     9, 8, 8, 6,    11, 9, 9, 6,    11,11,11, 7,
	12,11,11, 9,    12,12,12,11,    12,12,12,11,    13,13,13,12,
	13,13,13,13,    13,14,13,13,    14,14,14,13,    14,14,14,14,
},
{
	 4, 0, 0, 0,
	 6, 4, 0, 0,     6, 5, 4, 0,     6, 5, 5, 4,     7, 5, 5, 4,
	 7, 5, 5, 4,     7, 6, 6, 4,     7, 6, 6, 4,     8, 7, 7, 5,
	 8, 8, 7, 6,     9, 8, 8, 7,     9, 9, 8, 8,     9, 9, 9, 8,
	10, 9, 9, 9,    10,10,10,10,    10,10,10,10,    10,10,10,10,
},
};

static const unsigned char h264_coeff_token_bits[3][4*17] = {
{
	 1, 0, 0, 0,
	 5, 1, 0, 0,     7, 4, 1, 0,     7, 6, 5, 3,     7, 6, 5, 3,
	 7, 6, 5, 4,    15, 6, 5, 4,    11,14, 5, 4,     8,10,13, 4,
	15,14, 9, 4,    11,10,13,12,    15,14, 9,12,    11,10,13, 8,
	15, 1, 9,12,    11,14,13, 8,     7,10, 9,12,     4, 6, 5, 8,
},
{
	 3, 0, 0, 0,
	11, 2, 0, 0,     7, 7, 3, 0,     7,10, 9, 5,     7, 6, 5, 4,
	 4, 6, 5, 6,     7, 6, 5, 8,    15, 6, 5, 4,    11,14,13, 4,
	15,10, 9, 4,    11,14,13,12,     8,10, 9, 8,    15,14,13,12,
	11,10, 9,12,     7,11, 6, 8,     9, 8,10, 1,     7, 6, 5, 4,
},
{
	15, 0, 0, 0,
	15,14, 0, 0,    11,15,13, 0,     8,12,14,12,    15,10,11,11,
	11, 8, 9,10,     9,14,13, 9,     8,10, 9, 8,    15,14,13,13,
	11,14,10,12,    15,10,13,12,    11,14, 9,12,     8,10,13, 8,
	13, 7, 9,12,     9,12,11,10,     5, 8, 7, 6,     1, 4, 3, 2,
},
};

/* Chroma DC (4:2:0), nC == -1. Indexed [TotalCoeff*4 + TrailingOnes]. */
static const unsigned char h264_chroma_dc_ct_len[4*5] = {
	2, 0, 0, 0,   6, 1, 0, 0,   6, 6, 3, 0,   6, 7, 7, 6,   6, 8, 8, 7,
};
static const unsigned char h264_chroma_dc_ct_bits[4*5] = {
	1, 0, 0, 0,   7, 1, 0, 0,   4, 6, 1, 0,   3, 3, 2, 5,   2, 3, 2, 0,
};

/* total_zeros for 4x4 blocks. Indexed [TotalCoeff-1][total_zeros]; len 0 absent. */
static const unsigned char h264_total_zeros_len[15][16] = {
	{1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},
	{3,3,3,3,3,4,4,4,4,5,5,6,6,6,6},
	{4,3,3,3,4,4,3,3,4,5,5,6,5,6},
	{5,3,4,4,3,3,3,4,3,4,5,5,5},
	{4,4,4,3,3,3,3,3,4,5,4,5},
	{6,5,3,3,3,3,3,3,4,3,6},
	{6,5,3,3,3,2,3,4,3,6},
	{6,4,5,3,2,2,3,3,6},
	{6,6,4,2,2,3,2,5},
	{5,5,3,2,2,2,4},
	{4,4,3,3,1,3},
	{4,4,2,1,3},
	{3,3,1,2},
	{2,2,1},
	{1,1},
};
static const unsigned char h264_total_zeros_bits[15][16] = {
	{1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},
	{7,6,5,4,3,5,4,3,2,3,2,3,2,1,0},
	{5,7,6,5,4,3,4,3,2,3,2,1,1,0},
	{3,7,5,4,6,5,4,3,3,2,2,1,0},
	{5,4,3,7,6,5,4,3,2,1,1,0},
	{1,1,7,6,5,4,3,2,1,1,0},
	{1,1,5,4,3,3,2,1,1,0},
	{1,1,1,3,3,2,2,1,0},
	{1,0,1,3,2,1,1,1},
	{1,0,1,3,2,1,1},
	{0,1,1,2,1,3},
	{0,1,1,1,1},
	{0,1,1,1},
	{0,1,1},
	{0,1},
};

/* total_zeros for the chroma DC 2x2 block. Indexed [TotalCoeff-1][total_zeros]. */
static const unsigned char h264_chroma_dc_tz_len[3][4] = {
	{1,2,3,3}, {1,2,2,0}, {1,1,0,0},
};
static const unsigned char h264_chroma_dc_tz_bits[3][4] = {
	{1,1,1,0}, {1,1,0,0}, {1,0,0,0},
};

/* run_before. Indexed [min(zerosLeft,7)-1][run_before]; len 0 absent. */
static const unsigned char h264_run_len[7][16] = {
	{1,1},
	{1,2,2},
	{2,2,2,2},
	{2,2,2,3,3},
	{2,2,3,3,3,3},
	{2,3,3,3,3,3,3},
	{3,3,3,3,3,3,3,4,5,6,7,8,9,10,11},
};
static const unsigned char h264_run_bits[7][16] = {
	{1,0},
	{1,1,0},
	{3,2,1,0},
	{3,2,1,1,0},
	{3,2,3,2,1,0},
	{3,0,1,3,2,5,4},
	{7,6,5,4,3,2,1,1,1,1,1,1,1,1,1},
};

/* Dequant: normAdjust position class per raster position, and the six qP%6 rows
 * of {class0,class1,class2} factors (H.264 8.5.9). LevelScale = 16*normAdjust for
 * flat scaling (weightScale 16). */
static const unsigned char h264_dequant_class[16] =
	{ 0,2,0,2,  2,1,2,1,  0,2,0,2,  2,1,2,1 };
static const unsigned char h264_normadjust[6][3] = {
	{10,16,13}, {11,18,14}, {13,20,16}, {14,23,18}, {16,25,20}, {18,29,23},
};

/* Table 8-15: chroma qP mapping for qPI in 30..51 (qPI<30 maps to itself). */
static const unsigned char h264_chroma_qp_map[22] = {
	29,30,31,32,32,33,34,34,35,35,36,36,37,37,38,38,39,39,39,39,39,39,
};

#endif /* OWOS_H264_TABLES_H */
