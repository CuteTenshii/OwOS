#include "gfx.h"

#define SB_GRIP_DOT 0x33415au

static int sb_clamp(const struct sidebar *sb, int w)
{
  if (w < sb->min_w)
    w = sb->min_w;
  if (w > sb->max_w)
    w = sb->max_w;
  return w;
}

void sidebar_init(struct sidebar *sb, int w, int min_w, int max_w)
{
  if (max_w < min_w)
    max_w = min_w;
  sb->min_w = min_w;
  sb->max_w = max_w;
  sb->resizable = 1;
  sb->dragging = 0;
  sb->w = sb_clamp(sb, w);
}

void sidebar_set_width(struct sidebar *sb, int w) { sb->w = sb_clamp(sb, w); }

void sidebar_draw(struct sidebar *sb, int x, int y, int w, int h, unsigned bg, unsigned divider)
{
  gfx_fill_rect(x, y, w, h, bg);
  gfx_fill_rect(x + w, y, 1, h, divider);
  if (sb->resizable && h > 24) {
    int cy = y + h / 2 - 9;
    for (int i = 0; i < 4; i++)
      gfx_fill_rect(x + w - 1, cy + i * 5, 2, 2, SB_GRIP_DOT);
  }
}

int sidebar_press(struct sidebar *sb, int x, int y, int w, int h, int mx, int my)
{
  sb->dragging = 0;
  if (!sb->resizable)
    return 0;
  int gx = x + w;
  if (mx >= gx - SIDEBAR_GRIP / 2 && mx <= gx + SIDEBAR_GRIP / 2 && my >= y && my < y + h) {
    sb->dragging = 1;
    return 1;
  }
  return 0;
}

int sidebar_drag(struct sidebar *sb, int x, int mx)
{
  if (!sb->dragging)
    return 0;
  int nw = sb_clamp(sb, mx - x);
  if (nw == sb->w)
    return 0;
  sb->w = nw;
  return 1;
}

void gfx_separator(int x, int y, int len, int vertical)
{
  if (len <= 0)
    return;
  if (vertical) {
    gfx_fill_rect(x, y, 1, len, 0x05070au);
    gfx_fill_rect(x + 1, y, 1, len, 0x3a4658u);
  } else {
    gfx_fill_rect(x, y, len, 1, 0x05070au);
    gfx_fill_rect(x, y + 1, len, 1, 0x3a4658u);
  }
}
