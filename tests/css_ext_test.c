/* Host tests for CSS grid placement and areas, alignment, aspect-ratio, text, transform, filter, clip-path. */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include "dom.h"
#include "test.h"
#include <string.h>

static struct css_style parse(const char *decls)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline(decls, &s);
  return s;
}

static void check_areas(void)
{
  struct css_style s = parse("grid-template: \". up .\" \"left . right\" \". down .\" / 48px 48px 48px");
  expect(s.n_areas == 4 && s.n_cols == 3 && s.n_rows == 3);
  expect(s.areas[0].c0 == 1 && s.areas[0].r0 == 0 && s.areas[0].c1 == 2 && s.areas[0].r1 == 1);
  expect(s.areas[2].c0 == 2 && s.areas[2].r0 == 1);
  expect(s.grid_cols[1].kind == CSS_TRK_PX && s.grid_cols[1].v == 48);
}

static void check_minified_areas(void)
{
  struct css_style s = parse("grid-template:\".up.\"\"left.right\"\".down.\"/48px 48px 48px");
  expect(s.n_areas == 4 && s.areas[0].c0 == 1 && s.areas[1].c0 == 0 && s.areas[1].r0 == 1);
}

static void check_area_span(void)
{
  struct css_style s = parse("grid-template-areas: \"h h\" \"a b\"");
  expect(s.n_areas == 3 && s.areas[0].c0 == 0 && s.areas[0].c1 == 2 && s.areas[0].r1 == 1);
}

static void check_grid_area(void)
{
  struct css_style s = parse("grid-area: up");
  expect(!strcmp(s.area, "up"));
  s = parse("grid-area: 1 / 2");
  expect(s.row_start == 1 && s.col_start == 2 && s.area[0] == '\0');
  s = parse("grid-area: 2 / 1 / span 2 / 4");
  expect(s.row_start == 2 && s.row_span == 2 && s.col_start == 1 && s.col_span == 3);
}

static void check_grid_lines(void)
{
  struct css_style s = parse("grid-column: 2 / 4; grid-row: span 3");
  expect(s.col_start == 2 && s.col_span == 2 && s.row_start == 0 && s.row_span == 3);
  s = parse("grid-column-start: 3; grid-column-end: 5");
  expect(s.col_start == 3 && s.col_span == 2);
  s = parse("grid-column: 1 / -1");
  expect(s.col_start == 1 && s.col_span == CSS_GRID_MAX);
}

static void check_align(void)
{
  struct css_style s = parse("align-self: center");
  expect(s.align_self == CSS_ALIGN_CENTER);
  s = parse("align-self: auto");
  expect(s.align_self == CSS_ALIGN_AUTO);
}

static void check_aspect(void)
{
  expect(parse("aspect-ratio: 16 / 9").aspect == 1778);
  expect(parse("aspect-ratio: 1").aspect == 1000);
  expect(parse("aspect-ratio: auto 4/3").aspect == 1333);
  expect(parse("aspect-ratio: auto").aspect == 0);
}

static void check_text(void)
{
  struct css_style s = parse("visibility: hidden; text-overflow: ellipsis; object-fit: cover; pointer-events: none");
  expect(s.visibility == 0 && s.text_ovf == 1 && s.obj_fit == CSS_OF_COVER && s.pointer_events == 0);
  s = parse("user-select: none");
  expect(s.user_select == 0);
  s = parse("-webkit-user-select: text");
  expect(s.user_select == 1);
}

static void check_cursor(void)
{
  expect_eq(parse("cursor: pointer").cursor, CSS_CUR_POINTER);
  expect_eq(parse("cursor: col-resize").cursor, CSS_CUR_EW);
  expect_eq(parse("cursor: url(a.png) 2 2, not-allowed").cursor, CSS_CUR_NOT_ALLOWED);
  expect_eq(parse("cursor: wait").cursor, CSS_CUR_WAIT);
  expect_eq(parse("cursor: bogus").cursor, -1);
}

