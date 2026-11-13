#include "gfx.h"

static const int ctl_kind[] = {
    [GFX_CTRL_CHECKBOX] = GFX_W_CHECKBOX, [GFX_CTRL_RADIO] = GFX_W_RADIO, [GFX_CTRL_SWITCH] = GFX_W_SWITCH};
static const int ctl_part_w[] = {
    [GFX_CTRL_CHECKBOX] = GUI_CHECK_SIZE, [GFX_CTRL_RADIO] = GUI_CHECK_SIZE, [GFX_CTRL_SWITCH] = GUI_SWITCH_W};
static const int ctl_part_h[] = {
    [GFX_CTRL_CHECKBOX] = GUI_CHECK_SIZE, [GFX_CTRL_RADIO] = GUI_CHECK_SIZE, [GFX_CTRL_SWITCH] = GUI_SWITCH_H};
static const int ctl_role[] = {
    [GFX_CTRL_CHECKBOX] = GFX_ROLE_CHECKBOX, [GFX_CTRL_RADIO] = GFX_ROLE_RADIO, [GFX_CTRL_SWITCH] = GFX_ROLE_SWITCH};

static int ctl_valid(int kind) { return kind >= GFX_CTRL_CHECKBOX && kind <= GFX_CTRL_SWITCH ? kind : 0; }

/* Where a control's parts land */
struct ctl_geom {
  struct gfx_box b;   /* border box */
  int pw, ph, px, py; /* the box, radio or switch */
  int lx, ly;         /* the label's top-left */
};

static int has_label(const char *label) { return label && *label; }

static void ctl_geom(int kind, const struct gfx_style *st, int x, int y, const char *label, struct ctl_geom *g)
{
  struct gfx_font f;
  gfx_style_font(st, &f);
  int fh = gfx_font_height(&f);
  g->pw = st->part_w > 0 ? st->part_w : ctl_part_w[kind];
  g->ph = st->part_h > 0 ? st->part_h : ctl_part_h[kind];
  int pl = st->padding[GFX_LEFT], pt = st->padding[GFX_TOP];
  g->b.x = x + st->margin[GFX_LEFT];
  g->b.y = y + st->margin[GFX_TOP];
  if (st->width > 0)
    g->b.w = st->width;
  else
    g->b.w = pl + g->pw + st->padding[GFX_RIGHT] + (has_label(label) ? st->gap + gfx_font_width(label, &f) : 0);
  if (st->height > 0)
    g->b.h = st->height;
  else
    g->b.h = (g->ph > fh ? g->ph : fh) + pt + st->padding[GFX_BOTTOM];
  int inner_h = g->b.h - pt - st->padding[GFX_BOTTOM];
  g->px = g->b.x + pl;
  g->py = g->b.y + pt + (inner_h - g->ph) / 2;
  g->lx = g->px + g->pw + st->gap;
  g->ly = g->b.y + pt + (inner_h - fh) / 2;
}

static void disc(int x, int y, int d, unsigned color)
{
  for (int py = 0; py < d; py++)
    for (int px = 0; px < d; px++) {
      int dx = 2 * px + 1 - d, dy = 2 * py + 1 - d;
      if (dx * dx + dy * dy <= d * d)
        gfx_pixel(x + px, y + py, color);
    }
}

/* A pill: two end discs joined by a band */
static void pill(int x, int y, int w, int h, unsigned color)
{
  if (w <= 0 || h <= 0)
    return;
  disc(x, y, h, color);
  disc(x + w - h, y, h, color);
  gfx_fill_rect(x + h / 2, y, w - h, h, color);
}

/* Draws the label and the focus ring, records the control's a11y node, and returns the state index to paint with. */
static int ctl_frame(int kind, const struct gfx_style *st, const struct ctl_geom *g, const char *label, int checked,
                     int state, int focused)
{
  unsigned a11y = gfx_a11y_state(state) | (focused ? GFX_A11Y_FOCUSED : 0) | (checked ? GFX_A11Y_CHECKED : 0);
  gfx_a11y_add(ctl_role[kind], label, a11y, g->b.x, g->b.y, g->b.w, g->b.h);
  int si = gfx_state_index(state);
  if (focused && !(st->flags & GFX_STYLE_NO_FOCUS))
    gfx_round_rect(g->px - 2, g->b.y, g->pw + 4, g->b.h, st->radius > 0 ? st->radius + 2 : 0, 1, st->focus);
  if (has_label(label)) {
    struct gfx_font f;
    gfx_style_font(st, &f);
    gfx_font_text(g->lx, g->ly, label, st->color[si].fg, &f);
  }
  return si;
}

/* The part's edge: the accent once on, else the state's border */
static unsigned ctl_edge(const struct gfx_style *st, int checked, int si)
{
  return checked && si != BTN_DISABLED ? st->accent[si].border : st->color[si].border;
}

