/* genfont.c — offline atlas baker for OwOS's antialiased console font.
 *
 * HOST tool, not part of the kernel. Links against the system FreeType to
 * rasterise a TrueType monospace face into an 8-bit coverage atlas the kernel
 * blends at runtime (see fb_glyph in kernel/fb.c). Output is a committed C
 * header (kernel/font_aa.h); the kernel build does NOT need FreeType.
 *
 *   cc -O2 tools/genfont.c -o /tmp/genfont $(pkg-config --cflags --libs freetype2)
 *   /tmp/genfont <font.ttf> <ppem> [cell_w cell_h] > out.h
 *
 * The kernel console uses a 9x18 cell (fb.h); the ring-3 GUI library keeps the
 * legacy 8x16 cell so app layouts are untouched:
 *   /tmp/genfont .../Hack-Regular.ttf 15       > kernel/font_aa.h     (9x18)
 *   /tmp/genfont .../Hack-Regular.ttf 13 8 16   > user/lib/font_aa.h   (8x16)
 *
 * For each of the 256 CP437 codepoints we emit one cell of coverage bytes
 * (0 = bg, 255 = fg). Codepoints we deliberately keep crisp — C0 controls, DEL,
 * and the box-drawing/block range 0xB0..0xDF (TUI borders must tile and stay
 * connected, which AA softens) — plus any glyph the face lacks, fall back to the
 * legacy 8x16 bitmap so the glyph repertoire never regresses. */

#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Output cell, chosen at runtime; these bound the static atlas buffer. */
#define MAX_W 16
#define MAX_H 32
static int FONT_W = 9, FONT_H = 18;

/* The legacy bitmap, used as the fallback source. */
#include "../kernel/font8x16.h"
#define BMP_W 8
#define BMP_H 16

/* CP437 -> Unicode. Entries that are 0 force the bitmap fallback (the C0/DEL
 * pseudo-glyphs have no sensible Unicode form in a text face). */
