#include "gfx.h"
#include "owos.h"

#define SEL_BG 0xffffffu
#define SEL_FG 0x101418u
#define SEL_BORDER 0x3a3f48u
#define SEL_ACCENT 0x2f6bd8u
#define SEL_HILITE 0xdce8fcu
#define SEL_CHEVRON 0x5a6472u
#define SEL_PAD 4

void select_init(struct select_box *s, const char *const *options, int count, int selected)
{
  s->options = options;
  s->count = count;
  s->selected = (selected >= 0 && selected < count) ? selected : 0;
  s->open = 0;
  s->hover = -1;
  s->focused = 0;
}

void select_draw(struct select_box *s, int x, int y, int w, int h)
{
  const char *lbl = (s->selected >= 0 && s->selected < s->count) ? s->options[s->selected] : "";
  unsigned st = GFX_A11Y_HASPOPUP | (s->open ? GFX_A11Y_EXPANDED : 0) | (s->focused ? GFX_A11Y_FOCUSED : 0);
  gfx_a11y_set_text(gfx_a11y_add(GFX_ROLE_SELECT, 0, st, x, y, w, h), lbl);

  if (s->focused)
    gfx_focus_ring(x, y, w, h);
  gfx_fill_rect(x, y, w, h, SEL_BG);
  gfx_rect(x, y, w, h, s->open ? SEL_ACCENT : SEL_BORDER);

  gfx_ui_text(x + SEL_PAD, y + (h - UIFONT_H) / 2, lbl, SEL_FG);

  int tx = x + w - 16, ty = y + (h - 4) / 2;
  for (int r = 0; r < 4; r++)
    gfx_fill_rect(tx + r, ty + r, 8 - 2 * r, 1, SEL_CHEVRON);

  if (s->open && s->count > 0) {
    int ly = y + h, lh = s->count * GUI_ROW_H;
    gfx_fill_rect(x, ly, w, lh, SEL_BG);
    for (int i = 0; i < s->count; i++) {
      int ry = ly + i * GUI_ROW_H;
      int lit = i == s->hover || (s->hover < 0 && i == s->selected);
      unsigned ost = (i == s->selected ? GFX_A11Y_SELECTED : 0) | (lit ? GFX_A11Y_HOVER : 0);
      gfx_a11y_set_value(gfx_a11y_add(GFX_ROLE_OPTION, s->options[i], ost, x, ry, w, GUI_ROW_H), i + 1, 0, s->count);
      if (lit)
        gfx_fill_rect(x + 1, ry, w - 2, GUI_ROW_H, SEL_HILITE);
      gfx_ui_text(x + SEL_PAD, ry + (GUI_ROW_H - UIFONT_H) / 2, s->options[i], SEL_FG);
    }
    gfx_rect(x, ly, w, lh, SEL_BORDER);
  }
}

int select_click(struct select_box *s, int x, int y, int w, int h, int mx, int my)
{
  int in_box = mx >= x && mx < x + w && my >= y && my < y + h;
  if (!s->open) {
    if (in_box) {
      s->open = 1;
      s->hover = s->selected;
      return -1;
    }
    return 0;
  }
  if (in_box) {
    s->open = 0;
    return -1;
  }
  int ly = y + h, lh = s->count * GUI_ROW_H;
  if (mx >= x && mx < x + w && my >= ly && my < ly + lh) {
    int row = (my - ly) / GUI_ROW_H;
    if (row >= 0 && row < s->count) {
      s->selected = row;
      s->open = 0;
      return 1;
    }
  }
  s->open = 0;
  return 0;
}

int select_motion(struct select_box *s, int x, int y, int w, int h, int mx, int my)
{
  if (!s->open)
    return 0;
  int ly = y + h, lh = s->count * GUI_ROW_H, nh = -1;
  if (mx >= x && mx < x + w && my >= ly && my < ly + lh)
    nh = (my - ly) / GUI_ROW_H;
  if (nh != s->hover) {
    s->hover = nh;
    return 1;
  }
  return 0;
}

int select_key(struct select_box *s, char key)
{
  unsigned char u = (unsigned char)key;
  if (s->count <= 0)
    return 0;
  if (u == KEY_UP || u == KEY_DOWN) {
    int dir = (u == KEY_DOWN) ? 1 : -1;
    if (s->open) {
      int h = (s->hover < 0) ? s->selected : s->hover;
      h += dir;
      if (h < 0)
        h = 0;
      if (h >= s->count)
        h = s->count - 1;
      s->hover = h;
      return -1;
    }
    int n = s->selected + dir;
    if (n < 0)
      n = 0;
    if (n >= s->count)
      n = s->count - 1;
    if (n != s->selected) {
      s->selected = n;
      return 1;
    }
    return -1;
  }
  if (u == KEY_HOME || u == KEY_END) {
    int n = (u == KEY_HOME) ? 0 : s->count - 1;
    if (s->open) {
      s->hover = n;
      return -1;
    }
    if (n != s->selected) {
      s->selected = n;
      return 1;
    }
    return -1;
  }
  if (key == '\n' || key == '\r' || key == ' ') {
    if (!s->open) {
      s->open = 1;
      s->hover = s->selected;
      return -1;
    }
    int h = (s->hover >= 0 && s->hover < s->count) ? s->hover : s->selected;
    s->selected = h;
    s->open = 0;
    return 1;
  }
  if (u == 27) {
    if (s->open) {
      s->open = 0;
      return -1;
    }
    return 0;
  }
  return 0;
}
