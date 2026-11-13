#include "gfx.h"

#define TAB_NEW_W 26
#define TAB_CLOSE_W 16
#define TAB_ARROW_W 18
#define TAB_STRIPE 2 /* the active tab's top stripe */

/* Where the strip's parts land for one frame */
struct strip_geom {
  struct gfx_box b; /* the strip's border box */
  int min_w, max_w;
  int tw;      /* width of each tab */
  int first;   /* first visible tab */
  int nvis;    /* tabs shown */
  int tabs_x;  /* left edge of the first visible tab */
  int arrows;  /* overflowing: scroll arrows flank the tabs */
  int right_x; /* the right arrow, when arrows is set */
  int new_x;   /* the "+" button */
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int tabbar_height(const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TABBAR, st);
  return gfx_style_outer_h(st, gfx_style_text_h(st));
}

static void strip_layout(const struct tabbar *tb, const struct gfx_style *st, int x, int y, int w, int n,
                         struct strip_geom *g)
{
  gfx_style_box(st, x, y, w, tabbar_height(st), &g->b);
  x = g->b.x;
  w = g->b.w;
  g->min_w = st->min_w > 0 ? st->min_w : 1;
  g->max_w = st->max_w >= g->min_w ? st->max_w : 1 << 20;
  if (n < 1)
    n = 1;
  if (n * g->min_w <= w - TAB_NEW_W) {
    g->arrows = 0;
    g->first = 0;
    g->nvis = n;
    g->tabs_x = x;
    g->tw = clampi((w - TAB_NEW_W) / n, g->min_w, g->max_w);
    g->right_x = 0;
    g->new_x = x + n * g->tw;
    return;
  }
  int avail = w - TAB_NEW_W - 2 * TAB_ARROW_W;
  g->arrows = 1;
  g->nvis = avail / g->min_w > 0 ? avail / g->min_w : 1;
  g->tw = avail / g->nvis > 0 ? avail / g->nvis : 1;
  g->first = clampi(tb->first, 0, n - g->nvis);
  g->tabs_x = x + TAB_ARROW_W;
  g->right_x = g->tabs_x + g->nvis * g->tw;
  g->new_x = g->right_x + TAB_ARROW_W;
}

static void tab_closebox(int cx, int cy, unsigned c)
{
  for (int i = 0; i < 7; i++) {
    gfx_pixel(cx + i, cy + i, c);
    gfx_pixel(cx + 6 - i, cy + i, c);
  }
}

static int isqrt_i(int v)
{
  int r = 0;
  if (v < 0)
    return 0;
  while ((r + 1) * (r + 1) <= v)
    r++;
  return r;
}

static void tab_globe(int x, int y)
{
  const int R = 7, cx = 8, cy = 8;
  for (int py = 0; py < 16; py++) {
    int dy = py - cy;
    int e = isqrt_i(R * R - dy * dy);
    for (int px = 0; px < 16; px++) {
      int dx = px - cx, d2 = dx * dx + dy * dy;
      if (d2 > R * R)
        continue;
      unsigned col;
      if (d2 >= (R - 1) * (R - 1))
        col = 0x2f5fb0u;
      else if (dy == 0 || dy == 3 || dy == -3)
        col = 0xcfe0ffu;
      else if (dx == 0 || dx == e / 2 || dx == -e / 2)
        col = 0xcfe0ffu;
      else
        col = 0x5b8defu;
      gfx_pixel(x + px, y + py, col);
    }
  }
}

static void tab_arrow(int x, int y, int h, int left, unsigned c)
{
  int cx = x + TAB_ARROW_W / 2, cy = y + h / 2;
  for (int i = 0; i < 4; i++) {
    int px = left ? cx - 2 + i : cx + 1 - i;
    gfx_pixel(px, cy - i, c);
    gfx_pixel(px, cy + i, c);
  }
}

