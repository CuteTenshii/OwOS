/* woff.c — WOFF 1.0 + WOFF2 → SFNT decoders (see woff.h).
 *
 * WOFF1: a header + table directory; each table is stored or zlib-compressed.
 * Reassembling the SFNT is just inflate-per-table + a fresh offset table.
 *
 * WOFF2 (RFC-ish, W3C WOFF2): the whole table set is one brotli stream, and the
 * glyf/loca tables are re-encoded into a compact transform that must be undone
 * to rebuild the standard glyf outlines + loca offsets (and hmtx may be
 * transformed too). The transform logic mirrors the reference woff2 decoder. */
#include "woff.h"
#include "owos.h"
#include "inflate.h"      /* zlib (WOFF1 per-table) */
#include "brotli.h"       /* brotli (WOFF2 combined stream) */

/* ----------------------------------------------------------- byte reader --- */
struct rd { const unsigned char *p; int len, pos; int err; };
static unsigned ru8(struct rd *r)  { if (r->pos + 1 > r->len) { r->err = 1; return 0; } return r->p[r->pos++]; }
static unsigned ru16(struct rd *r) { unsigned a = ru8(r), b = ru8(r); return (a << 8) | b; }
static unsigned ru32(struct rd *r) { unsigned a = ru16(r), b = ru16(r); return (a << 16) | b; }
static void rskip(struct rd *r, int n) { r->pos += n; if (r->pos > r->len) r->err = 1; }

/* WOFF2 UIntBase128: 1..5 bytes, 7 bits each, big-endian, no leading-zero byte. */
static unsigned rbase128(struct rd *r)
{
	unsigned v = 0;
	for (int i = 0; i < 5; i++) {
		unsigned c = ru8(r);
		if (r->err) return 0;
		if (i == 0 && c == 0x80) { r->err = 1; return 0; }
		if (v & 0xfe000000u) { r->err = 1; return 0; }
		v = (v << 7) | (c & 0x7f);
		if (!(c & 0x80)) return v;
	}
	r->err = 1;
	return 0;
}

/* WOFF2 255UInt16: a compact 1..3 byte unsigned. */
static unsigned r255(struct rd *r)
{
	unsigned c = ru8(r);
	if (c == 253) return ru16(r);
	if (c == 255) return ru8(r) + 253;
	if (c == 254) return ru8(r) + 253 * 2;
	return c;
}

/* ---------------------------------------------------------- output buffer -- */
struct ob { unsigned char *p; int len, cap, oom; };
static void ob_need(struct ob *o, int extra)
{
	if (o->len + extra <= o->cap) return;
	int nc = (o->len + extra) * 2 + 256;
	unsigned char *np = realloc(o->p, (unsigned)nc);
	if (!np) { o->oom = 1; return; }
	o->p = np; o->cap = nc;
}
static void ob_u8(struct ob *o, unsigned v)  { ob_need(o, 1); if (o->oom) return; o->p[o->len++] = (unsigned char)v; }
static void ob_u16(struct ob *o, unsigned v) { ob_u8(o, v >> 8); ob_u8(o, v); }
static void ob_u32(struct ob *o, unsigned v) { ob_u16(o, v >> 16); ob_u16(o, v); }
static void ob_put(struct ob *o, const unsigned char *s, int n)
{
	ob_need(o, n); if (o->oom) return;
	for (int i = 0; i < n; i++) o->p[o->len++] = s[i];
	}
static void ob_pad4(struct ob *o) { while (o->len & 3) ob_u8(o, 0); }

/* SFNT table checksum: sum of big-endian u32 over the data, zero-padded to 4. */
static unsigned table_checksum(const unsigned char *p, int n)
{
	unsigned sum = 0;
	int i = 0;
	for (; i + 4 <= n; i += 4)
		sum += ((unsigned)p[i] << 24) | (p[i+1] << 16) | (p[i+2] << 8) | p[i+3];
	if (i < n) {
		unsigned last = 0;
		for (int k = 0; k < 4; k++) last = (last << 8) | (i + k < n ? p[i + k] : 0);
		sum += last;
	}
	return sum;
}

/* A reconstructed table awaiting assembly into the SFNT. */
struct table { unsigned tag, checksum; unsigned char *data; int len; int owned; };

