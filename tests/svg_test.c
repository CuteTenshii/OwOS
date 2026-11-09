/* Host tests for the SVG rasterizer: shapes, strokes, opacity, gradients, <use>, clip paths, sizing. */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include "features/svg/svg.h"
#include "font.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#define W 20
static unsigned px[W * W];

static void render(const char *svg)
{
  memset(px, 0xAA, sizeof px);
  expect(svg_render(svg, (int)strlen(svg), W, W, px, 0x102030));
}

static unsigned at(int x, int y) { return px[y * W + x]; }

static void check_fill_and_aa(void)
{
  render("<svg viewBox='0 0 20 20'><rect x='5' y='5' width='10' height='10' fill='#ff0000'/></svg>");
  expect_eq(at(10, 10), 0xFFFF0000u);
  expect_eq(at(2, 2), 0u);
  render("<svg viewBox='0 0 20 20'><rect x='5.5' y='5' width='9' height='10' fill='#ffffff'/></svg>");
  expect_eq(at(5, 10) >> 24, 128u);
  expect_eq(at(6, 10), 0xFFFFFFFFu);
}

static void check_stroke_styles(void)
{
  render("<svg viewBox='0 0 20 20'><path d='M2 10H18' stroke='#00ff00' stroke-width='4' fill='none'/></svg>");
  expect_eq(at(10, 10), 0xFF00FF00u);
  expect_eq(at(0, 10), 0u);
  render(
      "<svg viewBox='0 0 20 20'><path d='M5 10H15' stroke='#00ff00' stroke-width='4' stroke-linecap='square'/></svg>");
  expect_eq(at(3, 10), 0xFF00FF00u);
  render("<svg viewBox='0 0 20 20'><path d='M5 10H15' stroke='#00ff00' stroke-width='4'/></svg>");
  expect_eq(at(3, 10), 0u);
  render("<svg viewBox='0 0 20 20'><path d='M0 10H20' stroke='#fff' stroke-width='4' stroke-dasharray='4 4'/></svg>");
  expect_eq(at(2, 10), 0xFFFFFFFFu);
  expect_eq(at(6, 10), 0u);
  expect_eq(at(10, 10), 0xFFFFFFFFu);
}

static void check_join_miter_vs_bevel(void)
{
  const char *m = "<svg viewBox='0 0 20 20'><path d='M3 17V8H17' stroke='#fff' stroke-width='4' fill='none'/></svg>";
  const char *b = "<svg viewBox='0 0 20 20'><path d='M3 17V8H17' stroke='#fff' stroke-width='4' fill='none' "
                  "stroke-linejoin='bevel'/></svg>";
  render(m);
  expect_eq(at(1, 6), 0xFFFFFFFFu);
  render(b);
  expect_eq(at(1, 6), 0u);
}

static void check_path_commands(void)
{
  render("<svg viewBox='0 0 20 20'><path d='M2 2h16v16H2zM6 6v8h8V6z' fill='#fff' fill-rule='evenodd'/></svg>");
  expect_eq(at(3, 3), 0xFFFFFFFFu);
  expect_eq(at(10, 10), 0u);
  render("<svg viewBox='0 0 20 20'><path d='M2 2h16v16H2zM6 6h8v8H6z' fill='#fff'/></svg>");
  expect_eq(at(10, 10), 0xFFFFFFFFu);
  render("<svg viewBox='0 0 20 20'><path d='M2 10a8 8 0 0116 0z' fill='#fff'/></svg>");
  expect_eq(at(10, 5), 0xFFFFFFFFu);
  expect_eq(at(10, 15), 0u);
  render("<svg viewBox='0 0 20 20'><path d='M2 2L18 2L18 18Z L2 18' stroke='#fff' fill='none'/></svg>");
  expect_eq(at(5, 2) >> 24 != 0, 1);
}

