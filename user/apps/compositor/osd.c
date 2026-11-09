/* osd.c: volume/brightness OSD and the power overlay */
#include "comp.h"

/* volume / brightness on-screen display */
int osd_kind, osd_level = -1; /* level 0..100, -1 = unreadable */
static bool osd_muted, osd_dirty;
static unsigned osd_until;

int osd_x(void) { return (screen_w - OSD_W) / 2; }
int osd_y(void) { return screen_h - WM_BAR_H - OSD_H - 24; }

static int sysfs_int(const char *path)
{
  char b[16];
  int n = readfile(path, b, sizeof b - 1);
  if (n <= 0)
    return -1;
  int v = 0, digits = 0;
  for (int i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++, digits++)
    v = v * 10 + (b[i] - '0');
  return digits ? v : -1;
}

void osd_show(int kind)
{
  if (kind == OSD_VOLUME) {
    osd_level = sysfs_int("/sys/class/sound/card0/volume");
    osd_muted = sysfs_int("/sys/class/sound/card0/muted") > 0;
  } else {
    osd_level = sysfs_int("/sys/class/graphics/fb0/brightness");
    osd_muted = false;
  }
  if (osd_level > 100)
    osd_level = 100;
  osd_kind = kind;
  osd_until = msec() + OSD_MS;
  osd_dirty = true;
}

/* True when the overlay appeared, changed or expired since the last call */
bool osd_tick(void)
{
  if (osd_kind != OSD_NONE && (int)(msec() - osd_until) >= 0) {
    osd_kind = OSD_NONE;
    osd_dirty = true;
  }
  bool d = osd_dirty;
  osd_dirty = false;
  return d;
}

static void fill_round(int x, int y, int w, int h, int r, unsigned c)
{
  for (int row = 0; row < h; row++) {
    int dy = row < r ? r - row : (row >= h - r ? row - (h - r) + 1 : 0);
    int inset = 0;
    if (dy)
      while ((r - inset) * (r - inset) + dy * dy > r * r + r)
        inset++;
    gfx_fill_rect(x + inset, y + row, w - 2 * inset, 1, c);
  }
}

static void osd_speaker(int x, int y, unsigned c)
{
  gfx_fill_rect(x, y + 6, 5, 8, c);
  for (int i = 0; i < 6; i++)
    gfx_fill_rect(x + 5 + i, y + 6 - i, 1, 8 + 2 * i, c);
}

static void osd_sun(int cx, int cy, unsigned c)
{
  static const signed char dir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++)
      if (dx * dx + dy * dy <= 17)
        gfx_pixel(cx + dx, cy + dy, c);
  for (int i = 0; i < 8; i++)
    for (int k = 7; k <= 9; k++) {
      int m = i < 4 ? k : k * 5 / 7;
      gfx_pixel(cx + dir[i][0] * m, cy + dir[i][1] * m, c);
    }
}

void draw_osd(void)
{
  int x = osd_x(), y = osd_y();
  fill_round(x, y, OSD_W, OSD_H, 10, OSD_BG);
  int ix = x + 14, iy = y + (OSD_H - 20) / 2, cy = y + OSD_H / 2;
  bool off = osd_kind == OSD_VOLUME && (osd_muted || osd_level == 0);
  if (osd_kind == OSD_VOLUME) {
    osd_speaker(ix, iy, OSD_FG);
    if (off) {
      for (int i = 0; i < 8; i++) {
        gfx_pixel(ix + 14 + i, cy - 4 + i, 0xff6060);
        gfx_pixel(ix + 14 + i, cy + 3 - i, 0xff6060);
      }
    }
  } else {
    osd_sun(ix + 8, cy, OSD_FG);
  }
  if (osd_level < 0)
    return;
  int bx = x + 54, bw = OSD_W - 54 - 52, by = cy - 3;
  fill_round(bx, by, bw, 6, 3, OSD_DIM);
  int fw = osd_muted ? 0 : bw * osd_level / 100;
  if (fw >= 6)
    fill_round(bx, by, fw, 6, 3, OSD_ACCENT);
  else if (fw > 0)
    gfx_fill_rect(bx, by + 1, fw, 4, OSD_ACCENT);
  char t[5];
  int n = 0;
  if (osd_muted) {
    gfx_ui_text(x + OSD_W - 46, cy - UIFONT_H / 2, "Mute", 0xff8080);
    return;
  }
  if (osd_level >= 100)
    t[n++] = '1';
  if (osd_level >= 10)
    t[n++] = (char)('0' + osd_level / 10 % 10);
  t[n++] = (char)('0' + osd_level % 10);
  t[n++] = '%';
  t[n] = '\0';
  gfx_ui_text(x + OSD_W - 14 - gfx_ui_width(t), cy - UIFONT_H / 2, t, OSD_FG);
}

void draw_power(void)
{
  int x = pw_x(), y = pw_y();
  fill_round(x, y, PW_W, pw_h(), 10, PW_BG);
  gfx_ui_text(x + PW_PAD + 6, y + (PW_TITLE_H - UIFONT_H) / 2, "Power", PW_FG);
  for (int i = 0; i < PW_ROWS; i++) {
    int ry = y + PW_TITLE_H + i * PW_ROW_H;
    if (i == pw_sel)
      fill_round(x + PW_PAD, ry, PW_W - 2 * PW_PAD, PW_ROW_H - 2, 6, PW_SEL);
    const char *label = i == PW_CANCEL ? "Cancel" : power_label[i];
    gfx_ui_text(x + PW_PAD + 12, ry + (PW_ROW_H - 2 - UIFONT_H) / 2, label, pw_enabled(i) ? PW_FG : PW_OFF);
  }
}

void pw_show(void)
{
  pw_open = true;
  pw_sel = can_sleep ? PA_SLEEP : PA_RESTART;
  start_open = false;
  tb_menu_open = false;
  tray_menu_close();
  at_active = false;
}

void pw_activate(int row)
{
  pw_open = false;
  if (row != PW_CANCEL)
    power_run(row);
}

void pw_key(int c)
{
  if (c == 27 || c == KEY_POWER) {
    pw_open = false;
  } else if (c == '\n' || c == '\r') {
    pw_activate(pw_sel);
  } else if (c == KEY_UP || c == KEY_DOWN) {
    int step = c == KEY_UP ? PW_ROWS - 1 : 1;
    do
      pw_sel = (pw_sel + step) % PW_ROWS;
    while (!pw_enabled(pw_sel));
  }
}