/* zlib inflate over fixed buffers (WOFF1 per-table); file-scope so they are
 * plain function pointers, not GCC nested-function trampolines. */
struct zsrc { const unsigned char *p; int len, pos; };
static int zsrc_rd(void *c) { struct zsrc *s = c; return s->pos < s->len ? s->p[s->pos++] : -1; }
struct zdst { unsigned char *p; int len, cap; };
static int zdst_wr(void *c, const unsigned char *p, unsigned n)
{
	struct zdst *d = c;
	for (unsigned k = 0; k < n; k++) { if (d->len >= d->cap) return -1; d->p[d->len++] = p[k]; }
	return 0;
}

/* Assemble final SFNT: offset table + records (sorted by tag) + 4-aligned data,
 * and patch head.checkSumAdjustment. Frees each owned table buffer. */
static unsigned char *assemble_sfnt(unsigned flavor, struct table *t, int n, int *out_len)
{
	/* sort table indices by tag (OpenType requires records in tag order) */
	int ord[256];
	for (int i = 0; i < n; i++) ord[i] = i;
	for (int i = 1; i < n; i++) {
		int x = ord[i], j = i;
		while (j > 0 && t[ord[j-1]].tag > t[x].tag) { ord[j] = ord[j-1]; j--; }
		ord[j] = x;
	}

	struct ob o = {0};
	int pow2 = 0; while ((1 << (pow2 + 1)) <= n) pow2++;
	ob_u32(&o, flavor);
	ob_u16(&o, n);
	ob_u16(&o, (1 << pow2) << 4);
	ob_u16(&o, pow2);
	ob_u16(&o, (n << 4) - ((1 << pow2) << 4));

	/* table records: offsets computed from the 4-aligned data layout */
	int data_off = 12 + 16 * n;
	int head_record = -1;
	for (int i = 0; i < n; i++) {
		struct table *e = &t[ord[i]];
		if (e->tag == 0x68656164) head_record = o.len;   /* 'head' */
		ob_u32(&o, e->tag);
		ob_u32(&o, e->checksum);
		ob_u32(&o, (unsigned)data_off);
		ob_u32(&o, (unsigned)e->len);
		data_off += (e->len + 3) & ~3;
	}
	for (int i = 0; i < n; i++) {
		struct table *e = &t[ord[i]];
		ob_put(&o, e->data, e->len);
		ob_pad4(&o);
	}
	if (o.oom) { free(o.p); goto done; }

	/* head.checkSumAdjustment = 0xB1B0AFBA - sum(whole font). head was written
	 * with adjustment 0, so summing the whole buffer now is correct. */
	if (head_record >= 0) {
		unsigned total = table_checksum(o.p, o.len);
		unsigned adj = 0xB1B0AFBAu - total;
		unsigned hoff = ((unsigned)o.p[head_record + 8] << 24) | (o.p[head_record+9] << 16) |
		                (o.p[head_record+10] << 8) | o.p[head_record+11];
		if (hoff + 12 <= (unsigned)o.len) {
			o.p[hoff + 8]  = (adj >> 24) & 0xff;
			o.p[hoff + 9]  = (adj >> 16) & 0xff;
			o.p[hoff + 10] = (adj >> 8) & 0xff;
			o.p[hoff + 11] = adj & 0xff;
		}
	}
	*out_len = o.len;
done:
	for (int i = 0; i < n; i++) if (t[i].owned) free(t[i].data);
	return o.oom ? 0 : o.p;
}

