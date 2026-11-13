/* style.c: widget styles, the app's theme, box geometry and the painting shared by the widgets. See gfx.h */
#include "gfx.h"

#define FOCUS 0x8fb4f5u

/* One colour set for all four states */
#define SAME4(...) {__VA_ARGS__, __VA_ARGS__, __VA_ARGS__, __VA_ARGS__}

/* The two-state controls share everything but their part size and the switch's off track */
#define CTL_COLORS(off_bg, off_bg_pressed, off_bg_disabled)                                                            \
  .color = {{off_bg, 0xe6ebf2u, 0x7a8696u, off_bg},                                                                    \
            {off_bg, 0xe6ebf2u, 0xa8b4c6u, off_bg},                                                                    \
            {off_bg_pressed, 0xe6ebf2u, 0xa8b4c6u, off_bg_pressed},                                                    \
            {off_bg_disabled, 0x636b78u, 0x4a525eu, off_bg_disabled}},                                                 \
  .accent = {{0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u},                                                             \
             {0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u},                                                             \
             {0x2559b8u, 0xffffffu, 0x2f6bd8u, 0x2559b8u},                                                             \
             {0x4a525eu, 0x8a93a0u, 0x4a525eu, 0x4a525eu}}

static const struct gfx_style builtin[GFX_W_COUNT] =
    {
        [GFX_W_BASE] =
            {
                .font = {0, 15, GFX_WEIGHT_NORMAL, 0},
                .color = SAME4({0x1c2330u, 0xe6ebf2u, 0x05070au, 0x1c2330u}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x5a8fe0u}),
                .focus = FOCUS,
            },
        [GFX_W_BUTTON] =
            {
                .border = 1,
                .align = GFX_ALIGN_CENTER,
                .color = {{0x3a4658u, 0xe6ebf2u, 0x05070au, 0x4a5a70u},
                          {0x46546au, 0xffffffu, 0x05070au, 0x586a84u},
                          {0x2a3342u, 0xc8d0dcu, 0x05070au, 0x2a3342u},
                          {0x2a3038u, 0x636b78u, 0x05070au, 0x2a3038u}},
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_CHECKBOX] =
            {
                .part_w = GUI_CHECK_SIZE,
                .part_h = GUI_CHECK_SIZE,
                .gap = 6,
                .border = 1,
                CTL_COLORS(0x1c2330u, 0x2a3342u, 0x2a3038u),
                .focus = FOCUS,
            },
        [GFX_W_RADIO] =
            {
                .part_w = GUI_CHECK_SIZE,
                .part_h = GUI_CHECK_SIZE,
                .gap = 6,
                .border = 1,
                CTL_COLORS(0x1c2330u, 0x2a3342u, 0x2a3038u),
                .focus = FOCUS,
            },
        [GFX_W_SWITCH] =
            {
                .part_w = GUI_SWITCH_W,
                .part_h = GUI_SWITCH_H,
                .gap = 6,
                .border = 1,
                CTL_COLORS(0x3a4658u, 0x3a4658u, 0x2a3038u),
                .focus = FOCUS,
            },
        [GFX_W_TEXTFIELD] =
            {
                .padding = {0, 3, 0, 3},
                .border = 1,
                .color = SAME4({0xffffffu, 0x101418u, 0x3a3f48u, 0xffffffu}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_SELECT] =
            {
                .padding = {0, 3, 0, 3},
                .row_h = GUI_ROW_H,
                .border = 1,
                .color = SAME4({0xffffffu, 0x101418u, 0x3a3f48u, 0x5a6472u}),
                .accent = SAME4({0xdce8fcu, 0x101418u, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_PROGRESS] =
            {
                .border = 1,
                .color = SAME4({0x1c2330u, 0xc8d0dcu, 0x05070au, 0x1c2330u}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x5a8fe0u}),
                .focus = FOCUS,
            },
        [GFX_W_SLIDER] =
            {
                .part_w = 6,
                .part_h = GUI_SLIDER_H,
                .track = 6,
                .border = 1,
                .color = SAME4({0x1c2330u, 0xe6ebf2u, 0x05070au, 0x1c2330u}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_SCROLLBAR] =
            {
                .padding = {0, 1, 0, 1},
                .row_h = 12,
                .color = SAME4({0xc8ccd2u, 0x4a5564u, 0xc8ccd2u, 0xb4b9c2u}),
                .accent = SAME4({0x6b7686u, 0x6b7686u, 0x6b7686u, 0x6b7686u}),
                .focus = FOCUS,
            },
        [GFX_W_MENUBAR] =
            {
                .padding = {0, 3, 0, 3},
                .gap = 4,
                .color = {{0x232a36u, 0xe0e6eeu, 0x05070au, 0x232a36u},
                          {0x232a36u, 0xe0e6eeu, 0x05070au, 0x232a36u},
                          {0x3a4a60u, 0xe0e6eeu, 0x05070au, 0x3a4a60u},
                          {0x232a36u, 0x636b78u, 0x05070au, 0x232a36u}},
                .accent = SAME4({0x3a4a60u, 0xe0e6eeu, 0x05070au, 0x3a4a60u}),
                .focus = FOCUS,
            },
        [GFX_W_MENU] =
            {
                .padding = {0, 5, 0, 5},
                .width = GUI_MENU_W,
                .row_h = GUI_ROW_H,
                .border = 1,
                .gap = 2,
                .color = {{0x2a3342u, 0xe0e6eeu, 0x05070au, 0x8a93a0u},
                          {0x2a3342u, 0xe0e6eeu, 0x05070au, 0x8a93a0u},
                          {0x2a3342u, 0xe0e6eeu, 0x05070au, 0x8a93a0u},
                          {0x2a3342u, 0x636b78u, 0x05070au, 0x636b78u}},
                .accent = {{0x2a3342u, 0x8fd98fu, 0x05070au, 0x8fd98fu},
                           {0x2a3342u, 0x8fd98fu, 0x05070au, 0x8fd98fu},
                           {0x2a3342u, 0x8fd98fu, 0x05070au, 0x8fd98fu},
                           {0x2a3342u, 0x636b78u, 0x05070au, 0x636b78u}},
                .focus = FOCUS,
            },
        [GFX_W_TABBAR] =
            {
                .padding = {3, 0, 3, 6},
                .min_w = 46,
                .max_w = 168,
                .part_w = 16,
                .part_h = 16,
                .gap = 5,
                .color = {{0x2a3342u, 0xb8c0ccu, 0x1c2330u, 0x8a93a0u},
                          {0x2a3342u, 0xc8d0dcu, 0x1c2330u, 0x8a93a0u},
                          {0x2a3342u, 0xb8c0ccu, 0x1c2330u, 0x8a93a0u},
                          {0x2a3342u, 0x56606eu, 0x1c2330u, 0x8a93a0u}},
                .accent = SAME4({0xf4f5f7u, 0x101418u, 0x2f6bd8u, 0x707880u}),
                .focus = FOCUS,
            },
        [GFX_W_STATUS] =
            {
                .padding = {0, 6, 0, 6},
                .color = SAME4({0x161d28u, 0x9fb0c4u, 0x05070au, 0x161d28u}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_SIDEBAR] =
            {
                .border = 1,
                .color = SAME4({0x0e1218u, 0xe6ebf2u, 0x05070au, 0x33415au}),
                .accent = SAME4({0x24405cu, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
        [GFX_W_SEPARATOR] =
            {
                .border = 1,
                .color = SAME4({0x1c2330u, 0xe6ebf2u, 0x05070au, 0x3a4658u}),
                .accent = SAME4({0x2f6bd8u, 0xffffffu, 0x2f6bd8u, 0x2f6bd8u}),
                .focus = FOCUS,
            },
};

static struct gfx_style theme[GFX_W_COUNT];
static int theme_ready;

static void theme_init(void)
{
  if (theme_ready)
    return;
  for (int i = 0; i < GFX_W_COUNT; i++)
    theme[i] = builtin[i];
  theme_ready = 1;
}

static int kind_ok(int kind) { return kind >= 0 && kind < GFX_W_COUNT ? kind : GFX_W_BASE; }

const struct gfx_style *gfx_theme(int kind)
{
  theme_init();
  return &theme[kind_ok(kind)];
}

const struct gfx_style *gfx_theme_default(int kind) { return &builtin[kind_ok(kind)]; }

void gfx_theme_set(int kind, const struct gfx_style *st)
{
  theme_init();
  if (kind < 0 || kind >= GFX_W_COUNT)
    return;
  theme[kind] = st ? *st : builtin[kind];
}

const struct gfx_style *gfx_style_of(int kind, const struct gfx_style *st) { return st ? st : gfx_theme(kind); }

void gfx_font_resolve(const struct gfx_font *f, struct gfx_font *out)
{
  const struct gfx_font *base = &gfx_theme(GFX_W_BASE)->font;
  *out = f ? *f : *base;
  if (!out->family || !out->family[0])
    out->family = base->family;
  if (out->px <= 0)
    out->px = base->px > 0 ? base->px : builtin[GFX_W_BASE].font.px;
  if (out->weight <= 0)
    out->weight = base->weight > 0 ? base->weight : GFX_WEIGHT_NORMAL;
}

void gfx_style_font(const struct gfx_style *st, struct gfx_font *out) { gfx_font_resolve(st ? &st->font : 0, out); }

int gfx_style_text_h(const struct gfx_style *st)
{
  if (st->height > 0)
    return st->height;
  struct gfx_font f;
  gfx_style_font(st, &f);
  int bw = st->border > 0 ? st->border : 0;
  return gfx_font_height(&f) + st->padding[GFX_TOP] + st->padding[GFX_BOTTOM] + 2 * bw;
}

void gfx_sides(int *sides, int top, int right, int bottom, int left)
{
  sides[GFX_TOP] = top;
  sides[GFX_RIGHT] = right;
  sides[GFX_BOTTOM] = bottom;
  sides[GFX_LEFT] = left;
}

int gfx_state_index(int state)
{
  state &= ~GFX_FOCUSED;
  return state >= 0 && state < GFX_STATES ? state : BTN_NORMAL;
}

void gfx_style_box(const struct gfx_style *st, int x, int y, int w, int h, struct gfx_box *out)
{
  out->x = x + st->margin[GFX_LEFT];
  out->y = y + st->margin[GFX_TOP];
  out->w = st->width > 0 ? st->width : w - st->margin[GFX_LEFT] - st->margin[GFX_RIGHT];
  out->h = st->height > 0 ? st->height : h - st->margin[GFX_TOP] - st->margin[GFX_BOTTOM];
  if (out->w < 0)
    out->w = 0;
  if (out->h < 0)
    out->h = 0;
}

void gfx_style_content(const struct gfx_style *st, const struct gfx_box *b, struct gfx_box *out)
{
  int bw = st->border > 0 ? st->border : 0;
  out->x = b->x + bw + st->padding[GFX_LEFT];
  out->y = b->y + bw + st->padding[GFX_TOP];
  out->w = b->w - 2 * bw - st->padding[GFX_LEFT] - st->padding[GFX_RIGHT];
  out->h = b->h - 2 * bw - st->padding[GFX_TOP] - st->padding[GFX_BOTTOM];
  if (out->w < 0)
    out->w = 0;
  if (out->h < 0)
    out->h = 0;
}

int gfx_style_outer_w(const struct gfx_style *st, int w) { return w + st->margin[GFX_LEFT] + st->margin[GFX_RIGHT]; }

int gfx_style_outer_h(const struct gfx_style *st, int h) { return h + st->margin[GFX_TOP] + st->margin[GFX_BOTTOM]; }

/* How far row `row` of a w x h shape with corner radius r is cut in from each side */
static int round_cut(int r, int h, int row)
{
  int d = row < r ? r - row : row >= h - r ? row - (h - 1 - r) : 0, cut = 0;
  for (; d > 0 && (r - cut - 1) * (r - cut - 1) + d * d > r * r; cut++)
    ;
  return cut;
}

static int clamp_radius(int r, int w, int h)
{
  if (r * 2 > w)
    r = w / 2;
  if (r * 2 > h)
    r = h / 2;
  return r > 0 ? r : 0;
}

void gfx_fill_round(int x, int y, int w, int h, int r, unsigned color)
{
  if (w <= 0 || h <= 0)
    return;
  r = clamp_radius(r, w, h);
  if (r == 0) {
    gfx_fill_rect(x, y, w, h, color);
    return;
  }
  for (int row = 0; row < r; row++) {
    int cut = round_cut(r, h, row);
    gfx_fill_rect(x + cut, y + row, w - 2 * cut, 1, color);
    gfx_fill_rect(x + cut, y + h - 1 - row, w - 2 * cut, 1, color);
  }
  gfx_fill_rect(x, y + r, w, h - 2 * r, color);
}

void gfx_round_rect(int x, int y, int w, int h, int r, int t, unsigned color)
{
  if (w <= 0 || h <= 0 || t <= 0)
    return;
  if (2 * t >= w || 2 * t >= h) {
    gfx_fill_round(x, y, w, h, r, color);
    return;
  }
  r = clamp_radius(r, w, h);
  int ir = clamp_radius(r - t, w - 2 * t, h - 2 * t);
  for (int row = 0; row < h; row++) {
    int co = r ? round_cut(r, h, row) : 0, irow = row - t;
    if (irow < 0 || irow >= h - 2 * t) {
      gfx_fill_rect(x + co, y + row, w - 2 * co, 1, color);
      continue;
    }
    int ci = t + (ir ? round_cut(ir, h - 2 * t, irow) : 0);
    gfx_fill_rect(x + co, y + row, ci - co, 1, color);
    gfx_fill_rect(x + w - ci, y + row, ci - co, 1, color);
  }
}

void gfx_style_paint(const struct gfx_style *st, const struct gfx_box *b, const struct gfx_colors *c)
{
  int bw = st->border > 0 ? st->border : 0;
  if (b->w <= 0 || b->h <= 0)
    return;
  gfx_fill_round(b->x + bw, b->y + bw, b->w - 2 * bw, b->h - 2 * bw, st->radius - bw, c->bg);
  if (c->detail != c->bg && b->h > 2 * bw + 1) {
    int inset = st->radius > bw ? st->radius : bw;
    gfx_fill_rect(b->x + inset, b->y + bw, b->w - 2 * inset, 1, c->detail);
  }
  if (bw)
    gfx_round_rect(b->x, b->y, b->w, b->h, st->radius, bw, c->border);
}

void gfx_style_label(const struct gfx_style *st, const struct gfx_box *c, const char *s, unsigned fg)
{
  if (!s || !*s)
    return;
  struct gfx_font f;
  gfx_style_font(st, &f);
  int tw = gfx_font_width(s, &f);
  int ty = c->y + (c->h - gfx_font_height(&f)) / 2;
  if (tw > c->w) {
    gfx_font_text_clip(c->x, ty, s, fg, &f, c->w);
    return;
  }
  int tx = st->align == GFX_ALIGN_CENTER ? c->x + (c->w - tw) / 2
           : st->align == GFX_ALIGN_END  ? c->x + c->w - tw
                                         : c->x;
  gfx_font_text(tx, ty, s, fg, &f);
}

void gfx_focus_ring(int x, int y, int w, int h, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_BASE, st);
  if (st->flags & GFX_STYLE_NO_FOCUS)
    return;
  gfx_round_rect(x - 3, y - 3, w + 6, h + 6, st->radius > 0 ? st->radius + 3 : 0, 2, st->focus);
}
