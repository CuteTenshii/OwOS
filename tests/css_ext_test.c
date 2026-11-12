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

/* fr weights keep their fraction: 1fr 1.6fr is not two equal columns. */
static void check_fractional_fr(void)
{
  struct css_style s = parse("grid-template-columns: 1fr 1.6fr 0.25fr 2fr");
  expect(s.n_cols == 4);
  expect(s.grid_cols[0].kind == CSS_TRK_FR && s.grid_cols[0].v == 100);
  expect(s.grid_cols[1].kind == CSS_TRK_FR && s.grid_cols[1].v == 160);
  expect(s.grid_cols[2].kind == CSS_TRK_FR && s.grid_cols[2].v == 25);
  expect(s.grid_cols[3].kind == CSS_TRK_FR && s.grid_cols[3].v == 200);
}

/* Logical margins and paddings land on the physical sides of horizontal left-to-right text. */
static void check_logical_box(void)
{
  struct css_style s = parse("margin-inline-start:auto;margin-inline-end:auto;padding-block-start:72px;"
                             "padding-block-end:56px;padding-inline:4px 9px");
  expect(s.margin[3] == -2 && s.margin[1] == -2 && s.margin[0] == -1 && s.margin[2] == -1);
  expect(s.padding[0] == 72 && s.padding[2] == 56 && s.padding[3] == 4 && s.padding[1] == 9);
  s = parse("margin-block:12px;padding-inline-end:3px");
  expect(s.margin[0] == 12 && s.margin[2] == 12 && s.margin[1] == -1 && s.padding[1] == 3 && s.padding[3] == -1);
}

/* A background shorthand without a colour, or a transparent one, clears a colour set earlier. */
static void check_background_clear(void)
{
  struct css_style s = parse("background:#8a2fd6;background:0 0");
  expect(s.bg == CSS_BG_CLEAR);
  s = parse("background-color:#8a2fd6;background-color:transparent");
  expect(s.bg == CSS_BG_CLEAR);
  s = parse("background:0 0;background-color:#102030");
  expect(s.bg == 0x102030);
  s = parse("background:#8a2fd6 url(x.png) no-repeat");
  expect(s.bg == 0x8a2fd6);
}

