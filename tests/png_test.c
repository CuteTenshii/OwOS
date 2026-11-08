/* png_test.c — host test for the streaming PNG decoder (user/lib/png/png.c).
 * Fixtures are built here with an independent encoder (all five filters, Adam7,
 * stored-block zlib, split IDATs), and every decoded pixel is checked against
 * the source samples. */
#define OWOS_SUITE_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crc32.h"
#include "png.h"

#define IL_CAP_W 512
#define IL_CAP_H 384
#define MAXSAMP (IL_CAP_W * (IL_CAP_H + 1))
#define ROWMAX 1024

struct spec {
	unsigned w, h, color, depth;
	int interlace;
	int filter;                 /* 0..4 fixed, 5 cycles per row */
	const uint16_t *s;          /* w*h*channels samples, row-major */
	unsigned plte_n, trns_n;    /* palette and its tRNS length */
	uint8_t plte[18], trns[3];
	int ckey;                   /* gray/RGB colour-key present */
	uint16_t ck[3];
};

static uint16_t samp[MAXSAMP];
static uint8_t raw[1 << 19];
static uint8_t png[1 << 20];
static size_t pn;
static uint8_t out[IL_CAP_W * (IL_CAP_H + 1) * 4];
static uint8_t out_il[sizeof out];

static unsigned channels(unsigned color)
{
	return color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
}

/* 16-bit channel 0 has a unique high byte per pixel, since the decoder keys on it. */
static uint16_t gen(unsigned i, unsigned c, unsigned depth)
{
	uint32_t h = (i * 2654435761u) ^ (c * 40503u + 12345);
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	if (depth == 16 && c == 0)
		return (uint16_t)((((i * 37 + 11) & 255) << 8) | (h & 255));
	return (uint16_t)(h & ((1u << depth) - 1));
}

