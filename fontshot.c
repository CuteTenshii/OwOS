/* Host preview of the browser's UTF-8 body rendering: decodes UTF-8 a codepoint
 * at a time and draws each glyph through user/lib/font (font_glyph_index +
 * font_render_glyph) with the same alpha blend main.c uses. Output: a PPM. */
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW   840
#define CH   430
static unsigned char canvas[CH][CW][3];

static void fill(unsigned char r, unsigned char g, unsigned char b)
{
	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++) {
			canvas[y][x][0] = r; canvas[y][x][1] = g; canvas[y][x][2] = b;
		}
}

/* Same decoder as user/lib (utf8_decode), inlined so the harness needs no syscalls. */
static int u8(const char *s, const char *end, unsigned *cp)
{
	unsigned char c = (unsigned char)s[0];
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xE0) == 0xC0 && s + 1 < end && (s[1] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
	if ((c & 0xF0) == 0xE0 && s + 2 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
	if ((c & 0xF8) == 0xF0 && s + 3 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
	*cp = 0xFFFD; return 1;
}

static void blit(int penx, int baseline, const unsigned char *bm, int w, int h,
                 int ox, int oy, unsigned fr, unsigned fg_, unsigned fb_)
{
	if (!bm) return;
	for (int gy = 0; gy < h; gy++) {
		int py = baseline + oy + gy;
		if (py < 0 || py >= CH) continue;
		for (int gx = 0; gx < w; gx++) {
			int px = penx + ox + gx;
			if (px < 0 || px >= CW) continue;
			unsigned a = bm[gy * w + gx];
			if (!a) continue;
			unsigned ia = 255 - a;
			canvas[py][px][0] = (fr  * a + canvas[py][px][0] * ia) / 255;
			canvas[py][px][1] = (fg_ * a + canvas[py][px][1] * ia) / 255;
			canvas[py][px][2] = (fb_ * a + canvas[py][px][2] * ia) / 255;
		}
	}
}

/* Box for a codepoint the font lacks (the browser's tofu). */
static void tofu(int x, int top, int w, unsigned r, unsigned g, unsigned b)
{
	int h = 13;
	for (int i = 0; i < w - 3; i++) {
		if (top+2 < CH){canvas[top+2][x+1+i][0]=r;canvas[top+2][x+1+i][1]=g;canvas[top+2][x+1+i][2]=b;}
		if (top+2+h < CH){canvas[top+2+h][x+1+i][0]=r;canvas[top+2+h][x+1+i][1]=g;canvas[top+2+h][x+1+i][2]=b;}
	}
	for (int i = 0; i < h; i++) {
		if (top+2+i<CH){canvas[top+2+i][x+1][0]=r;canvas[top+2+i][x+1][1]=g;canvas[top+2+i][x+1][2]=b;
			canvas[top+2+i][x+1+w-4][0]=r;canvas[top+2+i][x+1+w-4][1]=g;canvas[top+2+i][x+1+w-4][2]=b;}
	}
}

static int draw(struct font *f, int x, int line_top, float px, const char *s,
                unsigned fr, unsigned fg_, unsigned fb_, int bold)
{
	int asc = 0;
	font_vmetrics(f, px, &asc, 0, 0);
	const char *end = s + strlen(s);
	while (s < end) {
		unsigned cp;
		s += u8(s, end, &cp);
		int gi = font_glyph_index(f, (int)cp);
		int w, h, ox, oy, adv;
		unsigned char *bm = font_render_glyph(f, gi, px, &w, &h, &ox, &oy, &adv);
		if (bm) {
			blit(x, line_top + asc, bm, w, h, ox, oy, fr, fg_, fb_);
			if (bold) blit(x + 1, line_top + asc, bm, w, h, ox, oy, fr, fg_, fb_);
		} else if (cp > 0x20 && gi == 0) {
			tofu(x, line_top, adv, fr, fg_, fb_);
		}
		free(bm);
		x += adv;
	}
	return x;
}

static unsigned char *slurp(const char *path, int *n)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) { perror(path); exit(1); }
	fseek(fp, 0, SEEK_END); long L = ftell(fp); fseek(fp, 0, SEEK_SET);
	unsigned char *b = malloc(L);
	if (fread(b, 1, L, fp) != (size_t)L) exit(1);
	fclose(fp); *n = (int)L; return b;
}

