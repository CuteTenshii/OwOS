/* upng.c — streaming PNG decoder (see upng.h). Ring-3, built on inflate.c.
 *
 * Flow: verify signature -> read IHDR (+ PLTE) -> stream the concatenated IDAT
 * data (spanning chunks, skipping their length/CRC framing) through the
 * streaming inflater -> reassemble each filtered scanline as inflate output
 * arrives -> reverse the PNG filter -> expand samples to 8-bit RGB -> hand the
 * row to the caller.
 *
 * Supported: all five colour types — grayscale(0), RGB(2), palette(3),
 * gray+alpha(4), RGBA(6) — at bit depths 1/2/4/8/16, interlaced or not. Alpha
 * is dropped (the row callback is RGB). Non-interlaced images stream with a
 * working set of ~2 scanlines whatever the height; interlaced images need a
 * full RGB frame buffer (Adam7 passes deposit pixels across the whole image),
 * so they are capped at IL_MAXPX pixels and otherwise return UPNG_EUNSUP.
 */
#include "upng.h"
#include "inflate.h"
#include <stdint.h>

#define MAXW     4096            /* widest image we'll decode (bounds buffers)   */
#define ROWBYTES (MAXW * 8)      /* max stride: 16-bit RGBA = w*4*2 bytes        */
#define IL_MAXPX (512u * 512u)   /* interlaced frame budget (RGB): 512x512=768KiB*/

struct dec {
	upng_reader rd;  void *rctx;
	upng_row    row; void *row_ctx;
	unsigned w, h;
	unsigned depth, color, samples;  /* IHDR depth, colour type, samples/pixel  */
	unsigned stride;                 /* bytes per (sub-)scanline                */
	unsigned filt_bpp;               /* filter "bpp": ceil(samples*depth/8), >=1*/
	unsigned cur_y;
	unsigned col;                    /* bytes filled in cur[] (incl filter byte)*/
	int      prev_valid;             /* prev[] holds a real previous row        */
	int      rowstop;                /* the row callback asked to stop          */
	/* interlace pass state */
	int      interlaced;
	int      pass;                   /* current Adam7 pass 0..6, 7 = done       */
	unsigned pass_w, pass_h, pass_stride, pass_row;
	/* palette */
	uint8_t  plte[256 * 3];
	unsigned plte_n;
	/* transparency (tRNS) */
	uint8_t  trns[256];              /* palette: per-index alpha               */
	unsigned trns_n;                 /* number of palette alphas given         */
	int      has_ckey;               /* gray/RGB single transparent colour set */
	unsigned ck_r, ck_g, ck_b;       /* that colour, as 8-bit RGB              */
	/* IDAT chunk framing for idat_src(). */
	uint32_t chunk_left;
	int      in_chunk;
	int      idat_eof;
	uint8_t  cur[1 + ROWBYTES];      /* current scanline: filter byte + samples */
	uint8_t  prev[ROWBYTES];         /* previous reconstructed samples          */
	uint8_t  rgba[MAXW * 4];         /* one RGBA row handed to the callback     */
};

static struct dec D;
static uint8_t    il_frame[IL_MAXPX * 3];   /* interlaced de-interlace buffer   */

