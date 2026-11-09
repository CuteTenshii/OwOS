/* Host test for the checkbox, radio and switch (user/lib/gfx/toggle.c) with the gfx primitives stubbed. */
#define OWOS_SUITE_TEST
#include "gfx.h"
#include "test.h"
#include <string.h>

#define MARK 0xffffffu
#define ACCENT 0x2f6bd8u

static long mark_x, mark_n, accent_n;

int gfx_ui_width(const char *s) { return (int)strlen(s) * 7; }
void gfx_pixel(int x, int y, unsigned color)
{
  (void)y;
  if (color == MARK) {
    mark_x += x;
    mark_n++;
  } else if (color == ACCENT) {
    accent_n++;
  }
}

static void reset(void) { mark_x = mark_n = accent_n = 0; }

static void check_geometry(void)
{
  expect_eq(control_width(GFX_CTRL_CHECKBOX, NULL), GUI_CHECK_SIZE);
  expect_eq(control_width(GFX_CTRL_RADIO, ""), GUI_CHECK_SIZE);
  expect_eq(control_width(GFX_CTRL_SWITCH, NULL), GUI_SWITCH_W);
  expect_eq(control_width(GFX_CTRL_CHECKBOX, "abc"), GUI_CHECK_SIZE + 6 + 21);
}

static void check_hit(void)
{
  int w = control_width(GFX_CTRL_SWITCH, "abc");
  expect(control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10, 20));
  expect(control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10 + w - 1, 20 + GUI_CTRL_H - 1));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10 + w, 20));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 9, 20));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10, 20 + GUI_CTRL_H));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, NULL, 10 + GUI_SWITCH_W, 20));
}

static void check_radio_dot(void)
{
  reset();
  radio_draw(0, 0, NULL, 0, BTN_NORMAL, 0);
  long off = accent_n;
  reset();
  radio_draw(0, 0, NULL, 1, BTN_NORMAL, 0);
  expect(accent_n > off);
  reset();
  radio_draw(0, 0, NULL, 1, BTN_DISABLED, 0);
  expect_eq(accent_n, 0);
}

static void check_switch_thumb(void)
{
  reset();
  switch_draw(0, 0, NULL, 0, BTN_NORMAL, 0);
  expect(mark_n > 0);
  expect(mark_x / mark_n < GUI_SWITCH_W / 2);
  reset();
  switch_draw(0, 0, NULL, 1, BTN_NORMAL, 0);
  expect(mark_x / mark_n > GUI_SWITCH_W / 2);
}

int main(void)
{
  test("width", check_geometry);
  test("hit", check_hit);
  test("radio dot only when checked and enabled", check_radio_dot);
  test("switch thumb moves with state", check_switch_thumb);
  return 0;
}
