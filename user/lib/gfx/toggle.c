#include "gfx.h"

#define CTL_GAP 6
#define CTL_LABEL 0xe6ebf2u
#define CTL_LABEL_OFF 0x636b78u
#define CTL_BG 0x1c2330u
#define CTL_BG_PRESSED 0x2a3342u
#define CTL_BG_OFF 0x2a3038u
#define CTL_EDGE 0x7a8696u
#define CTL_EDGE_HOVER 0xa8b4c6u
#define CTL_EDGE_OFF 0x4a525eu
#define CTL_ON 0x2f6bd8u
#define CTL_ON_PRESSED 0x2559b8u
#define CTL_MARK 0xffffffu
#define CTL_MARK_OFF 0x8a93a0u
#define CTL_TRACK 0x3a4658u

static const int ctl_box_w[] = {
    [GFX_CTRL_CHECKBOX] = GUI_CHECK_SIZE, [GFX_CTRL_RADIO] = GUI_CHECK_SIZE, [GFX_CTRL_SWITCH] = GUI_SWITCH_W};
static const int ctl_box_h[] = {
    [GFX_CTRL_CHECKBOX] = GUI_CHECK_SIZE, [GFX_CTRL_RADIO] = GUI_CHECK_SIZE, [GFX_CTRL_SWITCH] = GUI_SWITCH_H};

static void disc(int x, int y, int d, unsigned color)
{
  for (int py = 0; py < d; py++)
    for (int px = 0; px < d; px++) {
      int dx = 2 * px + 1 - d, dy = 2 * py + 1 - d;
      if (dx * dx + dy * dy <= d * d)
        gfx_pixel(x + px, y + py, color);
    }
}

static const int ctl_role[] = {
    [GFX_CTRL_CHECKBOX] = GFX_ROLE_CHECKBOX, [GFX_CTRL_RADIO] = GFX_ROLE_RADIO, [GFX_CTRL_SWITCH] = GFX_ROLE_SWITCH};

/* Draws the label and the focus ring, records the control's a11y node, and returns the box's top-left y. */
static int ctl_frame(int kind, int x, int y, const char *label, int checked, int state, int focused)
{
  int bw = ctl_box_w[kind], by = y + (GUI_CTRL_H - ctl_box_h[kind]) / 2;
  unsigned a11y = gfx_a11y_state(state) | (focused ? GFX_A11Y_FOCUSED : 0) | (checked ? GFX_A11Y_CHECKED : 0);
  gfx_a11y_add(ctl_role[kind], label, a11y, x, y, control_width(kind, label), GUI_CTRL_H);
  if (focused)
    gfx_rect(x - 2, y, bw + 4, GUI_CTRL_H, GFX_FOCUS_COLOR);
  if (label && *label)
    gfx_ui_text(x + bw + CTL_GAP, y + (GUI_CTRL_H - UIFONT_H) / 2, label,
                state == BTN_DISABLED ? CTL_LABEL_OFF : CTL_LABEL);
  return by;
}

static unsigned ctl_edge(int checked, int state)
{
  return state == BTN_DISABLED ? CTL_EDGE_OFF : checked ? CTL_ON : state == BTN_NORMAL ? CTL_EDGE : CTL_EDGE_HOVER;
}

static unsigned ctl_on(int state)
{
  return state == BTN_DISABLED ? CTL_EDGE_OFF : state == BTN_PRESSED ? CTL_ON_PRESSED : CTL_ON;
}

void checkbox_draw(int x, int y, const char *label, int checked, int state, int focused)
{
  focused = focused || (state & GFX_FOCUSED);
  state &= ~GFX_FOCUSED;
  int by = ctl_frame(GFX_CTRL_CHECKBOX, x, y, label, checked, state, focused);
  unsigned fill = checked                 ? ctl_on(state)
                  : state == BTN_DISABLED ? CTL_BG_OFF
                  : state == BTN_PRESSED  ? CTL_BG_PRESSED
                                          : CTL_BG;
  gfx_fill_rect(x, by, GUI_CHECK_SIZE, GUI_CHECK_SIZE, fill);
  gfx_rect(x, by, GUI_CHECK_SIZE, GUI_CHECK_SIZE, ctl_edge(checked, state));
  if (!checked)
    return;
  unsigned mark = state == BTN_DISABLED ? CTL_MARK_OFF : CTL_MARK;
  for (int i = 0; i < 4; i++)
    gfx_fill_rect(x + 3 + i, by + 7 + i, 1, 2, mark);
  for (int i = 0; i < 5; i++)
    gfx_fill_rect(x + 7 + i, by + 9 - i, 1, 2, mark);
}

void radio_draw(int x, int y, const char *label, int checked, int state, int focused)
{
  focused = focused || (state & GFX_FOCUSED);
  state &= ~GFX_FOCUSED;
  int by = ctl_frame(GFX_CTRL_RADIO, x, y, label, checked, state, focused);
  disc(x, by, GUI_CHECK_SIZE, ctl_edge(checked, state));
  disc(x + 1, by + 1, GUI_CHECK_SIZE - 2,
       state == BTN_DISABLED  ? CTL_BG_OFF
       : state == BTN_PRESSED ? CTL_BG_PRESSED
                              : CTL_BG);
  if (checked)
    disc(x + 4, by + 4, GUI_CHECK_SIZE - 8, ctl_on(state));
}

void switch_draw(int x, int y, const char *label, int on, int state, int focused)
{
  focused = focused || (state & GFX_FOCUSED);
  state &= ~GFX_FOCUSED;
  int by = ctl_frame(GFX_CTRL_SWITCH, x, y, label, on, state, focused);
  int h = GUI_SWITCH_H, w = GUI_SWITCH_W;
  unsigned edge = ctl_edge(on, state);
  unsigned track = on ? ctl_on(state) : state == BTN_DISABLED ? CTL_BG_OFF : CTL_TRACK;
  disc(x, by, h, edge);
  disc(x + w - h, by, h, edge);
  gfx_fill_rect(x + h / 2, by, w - h, h, edge);
  disc(x + 1, by + 1, h - 2, track);
  disc(x + w - h + 1, by + 1, h - 2, track);
  gfx_fill_rect(x + h / 2, by + 1, w - h, h - 2, track);
  int td = h - 6;
  int tx = on ? x + w - 3 - td : x + 3;
  disc(tx, by + 3, td, state == BTN_DISABLED ? CTL_MARK_OFF : CTL_MARK);
}

int control_width(int kind, const char *label)
{
  int w = ctl_box_w[kind];
  return label && *label ? w + CTL_GAP + gfx_ui_width(label) : w;
}

int control_hit(int kind, int x, int y, const char *label, int mx, int my)
{
  return mx >= x && mx < x + control_width(kind, label) && my >= y && my < y + GUI_CTRL_H;
}
