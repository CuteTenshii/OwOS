/* cursor.c: mouse cursor shapes and drawing */
#include "comp.h"

/* cursors */
static const char *const arrow_glyph[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};
static const char *const hand_glyph[] = {
    "  XX     ", "  X.X    ", "  X.X    ", "  X.X    ", "  X.XXXX ", "  X.X..X ", "XXX...X.X",
    "X.......X", "X.......X", "X.......X", " X......X", " X......X", "  X.....X", "  XXXXXXX",
};
static const char *const text_glyph[] = {
    "X...X", "XX.XX", " X.X ", " X.X ", " X.X ", " X.X ", " X.X ",
    " X.X ", " X.X ", " X.X ", " X.X ", " X.X ", "XX.XX", "X...X",
};
static const char *const question_glyph[] = {
    " XXXXX ", "XX...XX", "X.XXX.X", "XXXXX.X", "  XX.XX", "  X.XX ", "  X.X  ", "  XXX  ", "  X.X  ", "  XXX  ",
};
static const char *const grab_glyph[] = {
    "   XX XX XX     ", "  X..X..X..XX   ", "  X..X..X..X.X  ", " XX..X..X..X..X ", "X.X.........X..X",
    "X..X.........X.X", " X...........X.X", " X............X ", "  X...........X ", "  X..........X  ",
    "   X.........X  ", "   X........X   ", "    XXXXXXXX    ",
};
static const char *const grabbing_glyph[] = {
    "                ", "                ", "    XX XX XX    ", "  XX..X..X..XX  ", " X..X.......X.X ",
    " X...........X.X", " X............X ", " X............X ", "  X..........X  ", "  X.........X   ",
    "   XXXXXXXXXX   ",
};
static const struct cur_shape cur_shapes[] = {
    [CUR_ARROW] = {arrow_glyph, 19, 0, 0},
    [CUR_HAND] = {hand_glyph, 14, 2, 0},
    [CUR_TEXT] = {text_glyph, 14, 2, 7},
    [CUR_GRAB] = {grab_glyph, 13, 8, 6},
    [CUR_GRABBING] = {grabbing_glyph, 11, 8, 6},
};
unsigned cursor_frame;

