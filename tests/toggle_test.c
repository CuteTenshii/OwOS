/* Host test for the checkbox, radio and switch (user/lib/gfx/toggle.c) with the gfx primitives stubbed. */
#define OWOS_SUITE_TEST
#include "gfx.h"
#include "gfx_host.h"
#include "test.h"
#include <stddef.h>

#define MARK 0xffffffu
#define ACCENT 0x2f6bd8u

static long mark_x, mark_n, accent_n;

static void count_pixel(int x, int y, unsigned color)
{
  (void)y;
  if (color == MARK) {
    mark_x += x;
    mark_n++;
  } else if (color == ACCENT) {
    accent_n++;
  }
}

static void reset(void)
{
  mark_x = mark_n = accent_n = 0;
  gfx_host_reset();
  gfx_host_pixel = count_pixel;
}

static void check_geometry(void)
{
  expect_eq(control_width(GFX_CTRL_CHECKBOX, NULL, NULL), GUI_CHECK_SIZE);
  expect_eq(control_width(GFX_CTRL_RADIO, "", NULL), GUI_CHECK_SIZE);
  expect_eq(control_width(GFX_CTRL_SWITCH, NULL, NULL), GUI_SWITCH_W);
  expect_eq(control_width(GFX_CTRL_CHECKBOX, "abc", NULL), GUI_CHECK_SIZE + 6 + 3 * GFX_HOST_CW);
}

static void check_hit(void)
{
  int w = control_width(GFX_CTRL_SWITCH, "abc", NULL);
  expect(control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10, 20, NULL));
  expect(control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10 + w - 1, 20 + GUI_CTRL_H - 1, NULL));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10 + w, 20, NULL));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 9, 20, NULL));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, "abc", 10, 20 + GUI_CTRL_H, NULL));
  expect(!control_hit(GFX_CTRL_SWITCH, 10, 20, NULL, 10 + GUI_SWITCH_W, 20, NULL));
}

static void check_radio_dot(void)
{
  reset();
  radio_draw(0, 0, NULL, 0, BTN_NORMAL, 0, NULL);
  long off = accent_n;
  reset();
  radio_draw(0, 0, NULL, 1, BTN_NORMAL, 0, NULL);
  expect(accent_n > off);
  reset();
  radio_draw(0, 0, NULL, 1, BTN_DISABLED, 0, NULL);
  expect_eq(accent_n, 0);
}

static void check_switch_thumb(void)
{
  reset();
  switch_draw(0, 0, NULL, 0, BTN_NORMAL, 0, NULL);
  expect(mark_n > 0);
  expect(mark_x / mark_n < GUI_SWITCH_W / 2);
  reset();
  switch_draw(0, 0, NULL, 1, BTN_NORMAL, 0, NULL);
  expect(mark_x / mark_n > GUI_SWITCH_W / 2);
}

static void check_styled_geometry(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_CHECKBOX);
  st.part_w = st.part_h = 20;
  st.gap = 10;
  gfx_sides(st.margin, 2, 0, 0, 3);
  expect_eq(control_width(GFX_CTRL_CHECKBOX, "abc", &st), 3 + 20 + 10 + 3 * GFX_HOST_CW);
  expect_eq(control_height(GFX_CTRL_CHECKBOX, &st), 2 + 20); /* the part outgrows the 18px line */
  expect(control_hit(GFX_CTRL_CHECKBOX, 0, 0, "abc", 3, 2, &st));
  expect(!control_hit(GFX_CTRL_CHECKBOX, 0, 0, "abc", 2, 2, &st)); /* the margin is not the control */
  expect(!control_hit(GFX_CTRL_CHECKBOX, 0, 0, "abc", 3, 1, &st));
  st.font.px = 30; /* a 36px line now sets the row height */
  expect_eq(control_height(GFX_CTRL_CHECKBOX, &st), 2 + 36);
}

static int tick_right;

static void track_tick(int x, int y, int w, int h, unsigned color)
{
  (void)y;
  (void)h;
  if (color == MARK && x + w > tick_right)
    tick_right = x + w;
}

static void check_tick_scales(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_CHECKBOX);
  reset();
  gfx_host_fill = track_tick;
  tick_right = 0;
  checkbox_draw(0, 0, NULL, 1, BTN_NORMAL, 0, &st);
  int small = tick_right;
  st.part_w = st.part_h = 28;
  tick_right = 0;
  checkbox_draw(0, 0, NULL, 1, BTN_NORMAL, 0, &st);
  expect(small > 0);
  expect(tick_right > small);
  expect(tick_right <= 28);
  gfx_host_reset();
}

int main(void)
{
  test("width", check_geometry);
  test("hit", check_hit);
  test("radio dot only when checked and enabled", check_radio_dot);
  test("switch thumb moves with state", check_switch_thumb);
  test("styled margins, part and gap", check_styled_geometry);
  test("tick scales with the box", check_tick_scales);
  return 0;
}