/* ==================================================================== WOFF1 */
static unsigned char *woff1_to_sfnt(const unsigned char *data, int len, int *out_len)
{
	struct rd r = { data, len, 0, 0 };
	rskip(&r, 4);                       /* signature */
	unsigned flavor = ru32(&r);
	rskip(&r, 4);                       /* length */
	int num = ru16(&r);
	rskip(&r, 2 + 4 + 2 + 2 + 12 + 8);  /* reserved, totalSfntSize, ver, meta, priv */
	if (r.err || num <= 0 || num > 256) return 0;

	struct table *t = calloc(num, sizeof *t);
	if (!t) return 0;
	int ok = 1;
	for (int i = 0; i < num && ok; i++) {
		unsigned tag = ru32(&r), off = ru32(&r), clen = ru32(&r), olen = ru32(&r), csum = ru32(&r);
		if (r.err || (long)off + clen > len) { ok = 0; break; }
		unsigned char *out = malloc(olen ? olen : 1);
		if (!out) { ok = 0; break; }
		if (clen < olen) {
			/* zlib-compressed table */
			struct zsrc src = { data + off, (int)clen, 0 };
			struct zdst dst = { out, 0, (int)olen };
			if (inflate_http(zsrc_rd, &src, zdst_wr, &dst) < 0 || dst.len != (int)olen) { free(out); ok = 0; break; }
		} else {
			for (unsigned k = 0; k < olen; k++) out[k] = data[off + k];
		}
		t[i].tag = tag; t[i].data = out; t[i].len = (int)olen; t[i].owned = 1;
		t[i].checksum = csum ? csum : table_checksum(out, (int)olen);
		if (tag == 0x68656164 && olen >= 12) { out[8] = out[9] = out[10] = out[11] = 0; }  /* zero adj */
	}
	if (!ok) { for (int i = 0; i < num; i++) free(t[i].data); free(t); return 0; }
	unsigned char *sfnt = assemble_sfnt(flavor, t, num, out_len);
	free(t);
	return sfnt;
}

/* ==================================================================== WOFF2 */
static const unsigned KNOWN_TAGS[63] = {
	0x636d6170,0x68656164,0x68686561,0x686d7478,0x6d617870,0x6e616d65,0x4f532f32,0x706f7374,
	0x63767420,0x6670676d,0x676c7966,0x6c6f6361,0x70726570,0x43464620,0x564f5247,0x45424454,
	0x45424c43,0x67617370,0x68646d78,0x6b65726e,0x4c545348,0x50434c54,0x56444d58,0x76686561,
	0x766d7478,0x42415345,0x47444546,0x47504f53,0x47535542,0x45425343,0x4a535446,0x4d415448,
	0x43424454,0x43424c43,0x434f4c52,0x4350414c,0x53564720,0x73626978,0x61636e74,0x61766172,
	0x62646174,0x626c6f63,0x62736c6e,0x63766172,0x66647363,0x66656174,0x666d7478,0x66766172,
	0x67766172,0x68737479,0x6a757374,0x6c636172,0x6d6f7274,0x6d6f7278,0x6f706264,0x70726f70,
	0x7472616b,0x5a617066,0x53696c66,0x476c6174,0x476c6f63,0x46656174,0x53696c6c
};
#define TAG_GLYF 0x676c7966
#define TAG_LOCA 0x6c6f6361
#define TAG_HEAD 0x68656164
#define TAG_HHEA 0x68686561
#define TAG_HMTX 0x686d7478

/* simple-glyph flags */
#define G_ONCURVE 1
#define G_XSHORT  2
#define G_YSHORT  4
#define G_REPEAT  8
#define G_XSAME   16
#define G_YSAME   32
#define G_OVERLAP 64
#define ENDPTS_OFF 10

struct point { int x, y, on; };

static int with_sign(int flag, int base) { return (flag & 1) ? base : -base; }

/* Decode n_points triplet-encoded outline points; returns bytes consumed from
 * `in`, or -1 on overrun. */
static int triplet_decode(const unsigned char *flags, const unsigned char *in, int in_size,
                          int n_points, struct point *out)
{
	int x = 0, y = 0, ti = 0;
	for (int i = 0; i < n_points; i++) {
		int flag = flags[i];
		int on = !(flag >> 7);
		flag &= 0x7f;
		int nbytes = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
		if (ti + nbytes > in_size) return -1;
		int dx, dy;
		if (flag < 10) { dx = 0; dy = with_sign(flag, ((flag & 14) << 7) + in[ti]); }
		else if (flag < 20) { dx = with_sign(flag, (((flag - 10) & 14) << 7) + in[ti]); dy = 0; }
		else if (flag < 84) {
			int b0 = flag - 20, b1 = in[ti];
			dx = with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
			dy = with_sign(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
		} else if (flag < 120) {
			int b0 = flag - 84;
			dx = with_sign(flag, 1 + ((b0 / 12) << 8) + in[ti]);
			dy = with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[ti + 1]);
		} else if (flag < 124) {
			int b2 = in[ti + 1];
			dx = with_sign(flag, (in[ti] << 4) + (b2 >> 4));
			dy = with_sign(flag >> 1, ((b2 & 0x0f) << 8) + in[ti + 2]);
		} else {
			dx = with_sign(flag, (in[ti] << 8) + in[ti + 1]);
			dy = with_sign(flag >> 1, (in[ti + 2] << 8) + in[ti + 3]);
		}
		ti += nbytes;
		x += dx; y += dy;
		out[i].x = x; out[i].y = y; out[i].on = on;
	}
	return ti;
}

