/* Host test for the font library (user/lib/font): family names, CFF (OpenType PostScript) outlines on a
 * font built in this file, WOFF2 decoding, and CBDT colour bitmaps. */
#define OWOS_SUITE_TEST
#include "font.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void family_of(const char *path, char *out, int n)
{
  FILE *fp = fopen(path, "rb");
  out[0] = '\0';
  expect(fp != NULL);
  if (!fp)
    return;
  static unsigned char buf[1 << 20];
  int len = (int)fread(buf, 1, sizeof buf, fp);
  fclose(fp);
  struct font *f = font_load(buf, len);
  expect(f != NULL);
  if (f) {
    font_family(f, out, n);
    font_free(f);
  }
}

static void sans(void)
{
  char fam[64];
  family_of("fonts/LiberationSans-Regular.ttf", fam, sizeof fam);
  expect_string(fam, "Liberation Sans");
}

static void mono(void)
{
  char fam[64];
  family_of("fonts/LiberationMono-Regular.ttf", fam, sizeof fam);
  expect_string(fam, "Liberation Mono");
}

static void truncates(void)
{
  char fam[6];
  family_of("fonts/LiberationSans-Regular.ttf", fam, sizeof fam);
  expect_string(fam, "Liber");
}

static void woff2(void)
{
  char fam[64];
  family_of("fonts/LiberationSans-Regular.woff2", fam, sizeof fam);
  expect_string(fam, "Liberation Sans");
}

static unsigned char *slurp(const char *path, int *len)
{
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return 0;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)n);
  if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
    free(b);
    b = 0;
  }
  fclose(fp);
  *len = (int)n;
  return b;
}

/* A CBDT-only font loads, maps U+1F33A to a PNG bitmap, and rasterizes nothing as an outline. */
static void color_emoji(void)
{
  int len = 0;
  unsigned char *b = slurp("fonts/NotoColorEmoji.ttf", &len);
  expect(b != NULL);
  if (!b)
    return;
  struct font *f = font_load(b, len);
  expect(f != NULL);
  if (f) {
    expect(font_has_color(f));
    int gi = font_glyph_index(f, 0x1F33A);
    expect(gi > 0);
    struct font_png p;
    expect(font_glyph_png(f, gi, 20, &p) == 1);
    expect(p.ppem == 109 && p.w > 0 && p.h > 0 && p.adv > 0 && p.len > 8);
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    expect(memcmp(p.png, sig, 8) == 0);
    expect(p.png + p.len <= b + len);
    int w, h, ox, oy, adv;
    expect(font_render_glyph(f, gi, 20, &w, &h, &ox, &oy, &adv) == NULL);
    expect(adv > 0);
    expect(font_glyph_png(f, 0, 20, &p) == 0);
    font_free(f);
  }
  free(b);
}

/* GSUB ligatures: regional indicators F+R become one flag glyph and a ZWJ family one glyph; plain text has none. */
static void emoji_ligatures(void)
{
  int len = 0;
  unsigned char *b = slurp("fonts/NotoColorEmoji.ttf", &len);
  expect(b != NULL);
  if (!b)
    return;
  struct font *f = font_load(b, len);
  expect(f != NULL);
  if (f) {
    int fr[2] = {font_glyph_index(f, 0x1F1EB), font_glyph_index(f, 0x1F1F7)}, used = 0;
    expect(fr[0] > 0 && fr[1] > 0);
    int flag = font_ligature(f, fr, 2, &used);
    expect(flag > 0 && flag != fr[0] && flag != fr[1]);
    expect_eq(used, 2);
    expect(font_glyph_png(f, flag, 20, &(struct font_png){0}) == 1);

    int fam[5] = {font_glyph_index(f, 0x1F468), font_glyph_index(f, 0x200D), font_glyph_index(f, 0x1F469),
                  font_glyph_index(f, 0x200D), font_glyph_index(f, 0x1F467)};
    expect(font_ligature(f, fam, 5, &used) > 0);
    expect_eq(used, 5);

    int lone[1] = {fr[0]};
    expect(font_ligature(f, lone, 1, &used) < 0); /* one indicator alone is not a flag */
    expect_eq(used, 0);
    font_free(f);
  }
  free(b);
}

/* An outline font has no colour glyphs. */
static void no_color_in_outline_font(void)
{
  int len = 0;
  unsigned char *b = slurp("fonts/LiberationSans-Regular.ttf", &len);
  expect(b != NULL);
  struct font *f = b ? font_load(b, len) : 0;
  expect(f != NULL);
  if (f) {
    struct font_png p;
    expect(!font_has_color(f));
    expect(font_glyph_png(f, font_glyph_index(f, 'A'), 15, &p) == 0);
    font_free(f);
  }
  free(b);
}

