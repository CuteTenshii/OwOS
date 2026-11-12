/* Host unit test for the browser's CSS font-* support (css.c).
 *
 * Exercises the `font` shorthand tokenizer (the weight-vs-size disambiguation,
 * optional /line-height, family stack, system-font keywords) plus font-variant /
 * font-variant-caps small-caps, all through css_apply_inline.
 *
 *   cc -std=c11 -Itests -Iuser/apps/browser \
 *      tests/css_font_test.c user/apps/browser/css/engine.c user/apps/browser/css/engine_values.c
 * user/apps/browser/css/engine_props.c user/apps/browser/css/engine_decl.c user/apps/browser/css/engine_sel.c
 * user/apps/browser/css/engine_sheet.c user/apps/browser/css/sibling.c \
 *      user/apps/browser/color.c user/apps/browser/dom.c \
 *      -lm -o /tmp/css_font_test && /tmp/css_font_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include <string.h>

/* Assert the fields the font-* work touches. Pass -1 for "don't care" on the
 * scalar sentinels; fam is compared only when non-NULL. */
static const char *c_decls, *c_fam;
static int c_px, c_pct, c_bold, c_italic, c_scaps;
static short c_lhpct;

static void chk_body(void)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline(c_decls, &s);
  int ok = 1;
  if (c_px != -2 && s.font_px != c_px)
    ok = 0;
  if (c_pct != -2 && s.font_pct != c_pct)
    ok = 0;
  if (c_bold != -2 && s.bold != c_bold)
    ok = 0;
  if (c_italic != -2 && s.italic != c_italic)
    ok = 0;
  if (c_scaps != -2 && s.small_caps != c_scaps)
    ok = 0;
  if (c_lhpct != -2 && s.lh_pct != c_lhpct)
    ok = 0;
  if (c_fam && strcmp(s.fam, c_fam) != 0)
    ok = 0;
  expect(ok);
}

static short weight_of(const char *decls)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline(decls, &s);
  return s.weight;
}

static void weight_numbers(void)
{
  expect(weight_of("color: red") == -1);
  expect(weight_of("font-weight: 800") == 800);
  expect(weight_of("font-weight: 600") == 600);
  expect(weight_of("font-weight: bold") == 700);
  expect(weight_of("font-weight: normal") == 400);
  expect(weight_of("font-weight: bolder") == CSS_W_BOLDER);
  expect(weight_of("font-weight: lighter") == CSS_W_LIGHTER);
  expect(weight_of("font: 800 2rem/1 sans-serif") == 800);
  expect(weight_of("font: 1rem sans-serif") == 400);
  expect(weight_of("font: bold 1rem sans-serif") == 700);
}

/* The shape fonts.bunny.net and Google Fonts serve: one face per subset, each limited by unicode-range. */
static const char bunny[] = "@font-face{font-family:'Nunito Sans';font-style:normal;font-weight:600;"
                            "src:url(https://cdn.example/latin-ext-600.woff2) format('woff2'),"
                            "url(https://cdn.example/latin-ext-600.woff) format('woff');"
                            "unicode-range:U+0100-02BA,U+02BD-02C5,U+1E00-1E9F}"
                            "@font-face{font-family:'Nunito Sans';font-style:italic;font-weight:600;"
                            "src:url(https://cdn.example/latin-600i.woff2) format('woff2');"
                            "unicode-range:U+0000-00FF,U+2000-206F,U+4??}"
                            "@font-face{font-family:Var;font-weight:100 900;src:url(/v.ttf)}";

static void fontface_descriptors(void)
{
  css_sheet *sh = css_new();
  css_add(sh, bunny, (int)strlen(bunny), "https://fonts.example/css");
  expect(css_fontface_count(sh) == 3);
  struct css_fontface f;
  expect(css_fontface_get(sh, 0, &f));
  expect_string(f.family, "nunito sans");
  expect_string(f.url, "https://cdn.example/latin-ext-600.woff2");
  expect_string(f.base, "https://fonts.example/css");
  expect(f.wmin == 600 && f.wmax == 600 && !f.italic && f.nranges == 3);
  expect(!css_fontface_covers(&f, 'A'));
  expect(css_fontface_covers(&f, 0x0152));
  expect(css_fontface_covers(&f, 0x1E9F));
  expect(!css_fontface_covers(&f, 0x1EA0));

  expect(css_fontface_get(sh, 1, &f));
  expect(f.italic && f.nranges == 3);
  expect(css_fontface_covers(&f, 'A') && css_fontface_covers(&f, 0xE9) && css_fontface_covers(&f, 0x2019));
  expect(css_fontface_covers(&f, 0x400) && css_fontface_covers(&f, 0x4FF) && !css_fontface_covers(&f, 0x500));

  expect(css_fontface_get(sh, 2, &f));
  expect_string(f.family, "var"); /* names fold to lowercase, as in font-family stacks */
  expect(f.wmin == 100 && f.wmax == 900 && f.nranges == 0);
  expect(css_fontface_covers(&f, 0x1F33A));
  expect(!css_fontface_get(sh, 3, &f));
  css_free(sh);
}