/* em and ch widths wait for the element's font; px and rem resolve at once. */
static void check_font_relative_widths(void)
{
  struct css_style s = parse("max-width:34ch;width:2.5em;min-width:3rem");
  expect(s.font_rel_w[1] == -3400 && s.font_rel_w[0] == 250 && s.font_rel_w[2] == 0 && s.min_w == 48);
  s = parse("max-width:34ch;max-width:200px");
  expect(s.font_rel_w[1] == 0 && s.max_w == 200);
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
  expect_eq(parse("tab-size: 2").tab_size, 2);
  expect_eq(parse("scrollbar-width: none").sb_w, 0);
  expect_eq(parse("scrollbar-width: thin").sb_w, 6);
  expect_eq(parse("scrollbar-width: auto").sb_w, -2);
  expect_eq(parse("scrollbar-color: #102030 #405060").sb_thumb, 0x102030);
  expect_eq(parse("scrollbar-color: #102030 #405060").sb_track, 0x405060);
  expect_eq(parse("scrollbar-color: auto").sb_thumb, -2);
  expect_eq(parse("scrollbar-color: red").sb_thumb, -1);
  expect_eq(parse("tab-size: 0").tab_size, 0);
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
  expect(s.has_tr == 1 && s.tr_mat == 1 && s.tr_m[0] == -4096 && s.tr_m[3] == -4096);
  s = parse("transform: rotate(90deg)");
  expect(s.tr_mat && s.tr_m[0] == 0 && s.tr_m[1] == 4096 && s.tr_m[2] == -4096 && s.tr_m[3] == 0);
  s = parse("transform: scale(2, 0.5)");
  expect(s.tr_mat && s.tr_m[0] == 8192 && s.tr_m[3] == 2048 && s.tr_m[1] == 0);
  s = parse("transform: translate(10px, 20px) rotate(180deg)"); /* the rotation acts on the point first */
  expect(s.tr_x == 10 && s.tr_y == 20 && s.tr_m[0] == -4096);
  s = parse("transform: scale(2) translate(10px, 5px)"); /* a scaled translation */
  expect(s.tr_x == 20 && s.tr_y == 10 && s.tr_m[0] == 8192);
  s = parse("transform: translate(-50%, -50%) scale(2)");
  expect(s.tr_xp == -50 && s.tr_yp == -50);
  s = parse("transform: skewX(45deg)");
  expect(s.tr_m[2] == 4096 && s.tr_m[1] == 0);
  s = parse("transform: matrix(1, 0, 0, 1, 7, 9)");
  expect(!s.tr_mat && s.tr_x == 7 && s.tr_y == 9);
  expect_eq(parse("rotate: 0.5turn").tr_m[0], -4096);
  s = parse("scale: -1 1");
  expect(s.tr_m[0] == -4096 && s.tr_m[3] == 4096);
  s = parse("transform: rotate(45deg)");
  expect(s.tr_m[0] == 2896 && s.tr_m[1] == 2896 && s.tr_m[2] == -2896);
  s = parse("transform-origin: left top");
  expect(s.tr_org == 1 && s.tr_oxp == 0 && s.tr_oyp == 0);
  s = parse("transform-origin: 10px 25%");
  expect(s.tr_ox == 10 && s.tr_oxp == 0 && s.tr_oyp == 25);
  s = parse("transform-origin: bottom");
  expect(s.tr_oyp == 100 && s.tr_oxp == 50);
  s = parse("transform-origin: right center");
  expect(s.tr_oxp == 100 && s.tr_oyp == 50);
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

static void check_content(void)
{
  struct css_style s = parse("content: \"a\" attr(data-n) \"b\\2192 c\" open-quote");
  char want[] = {'a', CONTENT_ATTR, 'd',        'a',        't', 'a',        '-',        'n',        CONTENT_END,
                 'b', (char)0xE2,   (char)0x86, (char)0x92, 'c', (char)0xE2, (char)0x80, (char)0x9C, 0};
  expect_string(s.content, want);
  s = parse("content: none");
  expect(s.content[0] == '\0');
  s = parse("content: \"x\\\"y\"");
  expect_string(s.content, "x\"y");
}

static void check_spacing(void)
{
  expect_eq(parse("letter-spacing: 2px").letter_sp, 2);
  expect_eq(parse("letter-spacing: .2px").letter_sp, 0);
  expect_eq(parse("letter-spacing: 0.1em").letter_sp, 2);
  expect_eq(parse("letter-spacing: -1px").letter_sp, -1);
  expect_eq(parse("letter-spacing: normal").letter_sp, 0);
  expect_eq(parse("color: red").letter_sp, CSS_LS_UNSET);
  struct css_style s = parse("vertical-align: super");
  expect(s.va_kind == CSS_VA_SUPER);
  s = parse("vertical-align: 3px");
  expect(s.va_kind == CSS_VA_LEN && s.va_len == 3);
  expect_eq(parse("vertical-align: baseline").va_kind, CSS_VA_BASELINE);
}

static void check_marker_and_clip(void)
{
  const char *html = "<details><summary id=a>x</summary></details><ul><li id=b>y</li></ul>";
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  css_sheet *sh = css_new();
  const char *css = "summary::-webkit-details-marker{display:none} li::marker{color:red}";
  css_add(sh, css, (int)strlen(css), "http://t/");
  expect(css_marker_hidden(sh, find(doc, "a")));
  expect(!css_marker_hidden(sh, find(doc, "b")));
  css_free(sh);
  dom_free(doc);
  expect_eq(parse("background-clip: content-box").bg_clip, 2);
  expect_eq(parse("background-clip: padding-box").bg_clip, 1);
  expect_eq(parse("background-clip: border-box").bg_clip, 0);
  expect_eq(parse("background-clip: text").bg_clip, -1);
}

static void check_scrollbar_rules(void)
{
  const char *html = "<html><body><p>x</p></body></html>";
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  struct dom_node *body = doc->first_child->first_child;
  while (body && strcmp(body->tag, "body"))
    body = body->next;
  css_sheet *sh = css_new();
  const char *css = "::-webkit-scrollbar{width:7px;background:#112233} ::-webkit-scrollbar-thumb{background:#445566;"
                    "border-radius:3px} ::-webkit-scrollbar-track{background:#778899}";
  css_add(sh, css, (int)strlen(css), "http://t/");
  struct css_sbparts p;
  expect(css_scrollbar_parts(sh, body, &p));
  expect(p.width == 7 && p.thumb == 0x445566 && p.radius == 3 && p.track == 0x778899 && !p.hidden);
  css_free(sh);
  sh = css_new();
  css = "body::-webkit-scrollbar{display:none}";
  css_add(sh, css, (int)strlen(css), "http://t/");
  expect(css_scrollbar_parts(sh, body, &p) && p.hidden);
  css_free(sh);
  sh = css_new();
  css_add(sh, "p{color:red}", 12, "http://t/");
  expect(!css_scrollbar_parts(sh, body, &p));
  css_free(sh);
  dom_free(doc);
}

int main(void)
{
  test("grid-template areas", check_areas);
  test("grid-template areas, minified", check_minified_areas);
  test("grid fr weights keep their fraction", check_fractional_fr);
  test("logical margin and padding", check_logical_box);
  test("background shorthand clears the colour", check_background_clear);
  test("em and ch widths stay font-relative", check_font_relative_widths);
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
  test("::-webkit-scrollbar rules", check_scrollbar_rules);
  test("::marker and background-clip", check_marker_and_clip);
  test("letter-spacing and vertical-align", check_spacing);
  test("content strings and attr()", check_content);
  return 0;
}
