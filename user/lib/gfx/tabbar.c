#include "gfx.h"

#define TAB_NEW_W 26
#define TAB_CLOSE_W 16
#define TAB_MAX_W 168
#define TAB_MIN_W 46

#define TAB_STRIP_BG 0x1c2330u
#define TAB_ON_BG 0xf4f5f7u
#define TAB_OFF_BG 0x2a3342u
#define TAB_ON_FG 0x101418u
#define TAB_OFF_FG 0xb8c0ccu
#define TAB_ACCENT 0x2f6bd8u

#define TAB_ARROW_W 18

/* Where the strip's parts land for one frame */
struct strip_geom {
  int tw;      /* width of each tab */
  int first;   /* first visible tab */
  int nvis;    /* tabs shown */
  int tabs_x;  /* left edge of the first visible tab */
  int arrows;  /* overflowing: scroll arrows flank the tabs */
  int right_x; /* the right arrow, when arrows is set */
  int new_x;   /* the "+" button */
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void strip_layout(const struct tabbar *tb, int x, int w, int n, struct strip_geom *g)
{
  if (n < 1)
    n = 1;
  if (n * TAB_MIN_W <= w - TAB_NEW_W) {
    g->arrows = 0;
    g->first = 0;
    g->nvis = n;
    g->tabs_x = x;
    g->tw = clampi((w - TAB_NEW_W) / n, TAB_MIN_W, TAB_MAX_W);
    g->right_x = 0;
    g->new_x = x + n * g->tw;
    return;
  }
  int avail = w - TAB_NEW_W - 2 * TAB_ARROW_W;
  g->arrows = 1;
  g->nvis = avail / TAB_MIN_W > 0 ? avail / TAB_MIN_W : 1;
  g->tw = avail / g->nvis > 0 ? avail / g->nvis : 1;
  g->first = clampi(tb->first, 0, n - g->nvis);
  g->tabs_x = x + TAB_ARROW_W;
  g->right_x = g->tabs_x + g->nvis * g->tw;
  g->new_x = g->right_x + TAB_ARROW_W;
}

static void tab_label(int x, int y, const char *s, int maxpx, unsigned fg)
{
  if (gfx_ui_width(s) <= maxpx) {
    gfx_ui_text(x, y, s, fg);
    return;
  }
  char buf[40];
  int k = 0;
  int dots = gfx_ui_cw_cp('.') * 2;
  int w = 0;
  for (int i = 0; s[i] && k < (int)sizeof buf - 3; i++) {
    int aw = gfx_ui_cw((unsigned char)s[i]);
    if (w + aw + dots > maxpx)
      break;
    buf[k++] = s[i];
    w += aw;
  }
  buf[k++] = '.';
  buf[k++] = '.';
  buf[k] = '\0';
  gfx_ui_text(x, y, buf, fg);
}

static void tab_closebox(int cx, int cy, unsigned c)
{
  for (int i = 0; i < 7; i++) {
    gfx_pixel(cx + i, cy + i, c);
    gfx_pixel(cx + 6 - i, cy + i, c);
  }
}

#define TAB_ICON 16
#define TAB_ICON_PAD 6
#define TAB_ICON_GAP 5

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

static void tab_arrow(int x, int y, int left, int enabled)
{
  unsigned c = enabled ? 0xc8d0dcu : 0x56606eu;
  int cx = x + TAB_ARROW_W / 2, cy = y + GUI_TAB_H / 2;
  for (int i = 0; i < 4; i++) {
    int px = left ? cx - 2 + i : cx + 1 - i;
    gfx_pixel(px, cy - i, c);
    gfx_pixel(px, cy + i, c);
  }
}

void tabbar_draw(struct tabbar *tb, int x, int y, int w, const char *const *labels, const unsigned *const *icons, int n,
                 int active)
{
  struct strip_geom g;
  strip_layout(tb, x, w, n, &g);
  if (active != tb->last_active) { /* bring a newly selected tab into view, but let the user scroll away */
    if (active < g.first)
      tb->first = active;
    else if (active >= g.first + g.nvis)
      tb->first = active - g.nvis + 1;
    tb->last_active = active;
    strip_layout(tb, x, w, n, &g);
  }
  tb->first = g.first;

  gfx_fill_rect(x, y, w, GUI_TAB_H, TAB_STRIP_BG);
  int tw = g.tw;
  int ty = y + (GUI_TAB_H - UIFONT_H) / 2;
  int iy = y + (GUI_TAB_H - TAB_ICON) / 2;
  for (int i = g.first; i < g.first + g.nvis && i < n; i++) {
    int tx = g.tabs_x + (i - g.first) * tw;
    int on = (i == active);
    unsigned bg = on ? TAB_ON_BG : TAB_OFF_BG;
    unsigned st = (on ? GFX_A11Y_SELECTED : 0) | (on && tb->focused ? GFX_A11Y_FOCUSED : 0);
    gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_TAB, labels[i], st, tx, y, tw - 1, GUI_TAB_H), i + 1, 0, n);
    gfx_fill_rect(tx, y, tw - 1, GUI_TAB_H, bg);
    if (on)
      gfx_fill_rect(tx, y, tw - 1, 2, TAB_ACCENT);
    if (on && tb->focused) { /* inside the tab: the strip has no room around it */
      gfx_rect(tx, y, tw - 1, GUI_TAB_H, TAB_ACCENT);
      gfx_rect(tx + 1, y + 1, tw - 3, GUI_TAB_H - 2, TAB_ACCENT);
    }
    gfx_fill_rect(tx + tw - 1, y, 1, GUI_TAB_H, TAB_STRIP_BG);

    int ix = tx + TAB_ICON_PAD;
    if (icons && icons[i])
      gfx_blit_argb(ix, iy, TAB_ICON, TAB_ICON, icons[i]);
    else
      tab_globe(ix, iy);

    int lx = ix + TAB_ICON + TAB_ICON_GAP;
    int textpx = tw - (lx - tx) - TAB_CLOSE_W;
    tab_label(lx, ty, labels[i], textpx, on ? TAB_ON_FG : TAB_OFF_FG);
    tab_closebox(tx + tw - TAB_CLOSE_W + 4, y + (GUI_TAB_H - 7) / 2, on ? 0x707880 : 0x8a93a0);
  }
  if (g.arrows) {
    tab_arrow(x, y, 1, g.first > 0);
    tab_arrow(g.right_x, y, 0, g.first + g.nvis < n);
  }
  gfx_a11y_add(GFX_ROLE_BUTTON, "New tab", 0, g.new_x, y, TAB_NEW_W, GUI_TAB_H);
  gfx_ui_text(g.new_x + (TAB_NEW_W - gfx_ui_width("+")) / 2, ty, "+", 0xc8d0dc);
}

void tabbar_scroll(struct tabbar *tb, int w, int n, int delta)
{
  struct strip_geom g;
  strip_layout(tb, 0, w, n, &g);
  tb->first = g.arrows ? clampi(g.first + delta, 0, n - g.nvis) : 0;
}

int tabbar_hit(struct tabbar *tb, int x, int y, int w, int n, int mx, int my, int *index)
{
  if (my < y || my >= y + GUI_TAB_H || mx < x)
    return TABBAR_NONE;
  struct strip_geom g;
  strip_layout(tb, x, w, n, &g);
  if (mx >= g.new_x && mx < g.new_x + TAB_NEW_W)
    return TABBAR_NEW;
  if (g.arrows && (mx < g.tabs_x || (mx >= g.right_x && mx < g.right_x + TAB_ARROW_W))) {
    tabbar_scroll(tb, w, n, mx < g.tabs_x ? -1 : 1);
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