static int s16(int v) { return (int)(short)(v & 0xffff); }
static int put16(unsigned char *d, int off, int v) { d[off] = (v >> 8) & 0xff; d[off+1] = v & 0xff; return off + 2; }

/* Encode points into the standard simple-glyph format at dst (flags then x then
 * y), starting at the precomputed flag_offset. Returns the glyph's total size. */
static int store_points(int n_points, const struct point *pts, int n_contours,
                       int instr_len, int has_overlap, unsigned char *dst, int dst_size)
{
	int flag_offset = ENDPTS_OFF + 2 * n_contours + 2 + instr_len;
	int last_flag = -1, repeat = 0, last_x = 0, last_y = 0, x_bytes = 0, y_bytes = 0;
	for (int i = 0; i < n_points; i++) {
		int flag = pts[i].on ? G_ONCURVE : 0;
		if (has_overlap && i == 0) flag |= G_OVERLAP;
		int dx = pts[i].x - last_x, dy = pts[i].y - last_y;
		if (dx == 0) flag |= G_XSAME;
		else if (dx > -256 && dx < 256) { flag |= G_XSHORT | (dx > 0 ? G_XSAME : 0); x_bytes += 1; }
		else x_bytes += 2;
		if (dy == 0) flag |= G_YSAME;
		else if (dy > -256 && dy < 256) { flag |= G_YSHORT | (dy > 0 ? G_YSAME : 0); y_bytes += 1; }
		else y_bytes += 2;
		if (flag == last_flag && repeat != 255) { dst[flag_offset - 1] |= G_REPEAT; repeat++; }
		else {
			if (repeat != 0) { if (flag_offset >= dst_size) return -1; dst[flag_offset++] = repeat; }
			if (flag_offset >= dst_size) return -1;
			dst[flag_offset++] = flag;
			repeat = 0;
		}
		last_x = pts[i].x; last_y = pts[i].y; last_flag = flag;
	}
	if (repeat != 0) { if (flag_offset >= dst_size) return -1; dst[flag_offset++] = repeat; }
	if (flag_offset + x_bytes + y_bytes > dst_size) return -1;
	int xo = flag_offset, yo = flag_offset + x_bytes;
	last_x = last_y = 0;
	for (int i = 0; i < n_points; i++) {
		int dx = pts[i].x - last_x;
		if (dx == 0) {}
		else if (dx > -256 && dx < 256) dst[xo++] = dx < 0 ? -dx : dx;
		else xo = put16(dst, xo, dx);
		last_x += dx;
		int dy = pts[i].y - last_y;
		if (dy == 0) {}
		else if (dy > -256 && dy < 256) dst[yo++] = dy < 0 ? -dy : dy;
		else yo = put16(dst, yo, dy);
		last_y += dy;
	}
	return yo;
}

static void compute_bbox(int n_points, const struct point *pts, unsigned char *dst)
{
	int xmin = 0, ymin = 0, xmax = 0, ymax = 0;
	if (n_points > 0) { xmin = xmax = pts[0].x; ymin = ymax = pts[0].y; }
	for (int i = 1; i < n_points; i++) {
		if (pts[i].x < xmin) xmin = pts[i].x;
		if (pts[i].x > xmax) xmax = pts[i].x;
		if (pts[i].y < ymin) ymin = pts[i].y;
		if (pts[i].y > ymax) ymax = pts[i].y;
	}
	put16(dst, 2, xmin); put16(dst, 4, ymin); put16(dst, 6, xmax); put16(dst, 8, ymax);
}

/* Composite-glyph component flags. */
#define C_ARGS_WORDS 1
#define C_HAVE_SCALE 8
#define C_MORE       32
#define C_XY_SCALE   64
#define C_2X2        128
#define C_HAVE_INSTR 256

/* Measure one composite glyph's component bytes in `cs` (a reader), reporting
 * whether it carries instructions. Returns bytes, or -1 on overrun. */