/* Adam7 interlace pass geometry. */
static const unsigned IL_XSTART[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const unsigned IL_YSTART[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const unsigned IL_XSTEP[7]  = { 8, 8, 4, 4, 2, 2, 1 };
static const unsigned IL_YSTEP[7]  = { 8, 8, 8, 4, 4, 2, 2 };

static int rb(struct dec *d) { return d->rd(d->rctx); }

static int be32(struct dec *d, uint32_t *v)
{
	int a = rb(d), b = rb(d), c = rb(d), e = rb(d);
	if (a < 0 || b < 0 || c < 0 || e < 0)
		return -1;
	*v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)e;
	return 0;
}

static int chunk_hdr(struct dec *d, uint32_t *len, uint8_t ty[4])
{
	if (be32(d, len) < 0)
		return -1;
	for (int i = 0; i < 4; i++) {
		int c = rb(d);
		if (c < 0) return -1;
		ty[i] = (uint8_t)c;
	}
	return 0;
}

static int is_type(const uint8_t *ty, const char *s)
{
	return ty[0] == s[0] && ty[1] == s[1] && ty[2] == s[2] && ty[3] == s[3];
}

static int skip(struct dec *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (rb(d) < 0) return -1;
	return 0;
}

/* Reader handed to inflate: yields the concatenated IDAT *data* bytes, jumping
 * over each IDAT's CRC and the next chunk's length/type, and signalling EOF when
 * a non-IDAT chunk (IEND) is reached. */
static int idat_src(void *ctx)
{
	struct dec *d = ctx;
	while (d->chunk_left == 0) {
		if (d->idat_eof)
			return -1;
		if (d->in_chunk) {                 /* consume the finished chunk's CRC */
			if (skip(d, 4) < 0) { d->idat_eof = 1; return -1; }
			d->in_chunk = 0;
		}
		uint32_t len; uint8_t ty[4];
		if (chunk_hdr(d, &len, ty) < 0) { d->idat_eof = 1; return -1; }
		if (is_type(ty, "IDAT")) {
			d->chunk_left = len;
			d->in_chunk = 1;               /* even an empty IDAT has a CRC      */
		} else {
			d->idat_eof = 1;               /* IEND or other: IDAT data ends    */
			return -1;
		}
	}
	int b = rb(d);
	if (b < 0) { d->idat_eof = 1; return -1; }
	d->chunk_left--;
	return b;
}

static uint8_t paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = p > a ? p - a : a - p;
	int pb = p > b ? p - b : b - p;
	int pc = p > c ? p - c : c - p;
	if (pa <= pb && pa <= pc) return (uint8_t)a;
	if (pb <= pc) return (uint8_t)b;
	return (uint8_t)c;
}

/* Reverse the PNG filter on cur[] in place, using filt_bpp + the given stride
 * (which is the full-row stride or a pass stride). Returns 0, or <0 on a bad
 * filter byte. Rolls the result into prev[] for the next row. */
static int unfilter(struct dec *d, unsigned st)
{
	uint8_t  ft = d->cur[0];
	uint8_t *row = d->cur + 1;
	unsigned bpp = d->filt_bpp;
	for (unsigned x = 0; x < st; x++) {
		int a = x >= bpp ? row[x - bpp] : 0;
		int b = d->prev_valid ? d->prev[x] : 0;
		int c = (d->prev_valid && x >= bpp) ? d->prev[x - bpp] : 0;
		int v = row[x];
		switch (ft) {
		case 0: break;
		case 1: v += a; break;
		case 2: v += b; break;
		case 3: v += (a + b) / 2; break;
		case 4: v += paeth(a, b, c); break;
		default: return -1;
		}
		row[x] = (uint8_t)v;
	}
	for (unsigned x = 0; x < st; x++)
		d->prev[x] = row[x];
	d->prev_valid = 1;
	return 0;
}

/* One 8-bit-scaled colour sample (sample index `si` within the unfiltered row).
 * Handles 16-bit (take the high byte), 8-bit, and sub-byte depths (scaled to
 * 0..255). */
static unsigned chan8(const uint8_t *row, unsigned si, unsigned depth)
{
	if (depth == 16) return row[si * 2];
	if (depth == 8)  return row[si];
	unsigned per = 8 / depth, mask = (1u << depth) - 1;
	unsigned byte = row[si / per];
	unsigned shift = (per - 1 - (si % per)) * depth;
	return ((byte >> shift) & mask) * 255u / mask;
}

/* One raw (unscaled) sample — used for palette indices (depth 1/2/4/8). */
static unsigned raw_sample(const uint8_t *row, unsigned si, unsigned depth)
{
	if (depth == 8) return row[si];
	unsigned per = 8 / depth, mask = (1u << depth) - 1;
	unsigned byte = row[si / per];
	unsigned shift = (per - 1 - (si % per)) * depth;
	return (byte >> shift) & mask;
}

/* Expand `n` pixels of the unfiltered row into RGBA quads in dst — alpha comes
 * from an explicit alpha channel (types 4/6), a palette tRNS table (type 3), or
 * a single transparent colour-key (gray/RGB tRNS); otherwise 255 (opaque). */