static void check_opacity(void)
{
  render("<svg viewBox='0 0 20 20'><rect width='20' height='20' fill='#ff0000' fill-opacity='0.5'/></svg>");
  expect_eq(at(10, 10), 0x80FF0000u);
  render("<svg viewBox='0 0 20 20'><g opacity='0.5'><rect width='20' height='20' fill='#0000ff'/>"
         "<rect width='20' height='20' fill='#0000ff'/></g></svg>");
  expect_eq(at(10, 10), 0x800000FFu);
}

static void check_gradient(void)
{
  render("<svg viewBox='0 0 20 20'><defs><linearGradient id='g'><stop offset='0' stop-color='#000000'/>"
         "<stop offset='1' stop-color='#ffffff'/></linearGradient></defs>"
         "<rect width='20' height='20' fill='url(#g)'/></svg>");
  unsigned l = at(1, 10) & 0xFF, m = at(10, 10) & 0xFF, r = at(18, 10) & 0xFF;
  expect(l < 40 && r > 215 && m > 100 && m < 160);
  render("<svg viewBox='0 0 20 20'><defs><radialGradient id='g'><stop offset='0' stop-color='#ffffff'/>"
         "<stop offset='1' stop-color='#000000'/></radialGradient></defs>"
         "<circle cx='10' cy='10' r='10' fill='url(#g)'/></svg>");
  expect((at(10, 10) & 0xFF) > 230 && (at(10, 1) & 0xFF) < 60);
  render("<svg viewBox='0 0 20 20'><rect width='20' height='20' fill='url(#missing) #00ff00'/></svg>");
  expect_eq(at(10, 10), 0xFF00FF00u);
}

static void check_use_and_symbol(void)
{
  render("<svg viewBox='0 0 20 20'><defs><rect id='r' width='4' height='4' fill='#ff0000'/></defs>"
         "<use href='#r' x='10' y='10'/></svg>");
  expect_eq(at(11, 11), 0xFFFF0000u);
  expect_eq(at(2, 2), 0u);
  render("<svg viewBox='0 0 20 20'><symbol id='s' viewBox='0 0 2 2'><rect width='2' height='2' fill='#fff'/></symbol>"
         "<use href='#s' width='10' height='10'/></svg>");
  expect_eq(at(9, 9), 0xFFFFFFFFu);
  expect_eq(at(12, 12), 0u);
  render("<svg viewBox='0 0 20 20'><g id='a'><use href='#a'/></g><rect width='5' height='5' fill='#fff'/></svg>");
  expect_eq(at(2, 2), 0xFFFFFFFFu);
}

static void check_clip_path(void)
{
  render("<svg viewBox='0 0 20 20'><clipPath id='c'><rect width='10' height='20'/></clipPath>"
         "<rect width='20' height='20' fill='#fff' clip-path='url(#c)'/></svg>");
  expect_eq(at(5, 10), 0xFFFFFFFFu);
  expect_eq(at(15, 10), 0u);
}

static void check_style_sheet_and_color(void)
{
  render("<svg viewBox='0 0 20 20'><style>.a{fill:#00ff00} #b{fill:#0000ff}</style>"
         "<rect class='a' width='10' height='20'/><rect id='b' class='a' x='10' width='10' height='20'/></svg>");
  expect_eq(at(5, 10), 0xFF00FF00u);
  expect_eq(at(15, 10), 0xFF0000FFu);
  render("<svg viewBox='0 0 20 20'><rect width='20' height='20' fill='currentColor' style='fill:#ff00ff'/></svg>");
  expect_eq(at(10, 10), 0xFFFF00FFu);
  render("<svg viewBox='0 0 20 20' color='#ff0000'><rect width='20' height='20' fill='currentColor'/></svg>");
  expect_eq(at(10, 10), 0xFFFF0000u);
  render("<svg viewBox='0 0 20 20'><rect width='20' height='20' fill='currentColor'/></svg>");
  expect_eq(at(10, 10), 0xFF102030u);
}