static int size_of_composite(struct rd *cs, int *have_instr)
{
	int start = cs->pos, wi = 0, flags = C_MORE;
	while (flags & C_MORE) {
		flags = ru16(cs);
		if (cs->err) return -1;
		wi |= (flags & C_HAVE_INSTR) != 0;
		int arg = 2 + ((flags & C_ARGS_WORDS) ? 4 : 2);
		if (flags & C_HAVE_SCALE) arg += 2;
		else if (flags & C_XY_SCALE) arg += 4;
		else if (flags & C_2X2) arg += 8;
		rskip(cs, arg);
		if (cs->err) return -1;
	}
	*have_instr = wi;
	return cs->pos - start;
}

/* Reconstruct glyf + loca from the transformed table. Fills the glyf/loca (heap)
 * and the info needed for hmtx. Returns 0 on success. */
static int reconstruct_glyf(const unsigned char *data, int tlen,
                            unsigned char **glyf_out, int *glyf_len,
                            unsigned char **loca_out, int *loca_len,
                            int *num_glyphs_out, short **xmins_out)
{
	struct rd f = { data, tlen, 0, 0 };
	ru16(&f);                              /* version */
	int flags = ru16(&f);
	int has_overlap = flags & 1;
	int num_glyphs = ru16(&f);
	int index_format = ru16(&f);
	if (f.err) return -1;

	int sub_off = (2 + 7) * 4;
	if (sub_off > tlen) return -1;
	const unsigned char *sub_p[7]; int sub_n[7];
	int off = sub_off;
	for (int i = 0; i < 7; i++) {
		unsigned ss = ru32(&f);
		if (f.err || ss > (unsigned)(tlen - off)) return -1;
		sub_p[i] = data + off; sub_n[i] = (int)ss; off += (int)ss;
	}
	struct rd n_contour = { sub_p[0], sub_n[0], 0, 0 };
	struct rd n_points  = { sub_p[1], sub_n[1], 0, 0 };
	struct rd flag_s    = { sub_p[2], sub_n[2], 0, 0 };
	struct rd glyph_s   = { sub_p[3], sub_n[3], 0, 0 };
	struct rd comp_s    = { sub_p[4], sub_n[4], 0, 0 };
	struct rd bbox_s    = { sub_p[5], sub_n[5], 0, 0 };
	struct rd instr_s   = { sub_p[6], sub_n[6], 0, 0 };

	const unsigned char *overlap_bm = 0;
	if (has_overlap) {
		int obl = (num_glyphs + 7) >> 3;
		overlap_bm = data + off;
		if (obl > tlen - off) return -1;
	}

	const unsigned char *bbox_bm = bbox_s.p;
	int bitmap_len = ((num_glyphs + 31) >> 5) << 2;
	rskip(&bbox_s, bitmap_len);
	if (bbox_s.err) return -1;

	unsigned *loca_vals = malloc(sizeof(unsigned) * (num_glyphs + 1));
	short *xmins = calloc(num_glyphs > 0 ? num_glyphs : 1, sizeof(short));
	struct point *pts = 0; int pts_cap = 0;
	int gbuf_cap = 5120;
	unsigned char *gbuf = malloc(gbuf_cap);
	struct ob glyf = {0};
	if (!loca_vals || !xmins || !gbuf) goto fail;

	for (int i = 0; i < num_glyphs; i++) {
		int glyph_size = 0;
		int have_bbox = bbox_bm[i >> 3] & (0x80 >> (i & 7));
		int nc = ru16(&n_contour);
		if (n_contour.err) goto fail;

		if (nc == 0xffff) {
			/* composite */
			if (!have_bbox) goto fail;
			int have_instr = 0;
			struct rd peek = comp_s;
			int csize = size_of_composite(&peek, &have_instr);
			if (csize < 0) goto fail;
			int isize = 0;
			if (have_instr) { isize = r255(&glyph_s); if (glyph_s.err) goto fail; }
			int need = 12 + csize + isize;
			if (need > gbuf_cap) { unsigned char *ng = realloc(gbuf, need); if (!ng) goto fail; gbuf = ng; gbuf_cap = need; }
			glyph_size = put16(gbuf, 0, nc);
			for (int k = 0; k < 8; k++) gbuf[glyph_size + k] = ru8(&bbox_s);
			glyph_size += 8;
			for (int k = 0; k < csize; k++) gbuf[glyph_size + k] = ru8(&comp_s);
			glyph_size += csize;
			if (have_instr) {
				glyph_size = put16(gbuf, glyph_size, isize);
				for (int k = 0; k < isize; k++) gbuf[glyph_size + k] = ru8(&instr_s);
				glyph_size += isize;
			}
			if (comp_s.err || bbox_s.err || instr_s.err) goto fail;
		} else if (nc > 0) {
			/* simple */
			int total = 0;
			int npc[256]; int *npv = npc;
			if (nc > 256) { npv = malloc(sizeof(int) * nc); if (!npv) goto fail; }
			for (int j = 0; j < nc; j++) { npv[j] = r255(&n_points); total += npv[j]; }
			if (n_points.err) { if (npv != npc) free(npv); goto fail; }
			if (total > flag_s.len - flag_s.pos) { if (npv != npc) free(npv); goto fail; }
			const unsigned char *fb = flag_s.p + flag_s.pos;
			const unsigned char *tb = glyph_s.p + glyph_s.pos;
			int tsize = glyph_s.len - glyph_s.pos;
			if (total > pts_cap) { struct point *np = realloc(pts, sizeof(struct point) * total); if (!np) { if (npv != npc) free(npv); goto fail; } pts = np; pts_cap = total; }
			int consumed = triplet_decode(fb, tb, tsize, total, pts);
			if (consumed < 0) { if (npv != npc) free(npv); goto fail; }
			rskip(&flag_s, total); glyph_s.pos += consumed;
			int isize = r255(&glyph_s);
			if (glyph_s.err || total >= (1 << 27) || isize >= (1 << 30)) { if (npv != npc) free(npv); goto fail; }
			int need = 12 + 2 * nc + 5 * total + isize;
			if (need > gbuf_cap) { unsigned char *ng = realloc(gbuf, need); if (!ng) { if (npv != npc) free(npv); goto fail; } gbuf = ng; gbuf_cap = need; }
			put16(gbuf, 0, nc);
			if (have_bbox) { for (int k = 0; k < 8; k++) gbuf[2 + k] = ru8(&bbox_s); }
			else compute_bbox(total, pts, gbuf);
			glyph_size = ENDPTS_OFF;
			int endpt = -1;
			for (int c = 0; c < nc; c++) { endpt += npv[c]; if (endpt >= 65536) { if (npv != npc) free(npv); goto fail; } glyph_size = put16(gbuf, glyph_size, endpt); }
			glyph_size = put16(gbuf, glyph_size, isize);
			for (int k = 0; k < isize; k++) gbuf[glyph_size + k] = ru8(&instr_s);
			glyph_size += isize;
			int has_ov = overlap_bm && (overlap_bm[i >> 3] & (0x80 >> (i & 7)));
			int gs = store_points(total, pts, nc, isize, has_ov, gbuf, gbuf_cap);
			if (npv != npc) free(npv);
			if (gs < 0 || bbox_s.err || instr_s.err) goto fail;
			glyph_size = gs;
			xmins[i] = (short)s16((gbuf[2] << 8) | gbuf[3]);
		} else {
			/* empty glyph */
			if (have_bbox) goto fail;
		}

		loca_vals[i] = (unsigned)glyf.len;
		ob_put(&glyf, gbuf, glyph_size);
		ob_pad4(&glyf);
		if (glyf.oom) goto fail;
	}

	loca_vals[num_glyphs] = (unsigned)glyf.len;

	/* build loca */
	struct ob loca = {0};
	for (int i = 0; i <= num_glyphs; i++) {
		if (index_format) ob_u32(&loca, loca_vals[i]);
		else ob_u16(&loca, loca_vals[i] >> 1);
	}
	if (loca.oom) { free(loca.p); goto fail; }

	free(loca_vals); free(pts); free(gbuf);
	*glyf_out = glyf.p; *glyf_len = glyf.len;
	*loca_out = loca.p; *loca_len = loca.len;
	*num_glyphs_out = num_glyphs; *xmins_out = xmins;
	return 0;
fail:
	free(loca_vals); free(xmins); free(pts); free(gbuf); free(glyf.p);
	return -1;
}

