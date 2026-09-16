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

static int tabbar_each_w(int w, int n)
{
  if (n < 1)
    n = 1;
  int avail = w - TAB_NEW_W;
  int tw = avail / n;
  if (tw > TAB_MAX_W)
    tw = TAB_MAX_W;
  if (tw < TAB_MIN_W)
    tw = TAB_MIN_W;
  return tw;
}

static int tabbar_new_x(int x, int w, int n, int tw)
{
  int nx = x + n * tw;
  if (nx > x + w - TAB_NEW_W)
    nx = x + w - TAB_NEW_W;
  return nx;
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

void tabbar_draw(int x, int y, int w, const char *const *labels, const unsigned *const *icons, int n, int active)
{
  gfx_fill_rect(x, y, w, GUI_TAB_H, TAB_STRIP_BG);
  int tw = tabbar_each_w(w, n);
  int ty = y + (GUI_TAB_H - UIFONT_H) / 2;
  int iy = y + (GUI_TAB_H - TAB_ICON) / 2;
  for (int i = 0; i < n; i++) {
    int tx = x + i * tw;
    int on = (i == active);
    unsigned bg = on ? TAB_ON_BG : TAB_OFF_BG;
    gfx_fill_rect(tx, y, tw - 1, GUI_TAB_H, bg);
    if (on)
      gfx_fill_rect(tx, y, tw - 1, 2, 0x2f6bd8);
    gfx_fill_rect(tx + tw - 1, y, 1, GUI_TAB_H, TAB_STRIP_BG);

    int ix = tx + TAB_ICON_PAD;
    if (icons && icons[i])
      gfx_blit_argb(ix, iy, TAB_ICON, TAB_ICON, icons[i]);
    else
      tab_globe(ix, iy);

    int lx = ix + TAB_ICON + TAB_ICON_GAP;
    int has_x = n > 1;
    int textpx = tw - (lx - tx) - (has_x ? TAB_CLOSE_W : 4);
    tab_label(lx, ty, labels[i], textpx, on ? TAB_ON_FG : TAB_OFF_FG);
    if (has_x)
      tab_closebox(tx + tw - TAB_CLOSE_W + 4, y + (GUI_TAB_H - 7) / 2, on ? 0x707880 : 0x8a93a0);
  }
  int nx = tabbar_new_x(x, w, n, tw);
  gfx_ui_text(nx + (TAB_NEW_W - gfx_ui_width("+")) / 2, ty, "+", 0xc8d0dc);
}

int tabbar_hit(int x, int y, int w, int n, int mx, int my, int *index)
{
  if (my < y || my >= y + GUI_TAB_H || mx < x)
    return TABBAR_NONE;
  int tw = tabbar_each_w(w, n);
  int nx = tabbar_new_x(x, w, n, tw);
  if (mx >= nx && mx < nx + TAB_NEW_W)
    return TABBAR_NEW;
  int i = (mx - x) / tw;
  if (i < 0 || i >= n)
    return TABBAR_NONE;
  int tx = x + i * tw;
  if (n > 1 && mx >= tx + tw - TAB_CLOSE_W) {
    if (index)
      *index = i;
    return TABBAR_CLOSE;
  }
  if (index)
    *index = i;
  return TABBAR_SELECT;
}
