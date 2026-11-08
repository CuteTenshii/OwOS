/* Host unit test for the browser's CSS font-* support (css.c).
 *
 * Exercises the `font` shorthand tokenizer (the weight-vs-size disambiguation,
 * optional /line-height, family stack, system-font keywords) plus font-variant /
 * font-variant-caps small-caps, all through css_apply_inline.
 *
 *   cc -std=c11 -Iscripts -Iuser/apps/browser \
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

  return 0;
}
