/* settings.c, a desktop Settings app for OwOS */
#include "settings.h"

int W = 480, H = 320;
int sel = CAT_MOUSE;
unsigned seq = 1;

static struct sidebar sb;

static const char *const cat_name[NCATS] = {
    "Mouse / Touchpad", "Keyboard", "Sound", "Display", "Fonts", "Updates", "About",
};

/* Re-read the state the given category displays so it reflects the live kernel value */
static void load_category(int cat)
{
  if (cat == CAT_ABOUT)
    about_load();
  else
    dev_load(cat);
  controls_build(cat);
}

/* layout / paint */

/* Width actually rendered this frame: the widget property */
static int sidebar_w(void)
{
  int sbw = sb.w;
  if (sbw > W - 200)
    sbw = W - 200;
  if (sbw < 0)
    sbw = 0;
  return sbw;
}

static void render(void)
{
  int sbw = sidebar_w();
  gfx_clear(BG_PANEL);

  /* Sidebar: the widget paints the panel base, divider and resize grip */
  sidebar_draw(&sb, 0, 0, sbw, H, NULL);
  gfx_fill_rect(0, 0, sbw, HEADER_H, BG_HEADER);
  gfx_ui_text(8, 2, "Settings", FG_TITLE);
  for (int i = 0; i < NCATS; i++) {
    int ry = HEADER_H + i * ROW_H;
    int here = (i == sel);
    if (here)
      gfx_fill_rect(0, ry, sbw, ROW_H, BG_SEL);
    gfx_ui_text(8, ry + (ROW_H - UIFONT_H) / 2, cat_name[i], here ? FG_WHITE : FG_TEXT);
  }

  /* Detail pane: About renders its own text block */
  int cx = sbw + 1, cw = W - sbw - 1;
  gfx_fill_rect(cx, 0, cw, H, BG_PANEL);
  if (sel == CAT_ABOUT) {
    about_draw(cx, cw);
  } else if (sel == CAT_UPDATES) {
    updates_draw(cx, cw);
  } else {
    gfx_ui_text(cx + 16, 16, cat_name[sel], FG_WHITE);
    controls_draw(cx, cw);
  }
}

/* input */

static void click(int mx, int my)
{
  int sbw = sidebar_w();

  /* Divider grip: grab it to resize the sidebar (consumes the press) */
  if (sidebar_press(&sb, 0, 0, sbw, H, mx, my)) {
    seq++;
    return;
  }

  if (mx < sbw) { /* a category row */
    if (my < HEADER_H)
      return;
    int i = (my - HEADER_H) / ROW_H;
    if (i >= 0 && i < NCATS && i != sel) {
      sel = i;
      load_category(sel);
      seq++;
    }
    return;
  }

  if (sel == CAT_UPDATES)
    updates_click(sbw + 1, mx, my);
  else
    controls_click(sbw + 1, W - sbw - 1, mx, my);
}

static void drag(int mx)
{
  int cx = sidebar_w() + 1;
  if (controls_drag(mx, cx, W - cx))
    return;
  if (sidebar_drag(&sb, 0, mx))
    seq++;
}

/* Up/Down move the selected category; an open dropdown captures keys instead */
static void key(int k)
{
  if (controls_key(k))
    return;
  if (k == KEY_UP || k == KEY_DOWN) {
    sel += (k == KEY_DOWN) ? 1 : -1;
    if (sel < 0)
      sel = NCATS - 1;
    if (sel >= NCATS)
      sel = 0;
    load_category(sel);
    seq++;
  }
}

int main(void)
{
  int id = win_create(W, H, "Settings");
  if (id < 0) {
    puts("settings: no window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  sidebar_init(&sb, SIDEBAR_W, SIDEBAR_MIN, SIDEBAR_MAX);
  win_resizable(id, 1);

  /* Prime every category's state once so switching is instant */
  for (int c = 0; c < NCATS; c++)
    load_category(c);
  controls_build(sel);

  int last = -1;
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      if (ev.type == WEV_CLOSE) {
        win_close(id);
        return 0;
      } else if (ev.type == WEV_MOUSE)
        click(ev.a, ev.b);
      else if (ev.type == WEV_DRAG)
        drag(ev.a);
      else if (ev.type == WEV_MOVE) {
        int sbw = sidebar_w();
        controls_move(sbw + 1, W - sbw - 1, ev.a, ev.b);
      } else if (ev.type == WEV_KEY)
        key(ev.a);
      else if (ev.type == WEV_RESIZE) {
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H);
        seq++;
      }
    }
    updates_poll();
    if ((int)seq != last) {
      render();
      win_present(id);
      last = seq;
    }
    win_wait(id);
  }
}
