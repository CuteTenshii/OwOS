/* Host test for the scroll view widget (user/lib/gfx/scrollview.c) with the gfx primitives stubbed. */
#define OWOS_SUITE_TEST
#include "gfx.h"
#include "owos.h"
#include "test.h"

static int clip[4] = {0, 0, 640, 480};
static int bars_drawn;

void gfx_get_clip(int *x, int *y, int *w, int *h)
{
  *x = clip[0];
  *y = clip[1];
  *w = clip[2];
  *h = clip[3];
}
void gfx_set_clip(int x, int y, int w, int h)
{
  clip[0] = x;
  clip[1] = y;
  clip[2] = w;
  clip[3] = h;
}
void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible)
{
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)scroll;
  (void)total;
  (void)visible;
  bars_drawn++;
}
/* Whole bar is track; the position maps linearly from the pointer's y */
int scrollbar_hit(int x, int y, int w, int h, int mx, int my)
{
  if (mx < x || mx >= x + w || my < y || my >= y + h)
    return SCROLLBAR_NONE;
  return my < y + 10 ? SCROLLBAR_UP : my >= y + h - 10 ? SCROLLBAR_DOWN : SCROLLBAR_TRACK;
}
int scrollbar_pos(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible)
{
  if (scrollbar_hit(x, y, w, h, mx, my) != SCROLLBAR_TRACK)
    return scroll;
  return (my - y - 10) * (total - visible) / (h - 20);
}

static struct scrollview sv;

static void check_fits(void)
{
  expect_eq(scrollview_begin(&sv, 10, 50, 200, 100, 80), 50);
  expect_eq(scrollview_content_w(&sv), 200);
  scrollview_end(&sv);
  expect_eq(bars_drawn, 0);
  expect(!scrollview_wheel(&sv, -3));
  expect_eq(sv.scroll, 0);
}

static void check_overflow(void)
{
  expect_eq(scrollview_begin(&sv, 10, 50, 200, 100, 400), 50);
  expect_eq(scrollview_content_w(&sv), 200 - GUI_SCROLL_W);
  expect_eq(clip[2], 200 - GUI_SCROLL_W);
  scrollview_end(&sv);
  expect_eq(bars_drawn, 1);
  expect_eq(clip[2], 640);
  expect(scrollview_wheel(&sv, -2));
  expect_eq(sv.scroll, 40);
  expect_eq(scrollview_begin(&sv, 10, 50, 200, 100, 400), 10);
  scrollview_end(&sv);
  scrollview_wheel(&sv, -100);
  expect_eq(sv.scroll, 300);
  expect(!scrollview_wheel(&sv, -1));
  scrollview_wheel(&sv, 100);
  expect_eq(sv.scroll, 0);
}

static void check_keys(void)
{
  expect(scrollview_key(&sv, KEY_DOWN));
  expect_eq(sv.scroll, 20);
  expect(scrollview_key(&sv, KEY_END));
  expect_eq(sv.scroll, 300);
  expect(scrollview_key(&sv, KEY_HOME));
  expect_eq(sv.scroll, 0);
  expect(!scrollview_key(&sv, 'x'));
}

static void check_shrink(void)
{
  sv.scroll = 300;
  scrollview_begin(&sv, 10, 50, 200, 100, 150);
  scrollview_end(&sv);
  expect_eq(sv.scroll, 50);
}

static void check_reveal(void)
{
  sv.scroll = 0;
  scrollview_begin(&sv, 10, 50, 200, 100, 400);
  scrollview_end(&sv);
  expect(!scrollview_reveal(&sv, 10, 20));
  expect(scrollview_reveal(&sv, 250, 30));
  expect_eq(sv.scroll, 180);
  expect(scrollview_reveal(&sv, 60, 20));
  expect_eq(sv.scroll, 60);
}

static void check_press_drag(void)
{
  sv.scroll = 100;
  int bx = 10 + 200 - GUI_SCROLL_W;
  expect(!scrollview_press(&sv, 20, 80));
  expect(scrollview_press(&sv, bx + 2, 50 + 2));
  expect_eq(sv.scroll, 80);
  expect(scrollview_press(&sv, bx + 2, 50 + 100 - 2));
  expect_eq(sv.scroll, 100);
  expect(!scrollview_drag(&sv, bx, 70));
  expect(scrollview_press(&sv, bx + 2, 50 + 10));
  expect_eq(sv.scroll, 0);
  expect(scrollview_drag(&sv, bx - 40, 50 + 55));
  expect_eq(sv.scroll, 168);
  expect(!scrollview_press(&sv, 20, 80));
  expect(!scrollview_drag(&sv, bx, 100));
}

static void check_nested_clip(void)
{
  gfx_set_clip(0, 60, 100, 30);
  scrollview_begin(&sv, 10, 50, 200, 100, 400);
  expect_eq(clip[1], 60);
  expect_eq(clip[2] + clip[0], 100);
  expect_eq(clip[3], 30);
  scrollview_end(&sv);
  expect_eq(clip[3], 30);
}

int main(void)
{
  scrollview_init(&sv, 20);

  test("fits: no bar, no scrolling", check_fits);
  test("overflow: wheel, clamp, content width", check_overflow);
  test("keys", check_keys);
  test("content shrinking re-clamps the scroll", check_shrink);
  test("reveal", check_reveal);
  test("scrollbar press and drag", check_press_drag);
  test("nested clip is respected", check_nested_clip);
  return 0;
}
