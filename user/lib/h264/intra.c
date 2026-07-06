/* intra.c — intra prediction (H.264 8.3): Intra_4x4, Intra_16x16 and chroma.
 *
 * Prediction writes the predicted samples straight into the current picture
 * plane at the block's position; the residual is then added on top by the inverse
 * transform. Reference samples are the reconstructed neighbours already present in
 * the plane (left column, top row, the corner, and — for 4x4 — the top-right).
 * Neighbour availability follows the same-slice / constrained-intra rules; the
 * per-4x4 top-right has the well-known "unavailable for blocks 3,7,11,13,15"
 * behaviour and is substituted from the last top sample.
 */
#include "owos.h"
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"

static inline int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Is MB (nmbx,nmby) usable as an intra prediction neighbour of cur_mbaddr? */
static int nbr_intra(struct h264_decoder *d, int cur_mbaddr, int nmbx, int nmby)
{
	int a;
	if (nmbx < 0 || nmby < 0 || nmbx >= d->mb_w || nmby >= d->mb_h) return 0;
	a = nmby * d->mb_w + nmbx;
	if (a >= cur_mbaddr) return 0;
	if (d->mbs[a].slice_id != d->mbs[cur_mbaddr].slice_id) return 0;
	if (d->cur_pps->constrained_intra_pred && !d->mbs[a].intra) return 0;
	return 1;
}

/* ---- Intra_4x4 ------------------------------------------------------------ */
/* Top-right source per luma-4x4 block index: 0 within-MB (always ready), 1 from
 * the top MB, 2 from the top-right MB, 3 unavailable (substitute). */
static const unsigned char tr_src[16] =
	{ 1,1,0,3,  1,2,0,3,  0,0,0,3,  0,3,0,3 };

