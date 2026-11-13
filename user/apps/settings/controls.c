/* controls.c, dropdowns and the level slider of the active category */
#include "settings.h"

static int ctrl_box_w(int cw)
{
  int w = 220;
  if (w > cw - 32)
    w = cw - 32;
  if (w < 80)
    w = 80;
  return w;
}

struct control {
  const char *label, *hint;
  struct select_box *sel;
  void (*apply)(int idx);
};
static struct control ctrls[3];
static int nctrl;

/* Per-control vertical geometry: label, then the box, then a dim hint under it */
#define CONTROL_PITCH 84
static int ctrl_label_y(int i) { return CONTROLS_Y0 + i * CONTROL_PITCH; }
static int ctrl_box_y(int i) { return ctrl_label_y(i) + 24; }
static int ctrl_hint_y(int i) { return ctrl_box_y(i) + GUI_SELECT_H + 8; }

/* Level slider (0..100) bound to a sysfs node; hidden when the node is unreadable */
static const char *slider_label, *slider_path;
static int slider_val;
static bool slider_grab;

static int slider_y(void) { return ctrl_label_y(nctrl ? nctrl : 1); }
static int slider_box_y(void) { return slider_y() + 26; }

static void slider_load(const char *label, const char *path)
{
  char t[8];
  slider_grab = false;
  slider_label = slider_path = 0;
  if (read_proc(path, t, sizeof t) <= 0)
    return;
  slider_val = patoi(t);
  slider_label = label;
  slider_path = path;
}

static void slider_set(int val)
{
  char t[8];
  if (val < 0)
    val = 0;
  if (val > 100)
    val = 100;
  uitoa((unsigned)val, t);
  if (writefile(slider_path, t, strlen(t)) == 0)
    slider_val = val;
}

static void slider_at(int mx, int cx, int cw)
{
  int bw = ctrl_box_w(cw);
  slider_set(slider_value(cx + 16, bw, mx, 100, NULL));
}

static void draw_slider(int cx, int cw)
{
  if (!slider_path)
    return;
  int bw = ctrl_box_w(cw), x = cx + 16;
  char t[8];
  uitoa((unsigned)slider_val, t);
  strcat(t, "%");
  gfx_ui_text(x, slider_y(), slider_label, FG_TEXT);
  gfx_ui_text(x + bw - gfx_ui_width(t), slider_y(), t, FG_DIM);
  slider_draw(x, slider_box_y(), bw, slider_val, 100, 0, NULL);
  gfx_a11y_set_name(gfx_a11y_last(), slider_label);
}

void controls_add(const char *label, const char *hint, struct select_box *box, void (*apply)(int))
{
  if (nctrl >= (int)(sizeof ctrls / sizeof ctrls[0]))
    return;
  ctrls[nctrl].label = label;
  ctrls[nctrl].hint = hint;
  ctrls[nctrl].sel = box;
  ctrls[nctrl].apply = apply;
  nctrl++;
}

/* Populate ctrls[] for the active category from its freshly loaded state */
void controls_build(int cat)
{
  nctrl = 0;
  slider_path = 0;
  dev_controls(cat);
  if (cat == CAT_SOUND)
    slider_load("Volume", "/sys/class/sound/card0/volume");
  else if (cat == CAT_DISPLAY)
    slider_load("Brightness", "/sys/class/graphics/fb0/brightness");
}

/* Draw the active category's controls: labels and hints first, then the boxes */
void controls_draw(int cx, int cw)
{
  if (nctrl == 0) {
    const char *msg = sel == CAT_SOUND     ? "No audio output device."
                      : sel == CAT_DISPLAY ? "No framebuffer."
                      : sel == CAT_FONTS   ? "No fonts installed."
                                           : "No options available.";
    gfx_ui_text(cx + 16, CONTROLS_Y0, msg, FG_DIM);
    draw_slider(cx, cw);
    return;
  }
  int bw = ctrl_box_w(cw), open = -1;
  for (int i = 0; i < nctrl; i++) {
    gfx_ui_text(cx + 16, ctrl_label_y(i), ctrls[i].label, FG_TEXT);
    if (ctrls[i].hint)
      gfx_ui_text(cx + 16, ctrl_hint_y(i), ctrls[i].hint, FG_DIM);
  }
  for (int i = 0; i < nctrl; i++) {
    if (ctrls[i].sel->open) {
      open = i;
      continue;
    }
    select_draw(ctrls[i].sel, cx + 16, ctrl_box_y(i), bw, GUI_SELECT_H, NULL);
  }
  if (open >= 0)
    select_draw(ctrls[open].sel, cx + 16, ctrl_box_y(open), bw, GUI_SELECT_H, NULL);
  else
    draw_slider(cx, cw);
}

/* Route a press in the detail pane: the open dropdown first, then the slider and the rest */
void controls_click(int cx, int cw, int mx, int my)
{
  int bw = ctrl_box_w(cw), opened = -1;
  for (int i = 0; i < nctrl; i++)
    if (ctrls[i].sel->open)
      opened = i;

  slider_grab = false;
  if (opened < 0 && slider_path && slider_hit(cx + 16, slider_box_y(), bw, mx, my, NULL)) {
    slider_grab = true;
    slider_at(mx, cx, cw);
    seq++;
    return;
  }

  int order[3], no = 0;
  if (opened >= 0)
    order[no++] = opened;
  for (int i = 0; i < nctrl; i++)
    if (i != opened)
      order[no++] = i;

  for (int k = 0; k < no; k++) {
    int i = order[k];
    int r = select_click(ctrls[i].sel, cx + 16, ctrl_box_y(i), bw, GUI_SELECT_H, mx, my, NULL);
    if (r == 0)
      continue;
    if (r == 1)
      ctrls[i].apply(ctrls[i].sel->selected);
    for (int j = 0; j < nctrl; j++)
      if (j != i)
        ctrls[j].sel->open = 0;
    seq++;
    return;
  }
}

/* Track hover for whichever dropdown is open */
void controls_move(int cx, int cw, int mx, int my)
{
  int bw = ctrl_box_w(cw);
  for (int i = 0; i < nctrl; i++) {
    if (!ctrls[i].sel->open)
      continue;
    if (select_motion(ctrls[i].sel, cx + 16, ctrl_box_y(i), bw, GUI_SELECT_H, mx, my, NULL))
      seq++;
    break;
  }
}

/* An open dropdown captures keys; true if it consumed this one */
bool controls_key(int k)
{
  for (int i = 0; i < nctrl; i++) {
    if (!ctrls[i].sel->open)
      continue;
    int r = select_key(ctrls[i].sel, (char)k);
    if (r == 1) {
      ctrls[i].apply(ctrls[i].sel->selected);
      seq++;
      return true;
    } else if (r == -1) {
      seq++;
      return true;
    }
    break; /* open but unhandled: fall through to nav */
  }
  return false;
}

/* Move a grabbed slider; true if one was grabbed */
bool controls_drag(int mx, int cx, int cw)
{
  if (!slider_grab)
    return false;
  slider_at(mx, cx, cw);
  seq++;
  return true;
}