static void row_to_rgba(struct dec *d, const uint8_t *row, uint8_t *dst, unsigned n)
{
	unsigned depth = d->depth;
	for (unsigned x = 0; x < n; x++) {
		uint8_t *o = dst + x * 4;
		o[3] = 255;
		switch (d->color) {
		case 0: {                                   /* grayscale               */
			unsigned g = chan8(row, x, depth);
			o[0] = o[1] = o[2] = (uint8_t)g;
			if (d->has_ckey && g == d->ck_r) o[3] = 0;
			break;
		}
		case 2:                                     /* RGB                     */
			o[0] = (uint8_t)chan8(row, x * 3 + 0, depth);
			o[1] = (uint8_t)chan8(row, x * 3 + 1, depth);
			o[2] = (uint8_t)chan8(row, x * 3 + 2, depth);
			if (d->has_ckey && o[0] == d->ck_r && o[1] == d->ck_g && o[2] == d->ck_b)
				o[3] = 0;
			break;
		case 3: {                                   /* palette (indexed)       */
			unsigned idx = raw_sample(row, x, depth);
			if (idx >= d->plte_n) idx = 0;
			const uint8_t *p = d->plte + idx * 3;
			o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
			if (idx < d->trns_n) o[3] = d->trns[idx];
			break;
		}
		case 4:                                     /* gray + alpha            */
			o[0] = o[1] = o[2] = (uint8_t)chan8(row, x * 2 + 0, depth);
			o[3] = (uint8_t)chan8(row, x * 2 + 1, depth);
			break;
		case 6:                                     /* RGBA                    */
			o[0] = (uint8_t)chan8(row, x * 4 + 0, depth);
			o[1] = (uint8_t)chan8(row, x * 4 + 1, depth);
			o[2] = (uint8_t)chan8(row, x * 4 + 2, depth);
			o[3] = (uint8_t)chan8(row, x * 4 + 3, depth);
			break;
		}
	}
}

/* ------------------------------------------------------- non-interlaced sink */

static int emit_scanline(struct dec *d)
{
	if (unfilter(d, d->stride) < 0) return -1;
	row_to_rgba(d, d->cur + 1, d->rgba, d->w);
	return d->row(d->row_ctx, d->cur_y, d->rgba, d->w);
}

static int sink_plain(struct dec *d, const unsigned char *buf, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		d->cur[d->col++] = buf[i];
		if (d->col == 1 + d->stride) {
			int r = emit_scanline(d);
			d->col = 0;
			d->cur_y++;
			if (r < 0) { d->rowstop = 1; return -1; }
			if (d->cur_y >= d->h) return -1;       /* all rows done            */
		}
	}
	return 0;
}

/* ----------------------------------------------------------- interlaced sink */

/* Advance to the next non-empty Adam7 pass (or pass 7 = finished). Sets the
 * pass width/height/stride and resets the per-pass filter state. */
static void il_next_pass(struct dec *d)
{
	while (++d->pass < 7) {
		unsigned xs = IL_XSTART[d->pass], ys = IL_YSTART[d->pass];
		unsigned xst = IL_XSTEP[d->pass], yst = IL_YSTEP[d->pass];
		d->pass_w = (d->w > xs) ? (d->w - xs + xst - 1) / xst : 0;
		d->pass_h = (d->h > ys) ? (d->h - ys + yst - 1) / yst : 0;
		if (d->pass_w && d->pass_h) {
			d->pass_stride = (d->pass_w * d->samples * d->depth + 7) / 8;
			d->pass_row = 0; d->col = 0; d->prev_valid = 0;
			return;
		}
	}
}

static int sink_interlaced(struct dec *d, const unsigned char *buf, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (d->pass >= 7) return -1;
		d->cur[d->col++] = buf[i];
		if (d->col == 1 + d->pass_stride) {
			if (unfilter(d, d->pass_stride) < 0) { d->rowstop = 0; return -1; }
			row_to_rgba(d, d->cur + 1, d->rgba, d->pass_w);
			unsigned fy = IL_YSTART[d->pass] + d->pass_row * IL_YSTEP[d->pass];
			unsigned xs = IL_XSTART[d->pass], xst = IL_XSTEP[d->pass];
			uint8_t *base = il_frame + (unsigned long)fy * d->w * 3;
			for (unsigned x = 0; x < d->pass_w; x++) {
				uint8_t *o = base + (unsigned long)(xs + x * xst) * 3;
				const uint8_t *s = d->rgba + x * 4;   /* alpha dropped here  */
				o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
			}
			d->col = 0;
			if (++d->pass_row >= d->pass_h) {
				il_next_pass(d);
				if (d->pass >= 7) return -1;       /* all passes done          */
			}
		}
	}
	return 0;
}