void h264_intra4x4(struct h264_decoder *d, int mbx, int mby, int blk, int mode)
{
	unsigned char *Y = d->cur->y;
	int stride = d->ystride;
	int bx = h264_blk_x[blk], by = h264_blk_y[blk];
	int x0 = mbx * 16 + bx, y0 = mby * 16 + by;
	int cur = mby * d->mb_w + mbx;
	int aleft, atop, atl, atr, i, x, y;
	int t[8], l[4], tl;
	int T[9], L[5];                          /* T[0]=t[-1]=tl, L[0]=l[-1]=tl       */
	unsigned char *dst = Y + y0 * stride + x0;

	aleft = (bx > 0) ? 1 : nbr_intra(d, cur, mbx - 1, mby);
	atop  = (by > 0) ? 1 : nbr_intra(d, cur, mbx, mby - 1);
	if (bx > 0 && by > 0)      atl = 1;
	else if (bx == 0 && by > 0) atl = nbr_intra(d, cur, mbx - 1, mby);
	else if (bx > 0 && by == 0) atl = nbr_intra(d, cur, mbx, mby - 1);
	else                        atl = nbr_intra(d, cur, mbx - 1, mby - 1);
	switch (tr_src[blk]) {
	case 0:  atr = atop; break;                              /* within current MB  */
	case 1:  atr = nbr_intra(d, cur, mbx, mby - 1); break;   /* top MB             */
	case 2:  atr = nbr_intra(d, cur, mbx + 1, mby - 1); break;
	default: atr = 0; break;
	}

	tl = atl ? Y[(y0 - 1) * stride + (x0 - 1)] : 128;
	for (i = 0; i < 4; i++) l[i] = aleft ? Y[(y0 + i) * stride + (x0 - 1)] : 128;
	for (i = 0; i < 4; i++) t[i] = atop ? Y[(y0 - 1) * stride + (x0 + i)] : 128;
	if (atr)       for (i = 0; i < 4; i++) t[4 + i] = Y[(y0 - 1) * stride + (x0 + 4 + i)];
	else if (atop) for (i = 0; i < 4; i++) t[4 + i] = t[3];
	else           for (i = 0; i < 4; i++) t[4 + i] = 128;

	T[0] = tl; for (i = 0; i < 8; i++) T[i + 1] = t[i];
	L[0] = tl; for (i = 0; i < 4; i++) L[i + 1] = l[i];
	#define TT(k) (T[(k) + 1])
	#define LL(k) (L[(k) + 1])

	for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
		int p = 128;
		switch (mode) {
		case 0: p = t[x]; break;                             /* Vertical           */
		case 1: p = l[y]; break;                             /* Horizontal         */
		case 2: {                                            /* DC                 */
			int s = 0, n = 0;
			if (atop)  { s += t[0]+t[1]+t[2]+t[3]; n += 4; }
			if (aleft) { s += l[0]+l[1]+l[2]+l[3]; n += 4; }
			p = n ? (s + (n >> 1)) / n : 128;
			break; }
		case 3:                                              /* Diagonal Down-Left */
			p = (x == 3 && y == 3) ? (t[6] + 3*t[7] + 2) >> 2
			                       : (t[x+y] + 2*t[x+y+1] + t[x+y+2] + 2) >> 2;
			break;
		case 4:                                              /* Diagonal Down-Right*/
			if (x > y)      p = (TT(x-y-2) + 2*TT(x-y-1) + TT(x-y) + 2) >> 2;
			else if (x < y) p = (LL(y-x-2) + 2*LL(y-x-1) + LL(y-x) + 2) >> 2;
			else            p = (t[0] + 2*tl + l[0] + 2) >> 2;
			break;
		case 5: {                                            /* Vertical-Right     */
			int z = 2*x - y;
			if (z >= 0 && (z & 1) == 0)  p = (TT(x-(y>>1)-1) + TT(x-(y>>1)) + 1) >> 1;
			else if (z >= 0)             p = (TT(x-(y>>1)-2) + 2*TT(x-(y>>1)-1) + TT(x-(y>>1)) + 2) >> 2;
			else if (z == -1)            p = (l[0] + 2*tl + t[0] + 2) >> 2;
			else                         p = (LL(y-1) + 2*LL(y-2) + LL(y-3) + 2) >> 2;
			break; }
		case 6: {                                            /* Horizontal-Down    */
			int z = 2*y - x;
			if (z >= 0 && (z & 1) == 0)  p = (LL(y-(x>>1)-1) + LL(y-(x>>1)) + 1) >> 1;
			else if (z >= 0)             p = (LL(y-(x>>1)-2) + 2*LL(y-(x>>1)-1) + LL(y-(x>>1)) + 2) >> 2;
			else if (z == -1)            p = (t[0] + 2*tl + l[0] + 2) >> 2;
			else                         p = (TT(x-1) + 2*TT(x-2) + TT(x-3) + 2) >> 2;
			break; }
		case 7:                                              /* Vertical-Left      */
			if ((y & 1) == 0) p = (t[x+(y>>1)] + t[x+(y>>1)+1] + 1) >> 1;
			else              p = (t[x+(y>>1)] + 2*t[x+(y>>1)+1] + t[x+(y>>1)+2] + 2) >> 2;
			break;
		case 8: {                                            /* Horizontal-Up      */
			int z = x + 2*y;
			if (z < 5 && (z & 1) == 0) p = (l[y+(x>>1)] + l[y+(x>>1)+1] + 1) >> 1;
			else if (z < 5)            p = (l[y+(x>>1)] + 2*l[y+(x>>1)+1] + l[y+(x>>1)+2] + 2) >> 2;
			else if (z == 5)           p = (l[2] + 3*l[3] + 2) >> 2;
			else                       p = l[3];
			break; }
		}
		dst[y * stride + x] = (unsigned char)clip8(p);
	}
	#undef TT
	#undef LL
}

