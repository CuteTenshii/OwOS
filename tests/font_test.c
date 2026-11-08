/* Host test for the font library (user/lib/font): family names, CFF (OpenType PostScript) outlines on a
 * font built in this file, and WOFF2 decoding. */
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
  unsigned char cmap[24] = {0, 0, 0, 1, 0, 3, 0, 1, 0, 0, 0, 12, 0, 6, 0, 12, 0, 0, 0, 65, 0, 1, 0, 1};
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
  test("cff_bad_charstrings_offset", cff_bad_charstrings_offset);
  return 0;
}
