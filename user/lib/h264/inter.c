/* inter.c — P-slice inter prediction (H.264 8.4): motion-vector prediction and
 * fractional-sample interpolation.
 *
 * Luma uses the separable 6-tap half-pel filter (1,-5,20,20,-5,1) with quarter-pel
 * positions formed by averaging the two nearest half/full samples; chroma uses
 * bilinear 1/8-pel interpolation. Reference planes are edge-extended (H264_BORDER)
 * so the taps and clamped fetches stay in bounds. Motion vectors are predicted by
 * the median of the left/top/top-right neighbours, with the directional special
 * cases for 16x8 / 8x16 partitions and the P_Skip zero-vector rule.
 */
#include "owos.h"
#include "h264.h"
#include "h264_priv.h"

static int clampc(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static int gp(const unsigned char *p, int stride, int w, int h, int x, int y)
{ x = clampc(x, 0, w - 1); y = clampc(y, 0, h - 1); return p[y * stride + x]; }

/* Horizontal / vertical unclipped 6-tap sums centred on (x,y). */
static int hor6(const unsigned char *p, int st, int w, int h, int x, int y)
{
	return gp(p,st,w,h,x-2,y) - 5*gp(p,st,w,h,x-1,y) + 20*gp(p,st,w,h,x,y)
	     + 20*gp(p,st,w,h,x+1,y) - 5*gp(p,st,w,h,x+2,y) + gp(p,st,w,h,x+3,y);
}
static int ver6(const unsigned char *p, int st, int w, int h, int x, int y)
{
	return gp(p,st,w,h,x,y-2) - 5*gp(p,st,w,h,x,y-1) + 20*gp(p,st,w,h,x,y)
	     + 20*gp(p,st,w,h,x,y+1) - 5*gp(p,st,w,h,x,y+2) + gp(p,st,w,h,x,y+3);
}

/* Interpolated luma sample at integer base (ix,iy) with quarter-pel (fx,fy). */
static int luma_sample(const unsigned char *p, int st, int w, int h, int ix, int iy, int fx, int fy)
{
	int G, H, M, b, hh, m, s, j, j1;
	if (fx == 0 && fy == 0) return gp(p,st,w,h,ix,iy);
	G = gp(p,st,w,h,ix,iy);
	if (fy == 0) {                                        /* row: a,b,c            */
		b = clip8((hor6(p,st,w,h,ix,iy) + 16) >> 5);
		if (fx == 2) return b;
		H = gp(p,st,w,h,ix + 1,iy);
		return (fx == 1) ? (G + b + 1) >> 1 : (H + b + 1) >> 1;
	}
	if (fx == 0) {                                        /* column: d,h,n         */
		hh = clip8((ver6(p,st,w,h,ix,iy) + 16) >> 5);
		if (fy == 2) return hh;
		M = gp(p,st,w,h,ix,iy + 1);
		return (fy == 1) ? (G + hh + 1) >> 1 : (M + hh + 1) >> 1;
	}
	/* centre column j (needed by every remaining position) */
	j1 = hor6(p,st,w,h,ix,iy-2) - 5*hor6(p,st,w,h,ix,iy-1) + 20*hor6(p,st,w,h,ix,iy)
	   + 20*hor6(p,st,w,h,ix,iy+1) - 5*hor6(p,st,w,h,ix,iy+2) + hor6(p,st,w,h,ix,iy+3);
	j = clip8((j1 + 512) >> 10);
	if (fx == 2 && fy == 2) return j;
	b  = clip8((hor6(p,st,w,h,ix,iy) + 16) >> 5);
	hh = clip8((ver6(p,st,w,h,ix,iy) + 16) >> 5);
	m  = clip8((ver6(p,st,w,h,ix+1,iy) + 16) >> 5);
	s  = clip8((hor6(p,st,w,h,ix,iy+1) + 16) >> 5);
	if (fx == 2)                return (fy == 1) ? (b + j + 1) >> 1 : (j + s + 1) >> 1;  /* f,q */
	if (fy == 2)                return (fx == 1) ? (hh + j + 1) >> 1 : (j + m + 1) >> 1; /* i,k */
	if (fx == 1 && fy == 1) return (b + hh + 1) >> 1;   /* e */
	if (fx == 3 && fy == 1) return (b + m + 1) >> 1;    /* g */
	if (fx == 1 && fy == 3) return (hh + s + 1) >> 1;   /* p */
	return (m + s + 1) >> 1;                             /* r */
}

void h264_inter_pred(struct h264_decoder *d, int mbx, int mby,
                     int bx, int by, int bw, int bh, h264_pic *ref, int mvx, int mvy)
{
	int lx, ly;
	if (!ref) return;
	for (ly = 0; ly < bh; ly++) for (lx = 0; lx < bw; lx++) {
		int fullx = mbx * 16 + bx + lx, fully = mby * 16 + by + ly;
		int rx = fullx * 4 + mvx, ry = fully * 4 + mvy;
		d->cur->y[fully * d->ystride + fullx] =
			(unsigned char)luma_sample(ref->y, d->ystride, d->yw, d->yh, rx >> 2, ry >> 2, rx & 3, ry & 3);
	}
	/* chroma: bilinear 1/8-pel; block is bw/2 x bh/2 chroma samples */
	{
		int cx, cy, comp;
		for (comp = 0; comp < 2; comp++) {
			unsigned char *dstpl = comp == 0 ? d->cur->cb : d->cur->cr;
			unsigned char *rpl   = comp == 0 ? ref->cb : ref->cr;
			for (cy = 0; cy < bh / 2; cy++) for (cx = 0; cx < bw / 2; cx++) {
				int fx = mbx * 8 + bx / 2 + cx, fy = mby * 8 + by / 2 + cy;
				int rcx = fx * 8 + mvx, rcy = fy * 8 + mvy;
				int ix = rcx >> 3, iy = rcy >> 3, xf = rcx & 7, yf = rcy & 7;
				int A = gp(rpl,d->cstride,d->cw,d->ch,ix,iy);
				int B = gp(rpl,d->cstride,d->cw,d->ch,ix+1,iy);
				int C = gp(rpl,d->cstride,d->cw,d->ch,ix,iy+1);
				int D = gp(rpl,d->cstride,d->cw,d->ch,ix+1,iy+1);
				dstpl[fy * d->cstride + fx] = (unsigned char)
					(((8-xf)*(8-yf)*A + xf*(8-yf)*B + (8-xf)*yf*C + xf*yf*D + 32) >> 6);
			}
		}
	}
}

/* ---- motion-vector prediction --------------------------------------------- */
typedef struct { int avail, mvx, mvy, ref; } nbmv;

/* Fetch the 4x4 block at MB-relative (nx4,ny4) (may be -1 or 4). */
static nbmv get_mv(struct h264_decoder *d, int mbx, int mby, int nx4, int ny4)
{
	nbmv r = { 0, 0, 0, -1 };
	int ambx = mbx, amby = mby, lx = nx4, ly = ny4, cur_addr = mby * d->mb_w + mbx;
	if (nx4 < 0) { ambx--; lx = nx4 + 4; } else if (nx4 > 3) { ambx++; lx = nx4 - 4; }
	if (ny4 < 0) { amby--; ly = ny4 + 4; } else if (ny4 > 3) { amby++; ly = ny4 - 4; }
	if (ambx == mbx && amby == mby) {
		h264_mb *cur = &d->mbs[cur_addr];
		/* A same-MB neighbour is available only once it has been decoded (its
		 * ref index has been stored); an undecoded 4x4 is unavailable. */
		if (cur->refidx[ly*4+lx] < 0) return r;
		r.avail = 1; r.ref = cur->refidx[ly*4+lx];
		r.mvx = cur->mv[ly*4+lx][0]; r.mvy = cur->mv[ly*4+lx][1];
		return r;
	}
	if (!h264_mb_avail(d, cur_addr, ambx, amby)) return r;
	{
		h264_mb *m = h264_mb_at(d, ambx, amby);
		r.avail = 1;
		if (m->intra) { r.ref = -1; r.mvx = r.mvy = 0; }
		else { r.ref = m->refidx[ly*4+lx]; r.mvx = m->mv[ly*4+lx][0]; r.mvy = m->mv[ly*4+lx][1]; }
	}
	return r;
}

static int median3(int a, int b, int c)
{
	int mn = a < b ? a : b, mx = a > b ? a : b;
	if (c < mn) mn = c;
	if (c > mx) mx = c;
	return a + b + c - mn - mx;
}

void h264_pred_mv(struct h264_decoder *d, int mbx, int mby, int bx4, int by4,
                  int pw, int ph, int refidx, int *mvx, int *mvy)
{
	nbmv A = get_mv(d, mbx, mby, bx4 - 1, by4);
	nbmv B = get_mv(d, mbx, mby, bx4, by4 - 1);
	/* Top-right neighbour C; if unavailable (off-picture, wrong slice, or not yet
	 * decoded), fall back to the top-left neighbour D. */
	nbmv C = get_mv(d, mbx, mby, bx4 + pw / 4, by4 - 1);
	if (!C.avail) C = get_mv(d, mbx, mby, bx4 - 1, by4 - 1);

	/* Directional predictors for the two-partition shapes. */
	if (pw == 16 && ph == 8) {
		if (by4 == 0 && B.ref == refidx) { *mvx = B.mvx; *mvy = B.mvy; return; }
		if (by4 != 0 && A.ref == refidx) { *mvx = A.mvx; *mvy = A.mvy; return; }
	} else if (pw == 8 && ph == 16) {
		if (bx4 == 0 && A.ref == refidx) { *mvx = A.mvx; *mvy = A.mvy; return; }
		if (bx4 != 0 && C.ref == refidx) { *mvx = C.mvx; *mvy = C.mvy; return; }
	}

	if (!B.avail && !C.avail && A.avail) { *mvx = A.mvx; *mvy = A.mvy; return; }

	{
		int m = (A.ref == refidx) + (B.ref == refidx) + (C.ref == refidx);
		if (m == 1) {
			if (A.ref == refidx) { *mvx = A.mvx; *mvy = A.mvy; }
			else if (B.ref == refidx) { *mvx = B.mvx; *mvy = B.mvy; }
			else { *mvx = C.mvx; *mvy = C.mvy; }
		} else {
			*mvx = median3(A.mvx, B.mvx, C.mvx);
			*mvy = median3(A.mvy, B.mvy, C.mvy);
		}
	}
}

void h264_skip_mv(struct h264_decoder *d, int mbx, int mby, int *mvx, int *mvy)
{
	nbmv A = get_mv(d, mbx, mby, -1, 0);
	nbmv B = get_mv(d, mbx, mby, 0, -1);
	if (!A.avail || !B.avail ||
	    (A.ref == 0 && A.mvx == 0 && A.mvy == 0) ||
	    (B.ref == 0 && B.mvx == 0 && B.mvy == 0)) {
		*mvx = 0; *mvy = 0; return;
	}
	h264_pred_mv(d, mbx, mby, 0, 0, 16, 16, 0, mvx, mvy);
}