static void check_transform_and_par(void)
{
  render(
      "<svg viewBox='0 0 20 20'><rect width='5' height='5' fill='#fff' transform='translate(10 10) scale(2)'/></svg>");
  expect_eq(at(15, 15), 0xFFFFFFFFu);
  expect_eq(at(8, 8), 0u);
  render("<svg viewBox='0 0 10 5'><rect width='10' height='5' fill='#fff'/></svg>");
  expect_eq(at(10, 10), 0xFFFFFFFFu);
  expect_eq(at(10, 2), 0u);
  render("<svg viewBox='0 0 10 5' preserveAspectRatio='none'><rect width='10' height='5' fill='#fff'/></svg>");
  expect_eq(at(10, 2), 0xFFFFFFFFu);
}

static void check_size(void)
{
  int w = 0, h = 0;
  const char *a = "<svg width='24' height='12' viewBox='0 0 100 100'></svg>";
  expect(svg_size(a, (int)strlen(a), &w, &h) && w == 24 && h == 12);
  const char *b = "<svg width='30' viewBox='0 0 100 50'></svg>";
  expect(svg_size(b, (int)strlen(b), &w, &h) && w == 30 && h == 15);
  const char *c = "<svg viewBox='0 0 8 4'></svg>";
  expect(svg_size(c, (int)strlen(c), &w, &h) && w == 8 && h == 4);
  const char *d = "<svg width='1in' height='12pt'></svg>";
  expect(svg_size(d, (int)strlen(d), &w, &h) && w == 96 && h == 16);
}

static void check_marker(void)
{
  render("<svg viewBox='0 0 20 20'><marker id='m' markerWidth='4' markerHeight='4' refX='2' refY='2' orient='auto' "
         "markerUnits='userSpaceOnUse'><rect width='4' height='4' fill='#ff0000'/></marker>"
         "<path d='M2 10H10' stroke='none' marker-end='url(#m)'/></svg>");
  expect_eq(at(11, 10), 0xFFFF0000u);
  expect_eq(at(3, 10), 0u);
}

static void check_pattern(void)
{
  render("<svg viewBox='0 0 20 20'><pattern id='p' width='4' height='4' patternUnits='userSpaceOnUse'>"
         "<rect width='2' height='2' fill='#ff0000'/></pattern><rect width='20' height='20' fill='url(#p)'/></svg>");
  expect_eq(at(1, 1), 0xFFFF0000u);
  expect_eq(at(3, 3), 0u);
  expect_eq(at(9, 1), 0xFFFF0000u);
}

static void check_mask(void)
{
  render("<svg viewBox='0 0 20 20'><mask id='m'><rect width='10' height='20' fill='#fff'/></mask>"
         "<rect width='20' height='20' fill='#00f' mask='url(#m)'/></svg>");
  expect_eq(at(5, 10), 0xFF0000FFu);
  expect_eq(at(15, 10), 0u);
  render("<svg viewBox='0 0 20 20'><mask id='m'><rect width='20' height='20' fill='#808080'/></mask>"
         "<rect width='20' height='20' fill='#fff' mask='url(#m)'/></svg>");
  unsigned a = at(10, 10) >> 24;
  expect(a > 120 && a < 136);
}