int main(void)
{
	fill(0xff, 0xff, 0xff);

	int n1, n2;
	unsigned char *d1 = slurp("/usr/share/fonts/noto/NotoSans-Regular.ttf", &n1);
	unsigned char *d2 = slurp("/usr/share/fonts/noto/NotoSans-Bold.ttf", &n2);
	struct font *reg = font_load(d1, n1);
	struct font *bld = font_load(d2, n2);
	if (!reg || !bld) { fprintf(stderr, "font_load failed\n"); return 1; }

	draw(bld, 24, 22, 22.0f, "OwOS Browser \xE2\x80\x94 UTF-8 / non-Latin text", 0x14,0x14,0x18, 1);

	int y = 70;
	draw(reg, 24, y, 16.0f, "Latin + accents:  caf\xC3\xA9, na\xC3\xAFve, K\xC3\xB6ln, r\xC3\xA9sum\xC3\xA9, \xC3\x86r\xC3\xB8, \xC3\x9Fharp", 0x22,0x22,0x26, 0); y += 28;
	draw(reg, 24, y, 16.0f, "Greek:  \xCE\x93\xCE\xB5\xCE\xB9\xCE\xB1 \xCF\x83\xCE\xBF\xCF\x85, \xCE\xBA\xCF\x8C\xCF\x83\xCE\xBC\xCE\xB5! \xCE\x91\xCE\xB2\xCE\xB3\xCE\xB4\xCE\xB5\xCE\xB6\xCE\xB7\xCE\xB8", 0x22,0x22,0x26, 0); y += 28;
	draw(reg, 24, y, 16.0f, "Cyrillic:  \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80! \xD0\x97\xD0\xB4\xD1\x80\xD0\xB0\xD0\xB2\xD1\x81\xD1\x82\xD0\xB2\xD1\x83\xD0\xB9", 0x22,0x22,0x26, 0); y += 28;
	draw(reg, 24, y, 16.0f, "Punctuation + symbols:  \xE2\x80\x9C""curly\xE2\x80\x9D \xE2\x80\x93 dash \xE2\x80\xA6 \xE2\x82\xAC\xC2\xA3\xC2\xA5 \xC2\xBD \xC2\xA9 \xC2\xAE \xE2\x84\xA2", 0x22,0x22,0x26, 0); y += 28;
	draw(reg, 24, y, 16.0f, "Mixed in one run:  The \xC2\xA5 r\xC3\xB6se \xCE\xBA\xCE\xB1\xCE\xB9 \xD0\xBC\xD0\xB8\xD1\x80 \xE2\x80\x94 50\xC2\xA0\xE2\x82\xAC, caf\xC3\xA9.", 0x22,0x22,0x26, 0); y += 40;

	draw(bld, 24, y, 16.0f, "Missing glyph (incl. emoji) -> tofu box, never mojibake:", 0x14,0x14,0x18, 1); y += 28;
	draw(reg, 24, y, 16.0f, "emoji \xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 and CJK \xE4\xBD\xA0\xE5\xA5\xBD are boxes here (this font lacks them)", 0xa0,0x30,0x30, 0); y += 40;

	draw(reg, 24, y, 13.0f, "Decoded per codepoint via user/lib utf8_decode, drawn by user/lib/font.", 0x66,0x66,0x6c, 0);

	FILE *o = fopen("fontshot.ppm", "wb");
	fprintf(o, "P6\n%d %d\n255\n", CW, CH);
	fwrite(canvas, 1, sizeof canvas, o);
	fclose(o);
	fprintf(stderr, "wrote fontshot.ppm\n");
	return 0;
}