static void init(struct spec *sp, unsigned color, unsigned depth, unsigned w, unsigned h, int interlace, int filter,
                 int trns)
{
	unsigned ch = channels(color);
	memset(sp, 0, sizeof *sp);
	sp->w = w; sp->h = h; sp->color = color; sp->depth = depth;
	sp->interlace = interlace; sp->filter = filter; sp->s = samp;
	for (unsigned i = 0; i < w * h; i++)
		for (unsigned c = 0; c < ch; c++)
			samp[i * ch + c] = gen(i, c, depth);
	if (color == 3) {
		sp->plte_n = (1u << depth) < 6 ? (1u << depth) : 6;
		for (unsigned i = 0; i < sp->plte_n * 3; i++)
			sp->plte[i] = (uint8_t)(i * 41 + 7);
		for (unsigned i = 0; i < w * h; i++)
			samp[i] %= sp->plte_n;
		if (trns) {
			sp->trns_n = sp->plte_n > 2 ? 3 : 1;
			sp->trns[0] = 10; sp->trns[1] = 200; sp->trns[2] = 0;
		}
	} else if (trns && (color == 0 || color == 2)) {
		sp->ckey = 1;
		for (unsigned c = 0; c < ch; c++)
			sp->ck[c] = samp[c];
	}
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(const char *ty, const void *d, uint32_t n)
{
	put32(png + pn, n);
	memcpy(png + pn + 4, ty, 4);
	if (n)
		memcpy(png + pn + 8, d, n);
	put32(png + pn + 8 + n, crc32(crc32(0, ty, 4), d, n));
	pn += 12 + n;
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void pack(const struct spec *sp, unsigned x0, unsigned dx, unsigned y, unsigned pw, uint8_t *o)
{
	unsigned ch = channels(sp->color), d = sp->depth;
	memset(o, 0, (pw * ch * d + 7) / 8);
	for (unsigned i = 0; i < pw; i++)
		for (unsigned c = 0; c < ch; c++) {
			unsigned v = sp->s[((size_t)y * sp->w + x0 + i * dx) * ch + c], k = i * ch + c;
			if (d == 16) { o[k * 2] = v >> 8; o[k * 2 + 1] = v; }
			else if (d == 8) o[k] = v;
			else o[k * d / 8] |= v << (8 - d - (k * d) % 8);
		}
}

/* Filter every (sub-)scanline into raw[]; returns the byte count. */
static size_t filtered(const struct spec *sp)
{
	static const unsigned xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
	static const unsigned xt[7] = {8, 8, 4, 4, 2, 2, 1}, yt[7] = {8, 8, 8, 4, 4, 2, 2};
	unsigned ch = channels(sp->color), bpp = (ch * sp->depth + 7) / 8;
	size_t n = 0;
	for (int p = 0; p < (sp->interlace ? 7 : 1); p++) {
		unsigned x0 = sp->interlace ? xs[p] : 0, y0 = sp->interlace ? ys[p] : 0;
		unsigned dx = sp->interlace ? xt[p] : 1, dy = sp->interlace ? yt[p] : 1;
		if (x0 >= sp->w || y0 >= sp->h)
			continue;
		unsigned pw = (sp->w - x0 + dx - 1) / dx, ph = (sp->h - y0 + dy - 1) / dy;
		unsigned stride = (pw * ch * sp->depth + 7) / 8;
		uint8_t prev[ROWMAX] = {0}, cur[ROWMAX];
		for (unsigned r = 0; r < ph; r++) {
			pack(sp, x0, dx, y0 + r * dy, pw, cur);
			unsigned ft = sp->filter < 5 ? (unsigned)sp->filter : r % 5;
			raw[n++] = ft;
			for (unsigned i = 0; i < stride; i++) {
				int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
				int pred = ft == 0 ? 0 : ft == 1 ? a : ft == 2 ? b : ft == 3 ? (a + b) / 2 : paeth(a, b, c);
				raw[n++] = (uint8_t)(cur[i] - pred);
			}
			memcpy(prev, cur, stride);
		}
	}
	return n;
}

/* Wraps data in a zlib stream of stored blocks, split across IDAT chunks. */
static void idat(const uint8_t *d, size_t n)
{
	static uint8_t z[sizeof raw + 64];
	size_t zn = 0, off = 0;
	uint32_t a = 1, b = 0;
	z[zn++] = 0x78; z[zn++] = 0x01;
	do {
		size_t len = n - off > 65535 ? 65535 : n - off;
		z[zn++] = off + len == n;
		z[zn++] = len; z[zn++] = len >> 8; z[zn++] = ~len; z[zn++] = ~len >> 8;
		memcpy(z + zn, d + off, len);
		zn += len; off += len;
	} while (off < n);
	for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
	put32(z + zn, (b << 16) | a);
	zn += 4;
	for (size_t o = 0; o < zn; o += 509)
		chunk("IDAT", z + o, zn - o > 509 ? 509 : (uint32_t)(zn - o));
}

static void encode(const struct spec *sp)
{
	uint8_t ih[13] = {0}, tr[6];
	put32(ih, sp->w); put32(ih + 4, sp->h);
	ih[8] = sp->depth; ih[9] = sp->color; ih[12] = sp->interlace;
	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	pn = 8;
	chunk("IHDR", ih, 13);
	if (sp->color == 3) {
		chunk("PLTE", sp->plte, sp->plte_n * 3);
		if (sp->trns_n)
			chunk("tRNS", sp->trns, sp->trns_n);
	} else if (sp->ckey) {
		unsigned n = channels(sp->color);
		for (unsigned c = 0; c < n; c++) { tr[c * 2] = sp->ck[c] >> 8; tr[c * 2 + 1] = sp->ck[c]; }
		chunk("tRNS", tr, n * 2);
	}
	idat(raw, filtered(sp));
	chunk("IEND", 0, 0);
}

static int sc(unsigned v, unsigned depth)
{
	return depth == 16 ? v >> 8 : depth == 8 ? (int)v : (int)(v * 255 / ((1u << depth) - 1));
}

static void want_px(const struct spec *sp, unsigned x, unsigned y, uint8_t o[4])
{
	unsigned ch = channels(sp->color), d = sp->depth;
	const uint16_t *s = sp->s + ((size_t)y * sp->w + x) * ch;
	o[3] = 255;
	switch (sp->color) {
	case 0:
		o[0] = o[1] = o[2] = sc(s[0], d);
		if (sp->ckey && s[0] == sp->ck[0]) o[3] = 0;
		break;
	case 2:
		for (int c = 0; c < 3; c++) o[c] = sc(s[c], d);
		if (sp->ckey && s[0] == sp->ck[0] && s[1] == sp->ck[1] && s[2] == sp->ck[2]) o[3] = 0;
		break;
	case 3:
		for (int c = 0; c < 3; c++) o[c] = sp->plte[s[0] * 3 + c];
		if (s[0] < sp->trns_n) o[3] = sp->trns[s[0]];
		break;
	case 4:
		o[0] = o[1] = o[2] = sc(s[0], d);
		o[3] = sc(s[1], d);
		break;
	default:
		for (int c = 0; c < 4; c++) o[c] = sc(s[c], d);
	}
}

struct mem { const uint8_t *p; size_t n, i; };
static int mem_rd(void *ctx)
{
	struct mem *m = ctx;
	return m->i < m->n ? m->p[m->i++] : -1;
}

struct sink { uint8_t *buf; unsigned rows, next_y, bad_order, stop_at; };
static int row_cb(void *ctx, unsigned y, const unsigned char *rgba, unsigned w)
{
	struct sink *k = ctx;
	if (y != k->next_y++)
		k->bad_order++;
	memcpy(k->buf + (size_t)y * w * 4, rgba, w * 4);
	return ++k->rows == k->stop_at ? -1 : 0;
}

/* Decode the first n bytes of png[]. */
static int decode_buf(size_t n, uint8_t *dst, struct sink *k, unsigned *w, unsigned *h)
{
	struct mem m = {png, n, 0};
	memset(k, 0, sizeof *k);
	k->buf = dst;
	return png_decode(mem_rd, &m, row_cb, k, w, h);
}

static int decode(const struct spec *sp, uint8_t *dst)
{
	struct sink k;
	unsigned w = 0, h = 0;
	encode(sp);
	int r = decode_buf(pn, dst, &k, &w, &h);
	expect_eq(w, sp->w);
	expect_eq(h, sp->h);
	expect_eq(k.rows, sp->h);
	expect_eq(k.bad_order, 0);
	return r;
}

static unsigned mismatches(const struct spec *sp, const uint8_t *got)
{
	unsigned bad = 0;
	for (unsigned y = 0; y < sp->h; y++)
		for (unsigned x = 0; x < sp->w; x++) {
			uint8_t w4[4];
			const uint8_t *g = got + ((size_t)y * sp->w + x) * 4;
			want_px(sp, x, y, w4);
			if (memcmp(g, w4, 4) && !bad++)
				fprintf(stderr, "color=%u depth=%u il=%d filter=%d %ux%u pixel (%u,%u): got %02x%02x%02x%02x want %02x%02x%02x%02x\n",
				        sp->color, sp->depth, sp->interlace, sp->filter, sp->w, sp->h, x, y,
				        g[0], g[1], g[2], g[3], w4[0], w4[1], w4[2], w4[3]);
		}
	return bad;
}

static const struct { unsigned color, depth; } FORMATS[] = {
	{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1}, {3, 2}, {3, 4}, {3, 8},
	{4, 8}, {4, 16}, {6, 8}, {6, 16},
};
#define NFORMATS (sizeof FORMATS / sizeof FORMATS[0])

/* Decode both layouts of one image; each must match the source and each other. */
static void check_pair(unsigned color, unsigned depth, unsigned w, unsigned h, int filter, int trns)
{
	struct spec sp;
	init(&sp, color, depth, w, h, 0, filter, trns);
	expect_eq(decode(&sp, out), PNG_OK);
	expect_eq(mismatches(&sp, out), 0);
	sp.interlace = 1;
	expect_eq(decode(&sp, out_il), PNG_OK);
	expect_eq(mismatches(&sp, out_il), 0);
	expect(!memcmp(out, out_il, (size_t)w * h * 4));
}

int main(void)
{
	test("every colour type and depth, plain and Adam7");
	for (unsigned f = 0; f < NFORMATS; f++)
		check_pair(FORMATS[f].color, FORMATS[f].depth, 13, 11, 5, 0);

	test("palette tRNS and colour-key transparency");
	for (unsigned f = 0; f < NFORMATS; f++) {
		unsigned c = FORMATS[f].color;
		if (c == 0 || c == 2 || c == 3)
			check_pair(c, FORMATS[f].depth, 13, 11, 5, 1);
	}

	test("each filter type");
	for (int ft = 0; ft < 5; ft++) {
		check_pair(0, 1, 13, 11, ft, 0);
		check_pair(2, 8, 13, 11, ft, 0);
		check_pair(6, 16, 13, 11, ft, 0);
	}

	test("small and degenerate sizes");
	static const unsigned dims[][2] = {{1, 1}, {2, 3}, {5, 1}, {1, 9}, {3, 3}, {9, 2}};
	for (unsigned i = 0; i < sizeof dims / sizeof dims[0]; i++) {
		check_pair(6, 8, dims[i][0], dims[i][1], 5, 0);
		check_pair(0, 2, dims[i][0], dims[i][1], 5, 0);
	}

	test("literal pixel values");
	{
		struct spec sp;
		static const uint16_t rgba16[8] = {0x1234, 0x5678, 0x9ABC, 0xDEF0, 0xFFFF, 0, 0x00FF, 0x8000};
		static const uint8_t want16[8] = {0x12, 0x56, 0x9A, 0xDE, 0xFF, 0, 0, 0x80};
		init(&sp, 6, 16, 2, 1, 0, 0, 0);
		sp.s = rgba16;
		expect_eq(decode(&sp, out), PNG_OK);
		expect(!memcmp(out, want16, 8));

		static const uint16_t bits[8] = {1, 0, 1, 1, 0, 0, 0, 1};
		static const uint8_t wantg[32] = {255,255,255,255, 0,0,0,255, 255,255,255,255, 255,255,255,255,
		                                  0,0,0,255, 0,0,0,255, 0,0,0,255, 255,255,255,255};
		init(&sp, 0, 1, 8, 1, 1, 5, 0);
		sp.s = bits;
		expect_eq(decode(&sp, out), PNG_OK);
		expect(!memcmp(out, wantg, 32));

		static const uint16_t idx[3] = {0, 1, 2};
		init(&sp, 3, 8, 3, 1, 0, 1, 1);
		sp.s = idx;
		expect_eq(decode(&sp, out), PNG_OK);
		expect_eq(out[3], 10);
		expect_eq(out[7], 200);
		expect_eq(out[11], 0);
		expect_eq(out[4], sp.plte[3]);

		static const uint16_t gray[3] = {0x40, 0x41, 0x40};
		init(&sp, 0, 8, 3, 1, 1, 0, 0);
		sp.s = gray;
		sp.ckey = 1; sp.ck[0] = 0x40;
		expect_eq(decode(&sp, out), PNG_OK);
		expect_eq(out[3], 0);
		expect_eq(out[7], 255);
		expect_eq(out[11], 0);
		expect_eq(out[0], 0x40);
	}

	test("interlaced alpha is preserved");
	{
		struct spec sp;
		init(&sp, 6, 8, 13, 11, 1, 5, 0);
		expect_eq(decode(&sp, out), PNG_OK);
		unsigned translucent = 0;
		for (unsigned i = 0; i < 13 * 11; i++)
			translucent += out[i * 4 + 3] != 255;
		expect(translucent > 100);
		expect_eq(mismatches(&sp, out), 0);
	}

	test("interlaced pixel cap");
	{
		struct spec sp;
		struct sink k;
		init(&sp, 0, 8, IL_CAP_W, IL_CAP_H, 1, 5, 0);
		expect_eq(decode(&sp, out), PNG_OK);
		expect_eq(mismatches(&sp, out), 0);

		init(&sp, 0, 8, IL_CAP_W, IL_CAP_H + 1, 1, 5, 0);
		encode(&sp);
		expect_eq(decode_buf(pn, out, &k, 0, 0), PNG_EUNSUP);
		expect_eq(k.rows, 0);

		sp.interlace = 0;
		expect_eq(decode(&sp, out), PNG_OK);
		expect_eq(mismatches(&sp, out), 0);
	}

	test("errors");
	{
		struct spec sp;
		struct sink k;
		init(&sp, 2, 8, 13, 11, 0, 5, 0);
		encode(&sp);
		size_t full = pn;

		png[0] ^= 1;
		expect_eq(decode_buf(full, out, &k, 0, 0), PNG_EBADSIG);
		png[0] ^= 1;

		expect_eq(decode_buf(full / 2, out, &k, 0, 0), PNG_EINFLATE);
		expect(k.rows < 11);
		expect_eq(decode_buf(30, out, &k, 0, 0), PNG_ETRUNC);
		expect_eq(decode_buf(4, out, &k, 0, 0), PNG_EBADSIG);

		init(&sp, 2, 4, 3, 3, 0, 0, 0);
		encode(&sp);
		expect_eq(decode_buf(pn, out, &k, 0, 0), PNG_EUNSUP);

		init(&sp, 6, 8, 3, 3, 0, 0, 0);
		encode(&sp);
		put32(png + 16, 0);
		expect_eq(decode_buf(pn, out, &k, 0, 0), PNG_EBADHDR);

		init(&sp, 0, 8, 4, 2, 0, 0, 0);
		encode(&sp);
		png[48] = 9; /* first scanline's filter byte, after the zlib and stored-block headers */
		expect_eq(decode_buf(pn, out, &k, 0, 0), PNG_EINFLATE);
	}

	test("caller can stop early");
	for (int il = 0; il < 2; il++) {
		struct spec sp;
		struct sink k;
		init(&sp, 6, 8, 13, 11, il, 5, 0);
		encode(&sp);
		struct mem m = {png, pn, 0};
		memset(&k, 0, sizeof k);
		k.buf = out;
		k.stop_at = 4;
		expect_eq(png_decode(mem_rd, &m, row_cb, &k, 0, 0), PNG_OK);
		expect_eq(k.rows, 4);
	}

	test("dimensions peek");
	{
		struct spec sp;
		unsigned w = 0, h = 0;
		init(&sp, 6, 8, 13, 11, 1, 0, 0);
		encode(&sp);
		struct mem m = {png, pn, 0};
		expect_eq(png_dimensions(mem_rd, &m, &w, &h), PNG_OK);
		expect_eq(w, 13);
		expect_eq(h, 11);
	}
	return 0;
}