static void check_filter(void)
{
  render("<svg viewBox='0 0 20 20'><filter id='f' x='0' y='0' width='1' height='1' filterUnits='objectBoundingBox'>"
         "<feFlood flood-color='#00ff00'/></filter><rect width='20' height='20' fill='red' filter='url(#f)'/></svg>");
  expect_eq(at(10, 10), 0xFF00FF00u);
  render(
      "<svg viewBox='0 0 20 20'><filter id='f' x='-1' y='-1' width='3' height='3'>"
      "<feOffset dx='5' dy='0'/></filter><rect x='2' y='5' width='4' height='4' fill='#f00' filter='url(#f)'/></svg>");
  expect_eq(at(3, 6), 0u);
  expect_eq(at(8, 6), 0xFFFF0000u);
  render("<svg viewBox='0 0 20 20'><filter id='f' x='-1' y='-1' width='3' height='3'><feGaussianBlur stdDeviation='2'/>"
         "</filter><rect x='4' y='4' width='10' height='12' fill='#fff' filter='url(#f)'/></svg>");
  unsigned edge = at(14, 10) >> 24, mid = at(9, 10) >> 24;
  expect(mid > 230 && edge > 60 && edge < 200);
  render("<svg viewBox='0 0 20 20'><filter id='f' color-interpolation-filters='sRGB'>"
         "<feColorMatrix type='matrix' values='0 0 0 0 0  0 0 0 0 1  0 0 0 0 0  0 0 0 1 0'/></filter>"
         "<rect width='20' height='20' fill='#ff0000' filter='url(#f)'/></svg>");
  expect_eq(at(10, 10), 0xFF00FF00u);
}

static void check_image(void)
{
  render("<svg viewBox='0 0 20 20'><image width='20' height='20' href='data:image/png;base64,"
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=='/></svg>");
  expect_eq(at(10, 10), 0xFFFF0000u);
}

static int first_ink_column(void)
{
  for (int x = 0; x < W; x++)
    for (int y = 0; y < W; y++)
      if (at(x, y) >> 24)
        return x;
  return -1;
}

static void check_text(void)
{
  FILE *fp = fopen("fonts/LiberationSans-Regular.ttf", "rb");
  expect(fp != 0);
  if (!fp)
    return;
  static unsigned char buf[1 << 20];
  int n = (int)fread(buf, 1, sizeof buf, fp);
  fclose(fp);
  svg_set_fonts(font_load(buf, n), 0);
  render("<svg viewBox='0 0 20 20'><text x='2' y='16' font-size='16' fill='#fff'>H</text></svg>");
  int ink = 0;
  for (int i = 0; i < W * W; i++)
    ink += (px[i] >> 24) > 0;
  expect(ink > 20);
  int start = first_ink_column();
  expect(start >= 2 && start <= 4);
  render("<svg viewBox='0 0 20 20'><text x='10' y='16' font-size='16' text-anchor='middle' fill='#fff'>H</text></svg>");
  expect(first_ink_column() > start + 1);
}

static void check_transform_units(void)
{
  render("<svg viewBox='0 0 20 20'><rect width='5' height='5' fill='#fff' style='transform: translate(10px, 10px)'/>"
         "<rect width='5' height='5' fill='#f00' transform='translate(10px, 0)'/></svg>");
  expect_eq(at(2, 2), 0u);
  expect_eq(at(12, 2), 0xFFFF0000u);
}

static void check_css_capture(void)
{
  struct css_style cs;
  css_style_reset(&cs);
  css_apply_inline("stroke-width: 3; fill: url(#g); opacity: .5 !important; margin: 4px", &cs);
  expect_string(cs.svg_decl, "stroke-width:3;fill:url(#g);opacity:.5;");
  css_style_reset(&cs);
  expect_string(cs.svg_decl, "");
}

int main(void)
{
  test("fill and anti-aliasing", check_fill_and_aa);
  test("stroke caps and dashes", check_stroke_styles);
  test("miter and bevel joins", check_join_miter_vs_bevel);
  test("path commands", check_path_commands);
  test("opacity and group opacity", check_opacity);
  test("gradients", check_gradient);
  test("use and symbol", check_use_and_symbol);
  test("clip-path", check_clip_path);
  test("style sheet and currentColor", check_style_sheet_and_color);
  test("transform and preserveAspectRatio", check_transform_and_par);
  test("intrinsic size", check_size);
  test("marker", check_marker);
  test("pattern", check_pattern);
  test("mask", check_mask);
  test("filter", check_filter);
  test("image", check_image);
  test("text", check_text);
  test("css transform units", check_transform_units);
  test("page css svg declarations", check_css_capture);
  return 0;
}