/* Each shipped Noto Sans face names its family and reports the weight and slant the font store matches on. */
static void noto_sans_faces(void)
{
  static const struct {
    const char *path;
    int weight, italic;
  } faces[] = {{"fonts/NotoSans-Regular.ttf", 400, 0},
               {"fonts/NotoSans-Bold.ttf", 700, 0},
               {"fonts/NotoSans-Italic.ttf", 400, 1},
               {"fonts/NotoSans-BoldItalic.ttf", 700, 1}};
  for (unsigned i = 0; i < sizeof faces / sizeof faces[0]; i++) {
    int len = 0;
    unsigned char *b = slurp(faces[i].path, &len);
    expect(b != NULL);
    struct font *f = b ? font_load(b, len) : 0;
    expect(f != NULL);
    if (f) {
      char fam[64];
      int weight, italic;
      font_family(f, fam, sizeof fam);
      expect_string(fam, "Noto Sans");
      font_style(f, &weight, &italic);
      expect_eq(weight, faces[i].weight);
      expect_eq(italic, faces[i].italic);
      font_free(f);
    }
    free(b);
  }
}

/* The CJK fallback is a CID-keyed CFF font: it covers Han, kana and Hangul, which the Liberation text faces lack,
 * and draws them as outlines. */
static void cjk_fallback(void)
{
  static const unsigned cps[] = {0x6F22 /* 漢 */, 0x3042 /* あ */, 0x30AB /* カ */, 0xD55C /* 한 */, 0xFF01 /* ！ */};
  int len = 0, tlen = 0;
  unsigned char *b = slurp("fonts/NotoSansCJKsc-Regular.otf", &len);
  unsigned char *tb = slurp("fonts/LiberationSans-Regular.ttf", &tlen);
  expect(b != NULL && tb != NULL);
  struct font *f = b ? font_load(b, len) : 0, *text = tb ? font_load(tb, tlen) : 0;
  expect(f != NULL && text != NULL);
  if (f && text) {
    char fam[64];
    font_family(f, fam, sizeof fam);
    expect_string(fam, "Noto Sans CJK SC");
    expect(!font_has_color(f));
    for (unsigned i = 0; i < sizeof cps / sizeof cps[0]; i++) {
      expect_eq(font_glyph_index(text, (int)cps[i]), 0);
      int gi = font_glyph_index(f, (int)cps[i]);
      expect(gi > 0);
      int w, h, ox, oy, adv, ink = 0;
      unsigned char *bm = font_render_glyph(f, gi, 16, &w, &h, &ox, &oy, &adv);
      expect(bm != NULL && w > 0 && h > 8 && oy < 0);
      expect(adv >= 14 && adv <= 17); /* full-width: one em */
      for (int p = 0; bm && p < w * h; p++)
        ink += bm[p] > 128;
      expect(ink > 6);
      free(bm);
    }
  }
  font_free(f);
  font_free(text);
  free(b);
  free(tb);
}

static void put16(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)v;
}

/* An 'OTTO' font whose glyph 1 (U+0041) is a 600x600 square at (100,100) in a 1000-unit em. Writes it to
 * `out` and returns its size; `cs_off` is added to the Top DICT's CharStrings offset. */