/* inflate output sink: dispatch to the plain or interlaced reassembler. */
static int scan_sink(void *ctx, const unsigned char *buf, unsigned n)
{
	struct dec *d = ctx;
	return d->interlaced ? sink_interlaced(d, buf, n) : sink_plain(d, buf, n);
}

int upng_dimensions(upng_reader rd, void *rctx, unsigned *w, unsigned *h)
{
	/* Read inline (no struct dec on the stack — it is large and would blow a
	 * small ring-3 stack). Signature, then the IHDR chunk's length+type, then
	 * its first 8 bytes (width, height). */
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	for (int i = 0; i < 8; i++)
		if (rd(rctx) != sig[i])
			return UPNG_EBADSIG;
	unsigned char b[8];
	for (int i = 0; i < 8; i++) {           /* chunk length(4) + type(4)        */
		int c = rd(rctx);
		if (c < 0) return UPNG_ETRUNC;
		b[i] = (unsigned char)c;
	}
	if (!(b[4] == 'I' && b[5] == 'H' && b[6] == 'D' && b[7] == 'R'))
		return UPNG_EBADHDR;
	unsigned char d[8];
	for (int i = 0; i < 8; i++) {            /* IHDR: width(4), height(4)        */
		int c = rd(rctx);
		if (c < 0) return UPNG_ETRUNC;
		d[i] = (unsigned char)c;
	}
	if (w) *w = ((unsigned)d[0] << 24) | ((unsigned)d[1] << 16) | ((unsigned)d[2] << 8) | d[3];
	if (h) *h = ((unsigned)d[4] << 24) | ((unsigned)d[5] << 16) | ((unsigned)d[6] << 8) | d[7];
	return UPNG_OK;
}

/* Validate the IHDR depth/colour combination per the PNG spec, and set samples. */
static int set_format(struct dec *d)
{
	unsigned dep = d->depth;
	switch (d->color) {
	case 0: d->samples = 1; if (!(dep==1||dep==2||dep==4||dep==8||dep==16)) return -1; break;
	case 2: d->samples = 3; if (!(dep==8||dep==16)) return -1; break;
	case 3: d->samples = 1; if (!(dep==1||dep==2||dep==4||dep==8)) return -1; break;
	case 4: d->samples = 2; if (!(dep==8||dep==16)) return -1; break;
	case 6: d->samples = 4; if (!(dep==8||dep==16)) return -1; break;
	default: return -1;
	}
	d->filt_bpp = (d->samples * dep + 7) / 8;
	if (d->filt_bpp == 0) d->filt_bpp = 1;
	d->stride = (d->w * d->samples * dep + 7) / 8;
	return 0;
}