static void check_transform(void)
{
  struct css_style s = parse("transform: translate(-50%, 10px)");
  expect(s.has_tr == 1 && s.tr_xp == -50 && s.tr_y == 10 && s.tr_x == 0);
  s = parse("transform: translateX(8px) translateY(-25%)");
  expect(s.tr_x == 8 && s.tr_yp == -25);
  s = parse("translate: 4px 8px");
  expect(s.has_tr == 1 && s.tr_x == 4 && s.tr_y == 8);
  s = parse("transform: translate(5px); transform: none");
  expect(s.has_tr == 0 && s.tr_x == 0);
  s = parse("transform: rotate(180deg)");
  expect(s.has_tr == 0);
}

static void check_filter(void)
{
  struct css_style s = parse("filter: brightness(1.5) grayscale(50%) invert(1)");
  expect(s.flt_bright == 1500 && s.flt_gray == 50 && s.flt_invert == 100 && s.flt_opacity == 100);
}

static void check_clip(void)
{
  struct css_style s = parse("clip-path: circle(50% at 50% 40%)");
  expect(s.clip_kind == CSS_CLIP_CIRCLE && s.clip_n == 3 && s.clip_v[0] == 500 && s.clip_v[2] == 400);
  s = parse("clip-path: inset(10px 20px round 8px)");
  expect(s.clip_kind == CSS_CLIP_INSET && s.clip_n == 5 && s.clip_v[0] == 10 && s.clip_v[1] == 20 &&
         s.clip_v[2] == 10 && s.clip_v[3] == 20 && s.clip_v[4] == 8);
  s = parse("clip-path: polygon(50% 0, 100% 100%, 0 100%)");
  expect(s.clip_kind == CSS_CLIP_POLYGON && s.clip_n == 6 && s.clip_v[3] == 1000);
  s = parse("clip-path: none");
  expect(s.clip_kind == CSS_CLIP_NONE);
}

static void check_units(void)
{
  css_set_viewport(800, 600);
  expect(parse("width: 100dvh").width == 600);
  expect(parse("width: 10svw").width == 80);
  expect(parse("padding-top: env(safe-area-inset-top, 12px)").padding[0] == 12);
  expect(parse("padding-top: env(safe-area-inset-top)").padding[0] == 0);
  expect(parse("padding-top: max(12px, env(safe-area-inset-top))").padding[0] == 12);
}

static struct dom_node *find(struct dom_node *n, const char *id)
{
  const char *v = dom_get_attr(n, "id");
  if (v && !strcmp(v, id))
    return n;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    struct dom_node *r = find(c, id);
    if (r)
      return r;
  }
  return 0;
}

static int color_of(css_sheet *sh, struct dom_node *el)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_sheet(sh, el, &s);
  return s.color;
}

static void check_dynamic_state(void)
{
  const char *html = "<div id=a class=btn><span id=b>x</span></div><p id=c>y</p>";
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  const char *css = ".btn{color:#111111} .btn:hover{color:#222222} .btn:active{color:#333333}"
                    "#c:focus{color:#444444} div:focus-within #b{color:#555555}";
  css_sheet *sh = css_new();
  css_add(sh, css, (int)strlen(css), "http://t/");
  struct dom_node *a = find(doc, "a"), *b = find(doc, "b"), *c = find(doc, "c");
  css_set_state(0, 0, 0);
  unsigned g0 = css_state_generation();
  expect_eq(color_of(sh, a), 0x111111);
  expect_eq(color_of(sh, c), -1);
  css_set_state(b, 0, 0); /* the pointer is on a descendant */
  expect(css_state_generation() != g0);
  expect_eq(color_of(sh, a), 0x222222);
  css_set_state(b, a, 0);
  expect_eq(color_of(sh, a), 0x333333);
  css_set_state(0, 0, c);
  expect_eq(color_of(sh, c), 0x444444);
  expect_eq(color_of(sh, b), -1);
  css_set_state(0, 0, b);
  expect_eq(color_of(sh, b), 0x555555);
  unsigned g1 = css_state_generation();
  css_set_state(0, 0, b);
  expect(css_state_generation() == g1);
  css_set_state(0, 0, 0);
  css_free(sh);
  dom_free(doc);
}