/* cursor drawing (ported from wm.c) */
static void draw_glyph(const struct cur_shape *s, int x, int y)
{
  for (int row = 0; row < s->nrows; row++) {
    const char *line = s->rows[row];
    for (int col = 0; line[col]; col++) {
      if (line[col] == 'X')
        gfx_pixel(x + col, y + row, 0x000000);
      else if (line[col] == '.')
        gfx_pixel(x + col, y + row, 0xffffff);
    }
  }
}
static void cur_dot(int cx, int cy, unsigned inner)
{
  gfx_fill_rect(cx - 2, cy - 2, 4, 4, 0x000000);
  gfx_fill_rect(cx - 1, cy - 1, 2, 2, inner);
}
static void draw_busy(int mx, int my)
{
  static const int dx[8] = {0, 4, 6, 4, 0, -4, -6, -4};
  static const int dy[8] = {-6, -4, 0, 4, 6, 4, 0, -4};
  int head = (int)((cursor_frame / 3) & 7);
  for (int i = 0; i < 8; i++) {
    int dist = (head - i + 8) & 7;
    int b = 40 + (7 - dist) * 30;
    if (b > 255)
      b = 255;
    cur_dot(mx + dx[i], my + dy[i], (unsigned)((b << 16) | (b << 8) | b));
  }
}
static void cur_stroke(int x, int y, int halo)
{
  if (halo) {
    gfx_pixel(x, y, 0xffffff);
    gfx_pixel(x + 1, y, 0xffffff);
    gfx_pixel(x - 1, y, 0xffffff);
    gfx_pixel(x, y + 1, 0xffffff);
    gfx_pixel(x, y - 1, 0xffffff);
  } else
    gfx_pixel(x, y, 0x000000);
}
static void draw_resize_nwse(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -5; t <= 5; t++)
      cur_stroke(mx + t, my + t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - 5 + k, my - 5, halo);
      cur_stroke(mx - 5, my - 5 + k, halo);
      cur_stroke(mx + 5 - k, my + 5, halo);
      cur_stroke(mx + 5, my + 5 - k, halo);
    }
  }
}
static void draw_resize_nesw(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -5; t <= 5; t++)
      cur_stroke(mx + t, my - t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx + 5 - k, my - 5, halo);
      cur_stroke(mx + 5, my - 5 + k, halo);
      cur_stroke(mx - 5 + k, my + 5, halo);
      cur_stroke(mx - 5, my + 5 - k, halo);
    }
  }
}
static void draw_resize_ns(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -6; t <= 6; t++)
      cur_stroke(mx, my + t, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - k, my - 6 + k, halo);
      cur_stroke(mx + k, my - 6 + k, halo);
      cur_stroke(mx - k, my + 6 - k, halo);
      cur_stroke(mx + k, my + 6 - k, halo);
    }
  }
}
static void draw_resize_ew(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -6; t <= 6; t++)
      cur_stroke(mx + t, my, halo);
    for (int k = 0; k <= 4; k++) {
      cur_stroke(mx - 6 + k, my - k, halo);
      cur_stroke(mx - 6 + k, my + k, halo);
      cur_stroke(mx + 6 - k, my - k, halo);
      cur_stroke(mx + 6 - k, my + k, halo);
    }
  }
}
static void draw_move(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int t = -7; t <= 7; t++) {
      cur_stroke(mx + t, my, halo);
      cur_stroke(mx, my + t, halo);
    }
    for (int k = 0; k <= 3; k++) {
      cur_stroke(mx - 7 + k, my - k, halo);
      cur_stroke(mx - 7 + k, my + k, halo);
      cur_stroke(mx + 7 - k, my - k, halo);
      cur_stroke(mx + 7 - k, my + k, halo);
      cur_stroke(mx - k, my - 7 + k, halo);
      cur_stroke(mx + k, my - 7 + k, halo);
      cur_stroke(mx - k, my + 7 - k, halo);
      cur_stroke(mx + k, my + 7 - k, halo);
    }
  }
}
static void draw_crosshair(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--)
    for (int t = 2; t <= 8; t++) {
      cur_stroke(mx - t, my, halo);
      cur_stroke(mx + t, my, halo);
      cur_stroke(mx, my - t, halo);
      cur_stroke(mx, my + t, halo);
    }
}
static void draw_not_allowed(int mx, int my)
{
  for (int halo = 1; halo >= 0; halo--) {
    for (int dy = -7; dy <= 7; dy++)
      for (int dx = -7; dx <= 7; dx++) {
        int d2 = dx * dx + dy * dy;
        if (d2 >= 28 && d2 <= 40)
          cur_stroke(mx + dx, my + dy, halo);
      }
    for (int t = -4; t <= 4; t++)
      cur_stroke(mx + t, my + t, halo);
  }
}
static void draw_help(int mx, int my)
{
  draw_glyph(&cur_shapes[CUR_ARROW], mx, my);
  static const struct cur_shape q = {question_glyph, 10, 0, 0};
  draw_glyph(&q, mx + 11, my + 9);
}
static void draw_progress(int mx, int my)
{
  static const int dx[4] = {0, 5, 0, -5};
  static const int dy[4] = {-5, 0, 5, 0};
  draw_glyph(&cur_shapes[CUR_ARROW], mx, my);
  int head = (int)((cursor_frame / 4) & 3);
  for (int i = 0; i < 4; i++)
    cur_dot(mx + 16 + dx[i], my + 16 + dy[i], i == head ? 0xffffff : 0x808080);
}
void draw_cursor(int kind, int mx, int my)
{
  switch (kind) {
  case CUR_MOVE:
    draw_move(mx, my);
    return;
  case CUR_CROSSHAIR:
    draw_crosshair(mx, my);
    return;
  case CUR_NOT_ALLOWED:
    draw_not_allowed(mx, my);
    return;
  case CUR_HELP:
    draw_help(mx, my);
    return;
  case CUR_PROGRESS:
    draw_progress(mx, my);
    return;
  case CUR_BUSY:
    draw_busy(mx, my);
    return;
  case CUR_RESIZE_NWSE:
    draw_resize_nwse(mx, my);
    return;
  case CUR_RESIZE_NESW:
    draw_resize_nesw(mx, my);
    return;
  case CUR_RESIZE_NS:
    draw_resize_ns(mx, my);
    return;
  case CUR_RESIZE_EW:
    draw_resize_ew(mx, my);
    return;
  default:
    if (kind < 0 || kind >= (int)(sizeof cur_shapes / sizeof cur_shapes[0]) || !cur_shapes[kind].rows)
      kind = CUR_ARROW;
    const struct cur_shape *s = &cur_shapes[kind];
    draw_glyph(s, mx - s->hotx, my - s->hoty);
    return;
  }
}