int upng_decode(upng_reader rd, void *rctx, upng_row row, void *row_ctx,
                unsigned *out_w, unsigned *out_h)
{
	struct dec *d = &D;
	d->rd = rd; d->rctx = rctx; d->row = row; d->row_ctx = row_ctx;
	d->cur_y = 0; d->col = 0; d->prev_valid = 0; d->rowstop = 0;
	d->chunk_left = 0; d->in_chunk = 0; d->idat_eof = 0;
	d->plte_n = 0; d->pass = -1;
	d->trns_n = 0; d->has_ckey = 0;

	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	for (int i = 0; i < 8; i++)
		if (rb(d) != sig[i])
			return UPNG_EBADSIG;

	unsigned interlace = 0;
	int have_ihdr = 0;
	for (;;) {
		uint32_t len; uint8_t ty[4];
		if (chunk_hdr(d, &len, ty) < 0)
			return UPNG_ETRUNC;
		if (is_type(ty, "IHDR")) {
			if (len < 13) return UPNG_EBADHDR;
			uint8_t hb[13];
			for (int i = 0; i < 13; i++) { int c = rb(d); if (c < 0) return UPNG_ETRUNC; hb[i] = (uint8_t)c; }
			if (len > 13 && skip(d, len - 13) < 0) return UPNG_ETRUNC;
			d->w = ((unsigned)hb[0] << 24) | (hb[1] << 16) | (hb[2] << 8) | hb[3];
			d->h = ((unsigned)hb[4] << 24) | (hb[5] << 16) | (hb[6] << 8) | hb[7];
			d->depth = hb[8]; d->color = hb[9]; interlace = hb[12];
			if (skip(d, 4) < 0) return UPNG_ETRUNC;       /* CRC               */
			have_ihdr = 1;
		} else if (is_type(ty, "PLTE")) {
			if (!have_ihdr) return UPNG_EBADHDR;
			unsigned n = len / 3; if (n > 256) n = 256;
			for (unsigned i = 0; i < n * 3; i++) { int c = rb(d); if (c < 0) return UPNG_ETRUNC; d->plte[i] = (uint8_t)c; }
			if (skip(d, len - n * 3 + 4) < 0) return UPNG_ETRUNC;   /* rest+CRC */
			d->plte_n = n;
		} else if (is_type(ty, "tRNS")) {
			if (!have_ihdr) return UPNG_EBADHDR;
			if (d->color == 3) {                          /* per-index alpha    */
				unsigned n = len > 256 ? 256 : len;
				for (unsigned i = 0; i < n; i++) { int c = rb(d); if (c < 0) return UPNG_ETRUNC; d->trns[i] = (uint8_t)c; }
				if (skip(d, len - n + 4) < 0) return UPNG_ETRUNC;
				d->trns_n = n;
			} else if (d->color == 0 && len >= 2) {       /* gray colour-key    */
				uint8_t b0 = (uint8_t)rb(d), b1 = (uint8_t)rb(d);
				d->ck_r = d->ck_g = d->ck_b = (d->depth == 16) ? b0 : b1;
				d->has_ckey = 1;
				if (skip(d, len - 2 + 4) < 0) return UPNG_ETRUNC;
			} else if (d->color == 2 && len >= 6) {       /* RGB colour-key     */
				uint8_t v[6]; for (int i = 0; i < 6; i++) { int c = rb(d); if (c < 0) return UPNG_ETRUNC; v[i] = (uint8_t)c; }
				d->ck_r = (d->depth == 16) ? v[0] : v[1];
				d->ck_g = (d->depth == 16) ? v[2] : v[3];
				d->ck_b = (d->depth == 16) ? v[4] : v[5];
				d->has_ckey = 1;
				if (skip(d, len - 6 + 4) < 0) return UPNG_ETRUNC;
			} else if (skip(d, len + 4) < 0) {
				return UPNG_ETRUNC;
			}
		} else if (is_type(ty, "IDAT")) {
			if (!have_ihdr) return UPNG_EBADHDR;
			d->chunk_left = len; d->in_chunk = 1;         /* first IDAT open    */
			break;
		} else if (is_type(ty, "IEND")) {
			return UPNG_ETRUNC;                            /* no image data     */
		} else if (skip(d, len + 4) < 0) {                 /* ancillary + CRC    */
			return UPNG_ETRUNC;
		}
	}
	if (!have_ihdr || d->w == 0 || d->h == 0) return UPNG_EBADHDR;
	if (interlace > 1 || d->w > MAXW) return UPNG_EUNSUP;
	if (set_format(d) < 0) return UPNG_EUNSUP;
	if (d->color == 3 && d->plte_n == 0) return UPNG_EBADHDR;  /* palette needs PLTE */
	d->interlaced = (interlace == 1);
	if (d->interlaced) {
		if ((unsigned long)d->w * d->h > IL_MAXPX) return UPNG_EUNSUP;
		il_next_pass(d);                              /* prime the first pass   */
	}

	if (out_w) *out_w = d->w;
	if (out_h) *out_h = d->h;

	/* IDAT is a zlib stream; inflate_http detects the 0x78 header and inflates. */
	inflate_http(idat_src, d, scan_sink, d);

	if (d->interlaced) {
		if (d->pass < 7) return UPNG_EINFLATE;        /* didn't finish passes   */
		for (unsigned y = 0; y < d->h; y++) {         /* emit the assembled rows*/
			const uint8_t *src = il_frame + (unsigned long)y * d->w * 3;
			for (unsigned x = 0; x < d->w; x++) {     /* RGB frame -> RGBA, opaque */
				d->rgba[x*4] = src[x*3]; d->rgba[x*4+1] = src[x*3+1];
				d->rgba[x*4+2] = src[x*3+2]; d->rgba[x*4+3] = 255;
			}
			int r = d->row(d->row_ctx, y, d->rgba, d->w);
			if (r < 0) return UPNG_OK;                /* caller stopped: fine   */
		}
		return UPNG_OK;
	}

	if (d->rowstop) return UPNG_OK;                 /* caller stopped: fine      */
	if (d->cur_y < d->h) return UPNG_EINFLATE;      /* didn't get every row      */
	return UPNG_OK;
}