/* ---- Intra_16x16 ---------------------------------------------------------- */
void h264_intra16x16(struct h264_decoder *d, int mbx, int mby, int mode)
{
	unsigned char *Y = d->cur->y;
	int stride = d->ystride, x0 = mbx * 16, y0 = mby * 16, cur = mby * d->mb_w + mbx;
	int aleft = nbr_intra(d, cur, mbx - 1, mby);
	int atop  = nbr_intra(d, cur, mbx, mby - 1);
	int atl   = nbr_intra(d, cur, mbx - 1, mby - 1);
	int t[16], l[16], tl, x, y;

	tl = atl ? Y[(y0 - 1) * stride + (x0 - 1)] : 128;
	for (x = 0; x < 16; x++) t[x] = atop  ? Y[(y0 - 1) * stride + (x0 + x)] : 128;
	for (y = 0; y < 16; y++) l[y] = aleft ? Y[(y0 + y) * stride + (x0 - 1)] : 128;

	if (mode == 3) {                                         /* Plane              */
		int H = 0, V = 0, a, b, c, i;
		for (i = 0; i < 8; i++)
			H += (i + 1) * ((i == 7 ? t[15] : t[8 + i]) - (i == 7 ? tl : t[6 - i]));
		for (i = 0; i < 8; i++)
			V += (i + 1) * ((i == 7 ? l[15] : l[8 + i]) - (i == 7 ? tl : l[6 - i]));
		a = 16 * (l[15] + t[15]);
		b = (5 * H + 32) >> 6;
		c = (5 * V + 32) >> 6;
		for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
			Y[(y0 + y) * stride + x0 + x] =
				(unsigned char)clip8((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
		return;
	}

	if (mode == 0) {                                         /* Vertical           */
		for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
			Y[(y0 + y) * stride + x0 + x] = (unsigned char)t[x];
	} else if (mode == 1) {                                  /* Horizontal         */
		for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
			Y[(y0 + y) * stride + x0 + x] = (unsigned char)l[y];
	} else {                                                 /* DC                 */
		int s = 0, n = 0, dc, i;
		if (atop)  { for (i = 0; i < 16; i++) s += t[i]; n += 16; }
		if (aleft) { for (i = 0; i < 16; i++) s += l[i]; n += 16; }
		dc = n ? (s + (n >> 1)) / n : 128;
		for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
			Y[(y0 + y) * stride + x0 + x] = (unsigned char)dc;
	}
}

/* ---- chroma (8x8, 4:2:0) -------------------------------------------------- */
static void chroma_plane(struct h264_decoder *d, unsigned char *C, int stride,
                         int mbx, int mby, int mode, int aleft, int atop, int atl)
{
	int x0 = mbx * 8, y0 = mby * 8, x, y;
	int t[8], l[8], tl;

	tl = atl ? C[(y0 - 1) * stride + (x0 - 1)] : 128;
	for (x = 0; x < 8; x++) t[x] = atop  ? C[(y0 - 1) * stride + (x0 + x)] : 128;
	for (y = 0; y < 8; y++) l[y] = aleft ? C[(y0 + y) * stride + (x0 - 1)] : 128;

	if (mode == 1) {                                         /* Horizontal         */
		for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
			C[(y0 + y) * stride + x0 + x] = (unsigned char)l[y];
	} else if (mode == 2) {                                  /* Vertical           */
		for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
			C[(y0 + y) * stride + x0 + x] = (unsigned char)t[x];
	} else if (mode == 3) {                                  /* Plane              */
		int H = 0, V = 0, a, b, c, i;
		for (i = 0; i < 4; i++)
			H += (i + 1) * ((i == 3 ? t[7] : t[4 + i]) - (i == 3 ? tl : t[2 - i]));
		for (i = 0; i < 4; i++)
			V += (i + 1) * ((i == 3 ? l[7] : l[4 + i]) - (i == 3 ? tl : l[2 - i]));
		a = 16 * (l[7] + t[7]);
		b = (17 * H + 16) >> 5;
		c = (17 * V + 16) >> 5;
		for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
			C[(y0 + y) * stride + x0 + x] =
				(unsigned char)clip8((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
	} else {                                                 /* DC (per 4x4)       */
		int bx, by;
		for (by = 0; by < 8; by += 4) for (bx = 0; bx < 8; bx += 4) {
			int st = 0, sl = 0, i, dc, use_t, use_l;
			for (i = 0; i < 4; i++) { st += t[bx + i]; sl += l[by + i]; }
			/* Which neighbours this quadrant prefers (8.3.4). */
			if ((bx == 0 && by == 0) || (bx > 0 && by > 0)) { use_t = atop; use_l = aleft; }
			else if (bx > 0 && by == 0)                      { use_t = atop; use_l = 0; if (!atop && aleft) use_l = 1; }
			else                                             { use_l = aleft; use_t = 0; if (!aleft && atop) use_t = 1; }
			if (use_t && use_l) dc = (st + sl + 4) >> 3;
			else if (use_l)     dc = (sl + 2) >> 2;
			else if (use_t)     dc = (st + 2) >> 2;
			else                dc = 128;
			for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
				C[(y0 + by + y) * stride + x0 + bx + x] = (unsigned char)dc;
		}
	}
}

void h264_intra_chroma(struct h264_decoder *d, int mbx, int mby, int mode)
{
	int cur = mby * d->mb_w + mbx;
	int aleft = nbr_intra(d, cur, mbx - 1, mby);
	int atop  = nbr_intra(d, cur, mbx, mby - 1);
	int atl   = nbr_intra(d, cur, mbx - 1, mby - 1);
	chroma_plane(d, d->cur->cb, d->cstride, mbx, mby, mode, aleft, atop, atl);
	chroma_plane(d, d->cur->cr, d->cstride, mbx, mby, mode, aleft, atop, atl);
}