static int build_cff_font(unsigned char *out, unsigned cs_off)
{
  static const unsigned char charstrings[] = {
      0,   2,   1,  1,  2, 16,                    /* count 2, offSize 1, offsets 1 2 16 */
      14,                                         /* .notdef: endchar */
      239, 239, 21,                               /* 100 100 rmoveto */
      28,  2,   88, 28, 2, 88, 28, 0xfd, 0xa8, 6, /* 600 600 -600 hlineto */
      14,
  };
  unsigned char cff[128], head[54] = {0}, hhea[36] = {0}, maxp[6] = {0, 0, 0x50, 0, 0, 2};
  unsigned char hmtx[8] = {0x01, 0xf4, 0, 0, 0x01, 0xf4, 0, 0};
  unsigned char cmap[44] = {0, 0, 0, 2,               /* two encodings */
                            0, 3, 0, 1,  0, 0, 0, 20, /* (3,1): U+0041 -> glyph 1 */
                            0, 1, 0, 0,  0, 0, 0, 32, /* (1,0): Mac code 0x80 -> glyph 1 */
                            0, 6, 0, 12, 0, 0, 0, 65, 0, 1, 0, 1, 0, 6, 0, 12, 0, 0, 0, 128, 0, 1, 0, 1};
  int n = 0;
  memcpy(cff, (unsigned char[]){1, 0, 4, 1, /* header */ 0, 1, 1, 1, 5, 'T', 'e', 's', 't'}, 13);
  n = 13;
  int top_at = n;
  memcpy(cff + n, (unsigned char[]){0, 1, 1, 1, 7, 29, 0, 0, 0, 0, 17}, 11);
  n += 11;
  memset(cff + n, 0, 4); /* empty String and Global Subr INDEXes */
  n += 4;
  unsigned cs_at = (unsigned)n + cs_off;
  for (int i = 0; i < 4; i++)
    cff[top_at + 6 + i] = (unsigned char)(cs_at >> (24 - 8 * i));
  memcpy(cff + n, charstrings, sizeof charstrings);
  n += (int)sizeof charstrings;

  put16(head + 18, 1000);
  put16(hhea + 4, 800);
  put16(hhea + 6, (unsigned)-200 & 0xffff);
  put16(hhea + 34, 2);
  struct {
    const char *tag;
    const unsigned char *d;
    int len;
  } t[] = {{"CFF ", cff, n},
           {"cmap", cmap, sizeof cmap},
           {"head", head, sizeof head},
           {"hhea", hhea, sizeof hhea},
           {"hmtx", hmtx, sizeof hmtx},
           {"maxp", maxp, sizeof maxp}};
  int nt = 6, pos = 12 + 16 * nt;
  memcpy(out, "OTTO", 4);
  put16(out + 4, (unsigned)nt);
  for (int i = 0; i < nt; i++) {
    unsigned char *e = out + 12 + 16 * i;
    memcpy(e, t[i].tag, 4);
    memset(e + 4, 0, 4);
    for (int k = 0; k < 4; k++) {
      e[8 + k] = (unsigned char)(pos >> (24 - 8 * k));
      e[12 + k] = (unsigned char)(t[i].len >> (24 - 8 * k));
    }
    memcpy(out + pos, t[i].d, (size_t)t[i].len);
    pos += t[i].len;
  }
  return pos;
}

static void cff_square(void)
{
  unsigned char buf[512];
  int len = build_cff_font(buf, 0);
  struct font *f = font_load(buf, len);
  expect(f != NULL);
  if (!f)
    return;
  int gi = font_glyph_index(f, 'A');
  expect_eq(gi, 1);
  int w, h, ox, oy, adv;
  unsigned char *bm = font_render_glyph(f, gi, 100, &w, &h, &ox, &oy, &adv);
  expect(bm != NULL);
  if (bm) {
    expect_eq(w, 60);
    expect_eq(h, 60);
    expect_eq(ox, 10);
    expect_eq(oy, -70);
    expect_eq(bm[0], 255);
    expect_eq(bm[30 * 60 + 30], 255);
    expect_eq(bm[60 * 60 - 1], 255);
  }
  expect_eq(adv, 50);
  free(bm);
  font_free(f);
}

static void glyph_index_by_encoding(void)
{
  unsigned char buf[512];
  int len = build_cff_font(buf, 0);
  struct font *f = font_load(buf, len);
  expect(f != NULL);
  if (!f)
    return;
  expect_eq(font_glyph_index_enc(f, 1, 0, 128), 1);
  expect_eq(font_glyph_index_enc(f, 1, 0, 'A'), 0);
  expect_eq(font_glyph_index_enc(f, 3, 1, 'A'), 1);
  expect_eq(font_glyph_index_enc(f, 3, 0, 'A'), 0);
  expect_eq(font_glyph_index(f, 128), 0);
  font_free(f);
}

static void cff_bad_charstrings_offset(void)
{
  unsigned char buf[512];
  int len = build_cff_font(buf, 100000);
  expect(font_load(buf, len) == NULL);
}

int main(void)
{
  test("sans", sans);
  test("mono", mono);
  test("truncates", truncates);
  test("woff2", woff2);
  test("cff_square", cff_square);
  test("glyph_index_by_encoding", glyph_index_by_encoding);
  test("cff_bad_charstrings_offset", cff_bad_charstrings_offset);
  test("color_emoji", color_emoji);
  test("emoji_ligatures", emoji_ligatures);
  test("no_color_in_outline_font", no_color_in_outline_font);
  test("noto_sans_faces", noto_sans_faces);
  test("cjk_fallback", cjk_fallback);
  return 0;
}