static int sheet_color(const char *css)
{
  const char *html = "<p id=a>x</p>";
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  css_sheet *sh = css_new();
  css_add(sh, css, (int)strlen(css), "http://t/");
  int c = color_of(sh, find(doc, "a"));
  css_free(sh);
  dom_free(doc);
  return c;
}

static void check_media_features(void)
{
  css_set_viewport(800, 600);
  expect_eq(sheet_color("#a{color:#010101} @media (hover:hover) and (pointer:fine){#a{color:#020202}}"), 0x020202);
  expect_eq(sheet_color("#a{color:#010101} @media (hover:none) and (pointer:coarse){#a{color:#020202}}"), 0x010101);
  expect_eq(sheet_color("#a{color:#010101} @media (prefers-reduced-motion:reduce){#a{color:#020202}}"), 0x010101);
  expect_eq(sheet_color("#a{color:#010101} @media (prefers-reduced-motion:no-preference){#a{color:#020202}}"),
            0x020202);
  expect_eq(sheet_color("#a{color:#010101} @media (any-hover:none){#a{color:#020202}}"), 0x010101);
}

static void check_supports(void)
{
  expect_eq(sheet_color("#a{color:#010101} @supports (display:grid){#a{color:#020202}}"), 0x020202);
  expect_eq(sheet_color("#a{color:#010101} @supports (backdrop-filter:blur(2px)){#a{color:#020202}}"), 0x010101);
  expect_eq(sheet_color("#a{color:#010101} @supports not (backdrop-filter:blur(2px)){#a{color:#020202}}"), 0x020202);
  expect_eq(sheet_color(
                "#a{color:#010101} @supports (display:grid) and (not (backdrop-filter:blur(2px))) {#a{color:#020202}}"),
            0x020202);
  expect_eq(sheet_color("#a{color:#010101} @supports (nope:1) or (aspect-ratio:1){#a{color:#020202}}"), 0x020202);
  expect_eq(sheet_color("#a{color:#010101} @supports selector(:hover){#a{color:#020202}}"), 0x020202);
  expect_eq(sheet_color("#a{color:#010101} @supports selector(:has(a)){#a{color:#020202}}"), 0x010101);
}

static void check_selection(void)
{
  const char *html = "<div id=a><p id=b>x</p></div><p id=c>y</p>";
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  const char *css = "#a::selection{background:#102030} p::selection{background:#405060}";
  css_sheet *sh = css_new();
  css_add(sh, css, (int)strlen(css), "http://t/");
  unsigned rgb = 0;
  expect(css_sheet_has_selection(sh));
  expect(css_selection_color(sh, find(doc, "a"), &rgb) && rgb == 0x102030);
  expect(css_selection_color(sh, find(doc, "b"), &rgb) && rgb == 0x405060);
  expect(css_selection_color(sh, find(doc, "c"), &rgb) && rgb == 0x405060);
  struct css_style s = parse("color: red");
  expect(s.color == 0xff0000);
  css_free(sh);
  css_sheet *plain = css_new();
  css_add(plain, "p{color:red}", 12, "http://t/");
  expect(!css_sheet_has_selection(plain));
  expect(color_of(plain, find(doc, "c")) == 0xff0000);
  css_free(plain);
  dom_free(doc);
}

int main(void)
{
  test("grid-template areas", check_areas);
  test("grid-template areas, minified", check_minified_areas);
  test("grid-template-areas spans", check_area_span);
  test("grid-area", check_grid_area);
  test("grid line placement", check_grid_lines);
  test("align-self, align-content", check_align);
  test("aspect-ratio", check_aspect);
  test("text and visibility properties", check_text);
  test("cursor", check_cursor);
  test("transform", check_transform);
  test("filter", check_filter);
  test("clip-path", check_clip);
  test("dvh and env()", check_units);
  test(":hover, :active, :focus", check_dynamic_state);
  test("hover, pointer and motion media features", check_media_features);
  test("@supports", check_supports);
  test("::selection", check_selection);
  return 0;
}