static const struct gfx_style *ctl_style(int kind, const struct gfx_style *st)
{
  return gfx_style_of(ctl_kind[kind], st);
}

void checkbox_draw(int x, int y, const char *label, int checked, int state, int focused, const struct gfx_style *st)
{
  st = ctl_style(GFX_CTRL_CHECKBOX, st);
  focused = focused || (state & GFX_FOCUSED);
  struct ctl_geom g;
  ctl_geom(GFX_CTRL_CHECKBOX, st, x, y, label, &g);
  int si = ctl_frame(GFX_CTRL_CHECKBOX, st, &g, label, checked, state, focused);
  int bw = st->border > 0 ? st->border : 0;
  gfx_fill_round(g.px, g.py, g.pw, g.ph, st->radius, checked ? st->accent[si].bg : st->color[si].bg);
  if (bw)
    gfx_round_rect(g.px, g.py, g.pw, g.ph, st->radius, bw, ctl_edge(st, checked, si));
  if (!checked)
    return;
  /* The tick, laid out on a 14px grid and scaled to the box */
  int s = g.pw < g.ph ? g.pw : g.ph, t = 2 * s / 14 > 0 ? 2 * s / 14 : 1;
  int mx = g.px + (g.pw - s) / 2, my = g.py + (g.ph - s) / 2;
  unsigned mark = st->accent[si].fg;
  for (int i = 0; i < 4 * s / 14; i++)
    gfx_fill_rect(mx + 3 * s / 14 + i, my + 7 * s / 14 + i, 1, t, mark);
  for (int i = 0; i < 5 * s / 14; i++)
    gfx_fill_rect(mx + 7 * s / 14 + i, my + 9 * s / 14 - i, 1, t, mark);
}

void radio_draw(int x, int y, const char *label, int checked, int state, int focused, const struct gfx_style *st)
{
  st = ctl_style(GFX_CTRL_RADIO, st);
  focused = focused || (state & GFX_FOCUSED);
  struct ctl_geom g;
  ctl_geom(GFX_CTRL_RADIO, st, x, y, label, &g);
  int si = ctl_frame(GFX_CTRL_RADIO, st, &g, label, checked, state, focused);
  int bw = st->border > 0 ? st->border : 0;
  int d = g.pw < g.ph ? g.pw : g.ph, dx = g.px + (g.pw - d) / 2, dy = g.py + (g.ph - d) / 2;
  disc(dx, dy, d, ctl_edge(st, checked, si));
  disc(dx + bw, dy + bw, d - 2 * bw, st->color[si].bg);
  if (checked) {
    int in = 4 * d / 14;
    disc(dx + in, dy + in, d - 2 * in, st->accent[si].bg);
  }
}

void switch_draw(int x, int y, const char *label, int on, int state, int focused, const struct gfx_style *st)
{
  st = ctl_style(GFX_CTRL_SWITCH, st);
  focused = focused || (state & GFX_FOCUSED);
  struct ctl_geom g;
  ctl_geom(GFX_CTRL_SWITCH, st, x, y, label, &g);
  int si = ctl_frame(GFX_CTRL_SWITCH, st, &g, label, on, state, focused);
  int bw = st->border > 0 ? st->border : 0, w = g.pw, h = g.ph;
  pill(g.px, g.py, w, h, ctl_edge(st, on, si));
  pill(g.px + bw, g.py + bw, w - 2 * bw, h - 2 * bw, on ? st->accent[si].bg : st->color[si].bg);
  int td = h - 6;
  int tx = on ? g.px + w - 3 - td : g.px + 3;
  disc(tx, g.py + 3, td, st->accent[si].fg);
}

int control_width(int kind, const char *label, const struct gfx_style *st)
{
  kind = ctl_valid(kind);
  st = ctl_style(kind, st);
  struct ctl_geom g;
  ctl_geom(kind, st, 0, 0, label, &g);
  return gfx_style_outer_w(st, g.b.w);
}

int control_height(int kind, const struct gfx_style *st)
{
  kind = ctl_valid(kind);
  st = ctl_style(kind, st);
  struct ctl_geom g;
  ctl_geom(kind, st, 0, 0, 0, &g);
  return gfx_style_outer_h(st, g.b.h);
}

int control_hit(int kind, int x, int y, const char *label, int mx, int my, const struct gfx_style *st)
{
  kind = ctl_valid(kind);
  st = ctl_style(kind, st);
  struct ctl_geom g;
  ctl_geom(kind, st, x, y, label, &g);
  return mx >= g.b.x && mx < g.b.x + g.b.w && my >= g.b.y && my < g.b.y + g.b.h;
}