/* src skips formats font_load can't read and takes extensionless URLs (Google Fonts serves those). */
static void fontface_src_choice(void)
{
  static const char src[] = "@font-face{font-family:A;src:url(a.eot);src:url(a.eot?#iefix) format('embedded-opentype'),"
                            "url(https://fonts.gstatic.example/s/a/v1/abc) format(\"woff2\")}"
                            "@font-face{font-family:B;src:url(b.svg#b) format('svg'),url(b-noext)}"
                            "@font-face{font-family:C;src:local('C'),url(c.eot)}";
  css_sheet *sh = css_new();
  css_add(sh, src, (int)strlen(src), 0);
  struct css_fontface f;
  expect(css_fontface_count(sh) == 2);
  expect(css_fontface_get(sh, 0, &f));
  expect_string(f.url, "https://fonts.gstatic.example/s/a/v1/abc");
  expect(css_fontface_get(sh, 1, &f));
  expect_string(f.url, "b-noext");
  css_free(sh);
}

static void chk(const char *decls, int want_px, int want_pct, int want_bold, int want_italic, int want_scaps,
                short want_lhpct, const char *want_fam)
{
  c_decls = decls;
  c_px = want_px;
  c_pct = want_pct;
  c_bold = want_bold;
  c_italic = want_italic;
  c_scaps = want_scaps;
  c_lhpct = want_lhpct;
  c_fam = want_fam;
  test(decls, chk_body);
}

int main(void)
{
  /* Bare shorthand: size + family, everything else resets to normal. */
  chk("font: 14px Arial", 14, -1, 0, 0, 0, -1, "arial");
  /* size/line-height + a quoted, multi-name family stack. */
  chk("font: 16px/1.5 \"Helvetica Neue\", sans-serif", 16, -1, 0, 0, 0, 150, "helvetica neue,sans-serif");
  /* Full prelude: style + variant + weight before the size. */
  chk("font: italic small-caps bold 18px/1.2 Georgia, serif", 18, -1, 1, 1, 1, 120, "georgia,serif");
  /* Numeric weight must NOT be mistaken for the font-size (the subtle case). */
  chk("font: 400 13px monospace", 13, -1, 0, 0, 0, -1, "monospace");
  chk("font: 700 20px Verdana", 20, -1, 1, 0, 0, -1, "verdana");
  /* Keyword size in the shorthand. */
  chk("font: bold large Tahoma", 18, -1, 1, 0, 0, -1, "tahoma");
  /* System-font keyword collapses to the UI face. */
  chk("font: menu", 15, -1, 0, 0, 0, -1, "sans-serif");
  chk("font: small-caption", 13, -1, 0, 0, 0, -1, "sans-serif");
  /* Invalid shorthand (no size) leaves the style untouched. */
  chk("font: italic bold", -1, -1, -1, -1, -1, -1, "");

  /* font-variant / font-variant-caps small-caps axis. */
  chk("font-variant: small-caps", -1, -1, -1, -1, 1, -1, NULL);
  chk("font-variant-caps: all-small-caps", -1, -1, -1, -1, 1, -1, NULL);
  chk("font-variant: normal", -1, -1, -1, -1, 0, -1, NULL);
  /* A ligature-only font-variant sets no caps. */
  chk("font-variant: common-ligatures", -1, -1, -1, -1, -1, -1, NULL);

  /* Longhand font-size keyword + percent forms still work. */
  chk("font-size: x-large", 24, -1, -1, -1, -1, -1, NULL);
  chk("font-size: 120%", -1, 120, -1, -1, -1, -1, NULL);
  /* em scales the parent's size like a percentage; rem stays absolute. */
  chk("font-size: .55em", -1, 55, -1, -1, -1, -1, NULL);
  chk("font-size: 2em", -1, 200, -1, -1, -1, -1, NULL);
  chk("font-size: 2rem", 32, -1, -1, -1, -1, -1, NULL);

  test("weight_numbers", weight_numbers);
  test("fontface_descriptors", fontface_descriptors);
  test("fontface_src_choice", fontface_src_choice);
  return 0;
}