static const unsigned short cp437[256] = {
	/* 0x00-0x1F: control range, drawn from the bitmap */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 0x20-0x7E: ASCII identity */
	0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
	0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
	0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
	0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
	0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
	0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,
	0,        /* 0x7F DEL -> bitmap */
	/* 0x80-0x8F */
	0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
	0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
	/* 0x90-0x9F */
	0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
	0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
	/* 0xA0-0xAF */
	0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
	0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
	/* 0xB0-0xDF: box-drawing & block elements -> bitmap (kept crisp) */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 0xE0-0xEF */
	0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
	0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
	/* 0xF0-0xFF */
	0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
	0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

static unsigned char atlas[256][MAX_H][MAX_W];

/* Expand the legacy 8x16 bitmap for code `c` into its cell, left-aligned and
 * vertically centred (the same pixels as before, placed in a taller cell when
 * FONT_H > 16). MSB of each byte is the leftmost pixel. */
static void fallback(int c)
{
	int yoff = (FONT_H - BMP_H) / 2;   /* 1 for 18 vs 16, 0 for 16 */
	for (int row = 0; row < BMP_H; row++) {
		unsigned char bits = font8x16[c][row];
		int y = yoff + row;
		if (y < 0 || y >= FONT_H)
			continue;
		for (int col = 0; col < BMP_W && col < FONT_W; col++)
			atlas[c][y][col] = (bits & (0x80 >> col)) ? 255 : 0;
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <font.ttf> <ppem> [cell_w cell_h]\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	int ppem = atoi(argv[2]);
	if (argc >= 5) { FONT_W = atoi(argv[3]); FONT_H = atoi(argv[4]); }
	const char *name = argc >= 6 ? argv[5] : "font_aa";
	int prop = argc >= 7 && !strcmp(argv[6], "prop");  /* emit per-glyph advances */
	if (FONT_W < 1 || FONT_W > MAX_W || FONT_H < 1 || FONT_H > MAX_H) {
		fprintf(stderr, "cell %dx%d out of range (max %dx%d)\n",
		        FONT_W, FONT_H, MAX_W, MAX_H);
		return 2;
	}

	FT_Library lib;
	FT_Face face;
	if (FT_Init_FreeType(&lib)) { fprintf(stderr, "FT init failed\n"); return 1; }
	if (FT_New_Face(lib, path, 0, &face)) {
		fprintf(stderr, "cannot open %s\n", path); return 1;
	}
	if (FT_Set_Pixel_Sizes(face, 0, ppem)) {
		fprintf(stderr, "set size %d failed\n", ppem); return 1;
	}

	int asc  = face->size->metrics.ascender  >> 6;   /* 26.6 -> px */
	int desc = -(face->size->metrics.descender >> 6);
	int lineh = asc + desc;
	int top_pad = (FONT_H - lineh) / 2;
	if (top_pad < 0) top_pad = 0;
	int baseline = top_pad + asc;

	int adv[256];
	int aa = 0, bmp = 0, clipped = 0;
	for (int c = 0; c < 256; c++) {
		unsigned short u = cp437[c];
		FT_UInt gi = u ? FT_Get_Char_Index(face, u) : 0;
		if (!gi) { fallback(c); adv[c] = BMP_W; bmp++; continue; }

		if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) ||
		    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
			fallback(c); adv[c] = BMP_W; bmp++; continue;
		}
		FT_GlyphSlot g = face->glyph;
		FT_Bitmap *bm = &g->bitmap;
		int x0 = g->bitmap_left;
		if (x0 < 0) x0 = 0;            /* keep ink inside the cell for left-bearing glyphs */
		int y0 = baseline - g->bitmap_top;
		for (int ry = 0; ry < (int)bm->rows; ry++) {
			int y = y0 + ry;
			if (y < 0 || y >= FONT_H) { if (bm->width) clipped++; continue; }
			const unsigned char *src = bm->buffer + ry * bm->pitch;
			for (int rx = 0; rx < (int)bm->width; rx++) {
				int x = x0 + rx;
				if (x < 0 || x >= FONT_W) { if (src[rx]) clipped++; continue; }
				atlas[c][y][x] = src[rx];
			}
		}
		/* Pen advance for proportional layout (26.6 -> px, rounded). The fixed
		 * grid ignores this; proportional renderers use the emitted table. */
		adv[c] = (g->advance.x + 32) >> 6;
		if (adv[c] < 1) adv[c] = 1;
		aa++;
	}

	fprintf(stderr, "genfont: %s @ %dpx -> %dx%d cell  (asc=%d desc=%d baseline=%d)\n",
	        path, ppem, FONT_W, FONT_H, asc, desc, baseline);
	fprintf(stderr, "genfont: %d AA glyphs, %d bitmap fallbacks, %d clipped pixels\n",
	        aa, bmp, clipped);

	/* Header guard from the array name, upper-cased: e.g. font_ui -> OWOS_FONT_UI_H */
	char guard[64];
	int gi2 = 0;
	guard[gi2++] = 'O'; guard[gi2++] = 'W'; guard[gi2++] = 'O'; guard[gi2++] = 'S'; guard[gi2++] = '_';
	for (const char *p = name; *p && gi2 < 60; p++)
		guard[gi2++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
	guard[gi2++] = '_'; guard[gi2++] = 'H'; guard[gi2] = 0;

	/* Emit the header. */
	printf("/* %s.h - antialiased %dx%d %s atlas, 8-bit coverage per pixel\n",
	       name, FONT_W, FONT_H, prop ? "proportional" : "monospace");
	printf(" * (0 = background, 255 = foreground). GENERATED by tools/genfont.c from\n");
	printf(" * %s at %dpx; do not edit by hand. CP437 order; controls and glyphs the\n", path, ppem);
	printf(" * face lacks fall back to the crisp 8x16 bitmap.%s */\n",
	       prop ? "  *_adv[] is the per-glyph pen advance." : "");
	printf("#ifndef %s\n#define %s\n", guard, guard);
	printf("static const unsigned char %s[256][%d][%d] = {\n", name, FONT_H, FONT_W);
	for (int c = 0; c < 256; c++) {
		printf("  {/*%3d*/", c);
		for (int y = 0; y < FONT_H; y++) {
			printf("{");
			for (int x = 0; x < FONT_W; x++)
				printf("%d%s", atlas[c][y][x], x + 1 < FONT_W ? "," : "");
			printf("}%s", y + 1 < FONT_H ? "," : "");
		}
		printf("},\n");
	}
	printf("};\n");
	if (prop) {
		printf("static const unsigned char %s_adv[256] = {\n  ", name);
		for (int c = 0; c < 256; c++)
			printf("%d,%s", adv[c], (c & 15) == 15 ? "\n  " : "");
		printf("\n};\n");
	}
	printf("#endif\n");

	FT_Done_Face(face);
	FT_Done_FreeType(lib);
	return 0;
}