void tabbar_draw(struct tabbar *tb, int x, int y, int w, const char *const *labels, const unsigned *const *icons, int n,
                 int active, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TABBAR, st);
  struct strip_geom g;
  strip_layout(tb, st, x, y, w, n, &g);
  if (active != tb->last_active) { /* bring a newly selected tab into view, but let the user scroll away */
    if (active < g.first)
      tb->first = active;
    else if (active >= g.first + g.nvis)
      tb->first = active - g.nvis + 1;
    tb->last_active = active;
    strip_layout(tb, st, x, y, w, n, &g);
  }
  tb->first = g.first;

  struct gfx_font f;
  gfx_style_font(st, &f);
  const struct gfx_colors *off = &st->color[BTN_NORMAL], *on_c = &st->accent[BTN_NORMAL];
  int sy = g.b.y, sh = g.b.h;
  int icon = st->part_w > 0 ? st->part_w : 16;
  int pt = st->padding[GFX_TOP], ch = sh - pt - st->padding[GFX_BOTTOM];
  int ty = sy + pt + (ch - gfx_font_height(&f)) / 2;
  int iy = sy + pt + (ch - icon) / 2;
  gfx_fill_rect(g.b.x, sy, g.b.w, sh, off->border);
  int tw = g.tw;
  for (int i = g.first; i < g.first + g.nvis && i < n; i++) {
    int tx = g.tabs_x + (i - g.first) * tw;
    int on = (i == active);
    const struct gfx_colors *c = on ? on_c : off;
    unsigned a11y = (on ? GFX_A11Y_SELECTED : 0) | (on && tb->focused ? GFX_A11Y_FOCUSED : 0);
    gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_TAB, labels[i], a11y, tx, sy, tw - 1, sh), i + 1, 0, n);
    gfx_fill_round(tx, sy, tw - 1, sh, st->radius, c->bg);
    if (on)
      gfx_fill_rect(tx + st->radius, sy, tw - 1 - 2 * st->radius, TAB_STRIPE, on_c->border);
    if (on && tb->focused && !(st->flags & GFX_STYLE_NO_FOCUS)) /* inside the tab: the strip has no room around it */
      gfx_round_rect(tx, sy, tw - 1, sh, st->radius, 2, on_c->border);

    int ix = tx + st->padding[GFX_LEFT];
    if (icons && icons[i])
      gfx_blit_argb(ix, iy, 16, 16, icons[i]);
    else
      tab_globe(ix, iy);

    int lx = ix + icon + st->gap;
    gfx_font_text_clip(lx, ty, labels[i], c->fg, &f, tw - (lx - tx) - TAB_CLOSE_W);
    tab_closebox(tx + tw - TAB_CLOSE_W + 4, sy + (sh - 7) / 2, c->detail);
  }
  if (g.arrows) {
    tab_arrow(g.b.x, sy, sh, 1, st->color[g.first > 0 ? BTN_HOVER : BTN_DISABLED].fg);
    tab_arrow(g.right_x, sy, sh, 0, st->color[g.first + g.nvis < n ? BTN_HOVER : BTN_DISABLED].fg);
  }
  gfx_a11y_add(GFX_ROLE_BUTTON, "New tab", 0, g.new_x, sy, TAB_NEW_W, sh);
  gfx_font_text(g.new_x + (TAB_NEW_W - gfx_font_width("+", &f)) / 2, ty, "+", st->color[BTN_HOVER].fg, &f);
}

void tabbar_scroll(struct tabbar *tb, int w, int n, int delta, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TABBAR, st);
  struct strip_geom g;
  strip_layout(tb, st, 0, 0, w, n, &g);
  tb->first = g.arrows ? clampi(g.first + delta, 0, n - g.nvis) : 0;
}

int tabbar_hit(struct tabbar *tb, int x, int y, int w, int n, int mx, int my, int *index, const struct gfx_style *st)
{
  st = gfx_style_of(GFX_W_TABBAR, st);
  struct strip_geom g;
  strip_layout(tb, st, x, y, w, n, &g);
  if (my < g.b.y || my >= g.b.y + g.b.h || mx < g.b.x)
    return TABBAR_NONE;
  if (mx >= g.new_x && mx < g.new_x + TAB_NEW_W)
    return TABBAR_NEW;
  if (g.arrows && (mx < g.tabs_x || (mx >= g.right_x && mx < g.right_x + TAB_ARROW_W))) {
    tabbar_scroll(tb, w, n, mx < g.tabs_x ? -1 : 1, st);
    return TABBAR_SCROLL;
  }
  if (mx < g.tabs_x)
    return TABBAR_NONE;
  int slot = (mx - g.tabs_x) / g.tw, i = g.first + slot;
  if (slot >= g.nvis || i >= n)
    return TABBAR_NONE;
  int tx = g.tabs_x + slot * g.tw;
  if (index)
    *index = i;
  if (mx >= tx + g.tw - TAB_CLOSE_W)
    return TABBAR_CLOSE;
  return TABBAR_SELECT;
}