/* Rebuild a transformed hmtx using x_mins from glyf. Returns heap buffer / len. */
static unsigned char *reconstruct_hmtx(const unsigned char *data, int tlen,
                                       int num_glyphs, int num_hmetrics,
                                       const short *xmins, int *out_len)
{
	struct rd r = { data, tlen, 0, 0 };
	int hflags = ru8(&r);
	if (r.err || (hflags & 0xFC)) return 0;
	int prop_lsb = !(hflags & 1), mono_lsb = !(hflags & 2);
	if (prop_lsb && mono_lsb) return 0;
	if (num_hmetrics < 1 || num_hmetrics > num_glyphs) return 0;

	int adv_w[1]; (void)adv_w;
	unsigned *adv = malloc(sizeof(unsigned) * num_hmetrics);
	int *lsb = malloc(sizeof(int) * num_glyphs);
	if (!adv || !lsb) { free(adv); free(lsb); return 0; }
	for (int i = 0; i < num_hmetrics; i++) adv[i] = ru16(&r);
	for (int i = 0; i < num_hmetrics; i++) lsb[i] = prop_lsb ? s16(ru16(&r)) : xmins[i];
	for (int i = num_hmetrics; i < num_glyphs; i++) lsb[i] = mono_lsb ? s16(ru16(&r)) : xmins[i];
	if (r.err) { free(adv); free(lsb); return 0; }

	int sz = 2 * num_glyphs + 2 * num_hmetrics;
	unsigned char *out = malloc(sz ? sz : 1);
	if (!out) { free(adv); free(lsb); return 0; }
	int o = 0;
	for (int i = 0; i < num_glyphs; i++) {
		if (i < num_hmetrics) o = put16(out, o, adv[i]);
		o = put16(out, o, lsb[i]);
	}
	free(adv); free(lsb);
	*out_len = sz;
	return out;
}

static unsigned char *woff2_to_sfnt(const unsigned char *data, int len, int *out_len)
{
	struct rd r = { data, len, 0, 0 };
	rskip(&r, 4);                       /* signature */
	unsigned flavor = ru32(&r);
	if (ru32(&r) != (unsigned)len) return 0;   /* length */
	int num = ru16(&r);
	rskip(&r, 6);                       /* reserved + totalSfntSize */
	unsigned comp_len = ru32(&r);
	rskip(&r, 4);                       /* major/minor version */
	rskip(&r, 12);                      /* meta off/len/origlen */
	rskip(&r, 8);                       /* priv off/len */
	if (r.err || num <= 0 || num > 256) return 0;

	/* table directory */
	struct table *t = calloc(num, sizeof *t);
	unsigned *src_off = calloc(num, sizeof(unsigned));
	unsigned *src_len = calloc(num, sizeof(unsigned));
	unsigned *tflags  = calloc(num, sizeof(unsigned));
	if (!t || !src_off || !src_len || !tflags) goto hdr_fail;
	{
		unsigned acc = 0;
		for (int i = 0; i < num; i++) {
			unsigned fb = ru8(&r);
			unsigned tag = (fb & 0x3f) == 0x3f ? ru32(&r) : KNOWN_TAGS[fb & 0x3f];
			int xform = (fb >> 6) & 3;
			int transformed = (tag == TAG_GLYF || tag == TAG_LOCA) ? (xform == 0) : (xform != 0);
			unsigned dlen = rbase128(&r);
			unsigned tlen = dlen;
			if (transformed) {
				tlen = rbase128(&r);
				if (tag == TAG_LOCA && tlen) goto hdr_fail;   /* loca xform length is 0 */
			}
			if (r.err) goto hdr_fail;
			t[i].tag = tag;
			t[i].len = (int)dlen;
			tflags[i] = transformed;
			src_off[i] = acc;
			src_len[i] = tlen;
			acc += tlen;
		}
		/* combined brotli stream follows the directory */
		if (r.pos + (long)comp_len > len) goto hdr_fail;
		int unc_len = 0;
		unsigned char *unc = brotli_decompress(data + r.pos, (int)comp_len, &unc_len);
		if (!unc || (unsigned)unc_len != acc) { free(unc); goto hdr_fail; }

		/* pass A: copy/reconstruct everything except a transformed hmtx */
		int num_glyphs = 0, num_hmetrics = 0, hmtx_idx = -1;
		short *xmins = 0;
		unsigned char *glyf_buf = 0, *loca_buf = 0; int glyf_len = 0, loca_len = 0;
		for (int i = 0; i < num; i++) {
			const unsigned char *sp = unc + src_off[i];
			if (src_off[i] + src_len[i] > acc) { free(unc); free(xmins); free(glyf_buf); free(loca_buf); goto hdr_fail; }
			if (t[i].tag == TAG_HHEA && (int)src_len[i] >= 36)
				num_hmetrics = (sp[34] << 8) | sp[35];
			if (!tflags[i]) {
				/* non-transformed: a view into unc (not owned) */
				t[i].data = (unsigned char *)sp;
				t[i].owned = 0;
				if (t[i].tag == TAG_HEAD && t[i].len >= 12) {  /* zero checkSumAdjustment */
					unsigned char *h = (unsigned char *)sp;
					h[8] = h[9] = h[10] = h[11] = 0;
				}
				t[i].checksum = table_checksum(t[i].data, t[i].len);
			} else if (t[i].tag == TAG_GLYF) {
				if (reconstruct_glyf(sp, (int)src_len[i], &glyf_buf, &glyf_len, &loca_buf, &loca_len, &num_glyphs, &xmins) != 0) { free(unc); goto hdr_fail; }
				t[i].data = glyf_buf; t[i].len = glyf_len; t[i].owned = 1;
				t[i].checksum = table_checksum(glyf_buf, glyf_len);
			} else if (t[i].tag == TAG_LOCA) {
				/* produced alongside glyf; filled in below once both exist */
				t[i].data = 0; t[i].owned = 0;
			} else if (t[i].tag == TAG_HMTX) {
				hmtx_idx = i;
			} else {
				free(unc); free(xmins); free(glyf_buf); free(loca_buf); goto hdr_fail;  /* unknown transform */
			}
		}
		/* attach loca (must follow a glyf) */
		for (int i = 0; i < num; i++) if (t[i].tag == TAG_LOCA) {
			if (!loca_buf) { free(unc); free(xmins); free(glyf_buf); goto hdr_fail; }
			t[i].data = loca_buf; t[i].len = loca_len; t[i].owned = 1; loca_buf = 0;
			t[i].checksum = table_checksum(t[i].data, t[i].len);
		}
		/* pass B: transformed hmtx */
		if (hmtx_idx >= 0) {
			int hl = 0;
			unsigned char *hm = reconstruct_hmtx(unc + src_off[hmtx_idx], (int)src_len[hmtx_idx],
			                                     num_glyphs, num_hmetrics, xmins, &hl);
			if (!hm) { free(unc); free(xmins); free(loca_buf); goto hdr_fail; }
			t[hmtx_idx].data = hm; t[hmtx_idx].len = hl; t[hmtx_idx].owned = 1;
			t[hmtx_idx].checksum = table_checksum(hm, hl);
		}
		free(xmins); free(loca_buf);

		/* assemble. Non-owned tables point into `unc`, so copy them out first so
		 * we can free `unc` — assemble frees owned buffers itself. */
		for (int i = 0; i < num; i++) if (!t[i].owned && t[i].data) {
			unsigned char *c = malloc(t[i].len ? t[i].len : 1);
			if (!c) { free(unc); goto hdr_fail; }
			for (int k = 0; k < t[i].len; k++) c[k] = t[i].data[k];
			t[i].data = c; t[i].owned = 1;
		}
		free(unc);
		unsigned char *sfnt = assemble_sfnt(flavor, t, num, out_len);
		free(t); free(src_off); free(src_len); free(tflags);
		return sfnt;
	}
hdr_fail:
	if (t) for (int i = 0; i < num; i++) if (t[i].owned) free(t[i].data);
	free(t); free(src_off); free(src_len); free(tflags);
	return 0;
}

/* ===================================================================== API */
int woff_is_woff(const unsigned char *d, int len)
{
	if (len < 4) return 0;
	unsigned sig = ((unsigned)d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
	return sig == 0x774f4646 /* wOFF */ || sig == 0x774f4632 /* wOF2 */;
}

unsigned char *woff_to_sfnt(const unsigned char *data, int len, int *out_len)
{
	if (len < 4) return 0;
	unsigned sig = ((unsigned)data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	if (sig == 0x774f4646) return woff1_to_sfnt(data, len, out_len);
	if (sig == 0x774f4632) return woff2_to_sfnt(data, len, out_len);
	return 0;
}
