/* Host test for widget styles and the theme (user/lib/gfx/style.c) with the gfx primitives stubbed. */
#define OWOS_SUITE_TEST
#include "gfx.h"
#include "gfx_host.h"
#include "test.h"
#include <string.h>

#define GRID 32
static unsigned grid[GRID][GRID];

static void grid_fill(int x, int y, int w, int h, unsigned color)
{
  for (int r = y; r < y + h; r++)
    for (int c = x; c < x + w; c++)
      if (r >= 0 && r < GRID && c >= 0 && c < GRID)
        grid[r][c] = color;
}

static void grid_reset(void)
{
  memset(grid, 0, sizeof grid);
  gfx_host_reset();
  gfx_host_fill = grid_fill;
}

static int text_x, text_y, text_n;
static unsigned text_fg;

static void record_text(int x, int y, const char *s, unsigned fg)
{
  (void)s;
  text_x = x;
  text_y = y;
  text_fg = fg;
  text_n++;
}

static void check_theme(void)
{
  const struct gfx_style *b = gfx_theme(GFX_W_BUTTON);
  expect(memcmp(b, gfx_theme_default(GFX_W_BUTTON), sizeof *b) == 0);
  expect(gfx_theme(-1) == gfx_theme(GFX_W_BASE));
  expect(gfx_theme(GFX_W_COUNT) == gfx_theme(GFX_W_BASE));
  expect(gfx_style_of(GFX_W_BUTTON, NULL) == b);

  struct gfx_style mine = *b;
  mine.radius = 7;
  gfx_theme_set(GFX_W_BUTTON, &mine);
  expect(gfx_theme(GFX_W_BUTTON) == b); /* the pointer stays, what it points to changes */
  expect_eq(b->radius, 7);
  expect_eq(gfx_theme_default(GFX_W_BUTTON)->radius, 0);
  gfx_theme_set(GFX_W_BUTTON, NULL);
  expect_eq(b->radius, 0);
}

static void check_box_model(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_BUTTON);
  struct gfx_box b, c;
  gfx_sides(st.margin, 1, 2, 3, 4);
  gfx_sides(st.padding, 5, 6, 7, 8);
  st.border = 2;
  gfx_style_box(&st, 10, 20, 100, 50, &b);
  expect_eq(b.x, 14);
  expect_eq(b.y, 21);
  expect_eq(b.w, 100 - 4 - 2);
  expect_eq(b.h, 50 - 1 - 3);
  gfx_style_content(&st, &b, &c);
  expect_eq(c.x, 14 + 2 + 8);
  expect_eq(c.y, 21 + 2 + 5);
  expect_eq(c.w, b.w - 4 - 8 - 6);
  expect_eq(c.h, b.h - 4 - 5 - 7);

  st.width = 40; /* a set size is the border box; the margin stays outside it */
  st.height = 30;
  gfx_style_box(&st, 10, 20, 100, 50, &b);
  expect_eq(b.x, 14);
  expect_eq(b.w, 40);
  expect_eq(b.h, 30);
  expect_eq(gfx_style_outer_w(&st, 40), 46);
  expect_eq(gfx_style_outer_h(&st, 30), 34);

  st.width = st.height = 0; /* margins wider than the space collapse the box, never invert it */
  gfx_style_box(&st, 0, 0, 3, 2, &b);
  expect_eq(b.w, 0);
  expect_eq(b.h, 0);
  gfx_style_content(&st, &b, &c);
  expect_eq(c.w, 0);
  expect_eq(c.h, 0);
}

static void check_font_inheritance(void)
{
  struct gfx_font f;
  gfx_font_resolve(NULL, &f);
  expect_eq(f.px, 15);
  expect_eq(f.weight, GFX_WEIGHT_NORMAL);
  expect(f.family == NULL);

  struct gfx_font bold = {.weight = GFX_WEIGHT_BOLD};
  gfx_font_resolve(&bold, &f);
  expect_eq(f.px, 15);
  expect_eq(f.weight, GFX_WEIGHT_BOLD);

  struct gfx_style base = *gfx_theme(GFX_W_BASE);
  base.font.px = 20;
  base.font.family = "Liberation Serif";
  gfx_theme_set(GFX_W_BASE, &base);
  gfx_style_font(gfx_theme(GFX_W_BUTTON), &f); /* a widget with no font of its own follows the base */
  expect_eq(f.px, 20);
  expect_string(f.family, "Liberation Serif");
  expect_eq(gfx_style_text_h(gfx_theme(GFX_W_BUTTON)), 24 + 2);
  struct gfx_style mono = *gfx_theme(GFX_W_BUTTON);
  mono.font.family = "monospace";
  gfx_style_font(&mono, &f);
  expect_string(f.family, "monospace");
  expect_eq(f.px, 20);
  gfx_theme_set(GFX_W_BASE, NULL);
  expect_eq(gfx_style_text_h(gfx_theme(GFX_W_BUTTON)), UIFONT_H + 2);
}

static void check_round_fill(void)
{
  grid_reset();
  gfx_fill_round(0, 0, 20, 20, 0, 1);
  expect_eq(grid[0][0], 1);
  expect_eq(grid[19][19], 1);

  grid_reset();
  gfx_fill_round(0, 0, 20, 20, 6, 1);
  expect_eq(grid[0][0], 0); /* corners cut */
  expect_eq(grid[0][19], 0);
  expect_eq(grid[19][0], 0);
  expect_eq(grid[19][19], 0);
  expect_eq(grid[10][10], 1);
  expect_eq(grid[0][10], 1); /* straight edges kept */
  expect_eq(grid[10][0], 1);
  for (int r = 0; r < 20; r++) /* mirror-symmetric */
    for (int c = 0; c < 20; c++)
      expect_eq(grid[r][c], grid[19 - r][19 - c]);

  grid_reset();
  gfx_fill_round(0, 0, 10, 4, 50, 1); /* a radius past half the side clamps to a pill */
  expect_eq(grid[2][5], 1);
  expect_eq(grid[0][0], 0);
}

static void check_round_rect(void)
{
  grid_reset();
  gfx_round_rect(2, 2, 10, 8, 0, 1, 1);
  expect_eq(grid[2][2], 1);
  expect_eq(grid[9][11], 1);
  expect_eq(grid[5][2], 1);
  expect_eq(grid[5][11], 1);
  expect_eq(grid[5][5], 0); /* hollow */
  expect_eq(grid[1][1], 0);
  expect_eq(grid[10][12], 0);

  grid_reset();
  gfx_round_rect(0, 0, 20, 20, 0, 3, 1);
  expect_eq(grid[2][10], 1);
  expect_eq(grid[3][10], 0);
  expect_eq(grid[10][17], 1);
  expect_eq(grid[10][16], 0);

  grid_reset();
  gfx_round_rect(0, 0, 20, 20, 6, 2, 1);
  expect_eq(grid[0][0], 0);
  expect_eq(grid[10][0], 1);
  expect_eq(grid[10][1], 1);
  expect_eq(grid[10][2], 0);
  expect_eq(grid[10][10], 0);
}

static void check_paint(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_BUTTON);
  struct gfx_colors c = {.bg = 2, .fg = 3, .border = 4, .detail = 5};
  struct gfx_box b = {0, 0, 20, 10};
  grid_reset();
  gfx_style_paint(&st, &b, &c);
  expect_eq(grid[0][5], 4);  /* border */
  expect_eq(grid[1][5], 5);  /* the lit top line inside it */
  expect_eq(grid[5][5], 2);  /* body */
  expect_eq(grid[5][19], 4); /* right border */

  c.detail = c.bg; /* flat: no line */
  st.border = 0;
  grid_reset();
  gfx_style_paint(&st, &b, &c);
  expect_eq(grid[0][5], 2);
  expect_eq(grid[1][5], 2);
}

static void check_label_align(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_BUTTON);
  struct gfx_box c = {10, 0, 100, UIFONT_H + 6};
  gfx_host_reset();
  gfx_host_text = record_text;
  st.align = GFX_ALIGN_START;
  gfx_style_label(&st, &c, "abcd", 7);
  expect_eq(text_x, 10);
  expect_eq(text_y, 3); /* centred vertically */
  expect_eq(text_fg, 7);
  st.align = GFX_ALIGN_CENTER;
  gfx_style_label(&st, &c, "abcd", 7);
  expect_eq(text_x, 10 + (100 - 4 * GFX_HOST_CW) / 2);
  st.align = GFX_ALIGN_END;
  gfx_style_label(&st, &c, "abcd", 7);
  expect_eq(text_x, 110 - 4 * GFX_HOST_CW);
  c.w = 2 * GFX_HOST_CW; /* too wide: cut from the start whatever the alignment */
  gfx_style_label(&st, &c, "abcd", 7);
  expect_eq(text_x, 10);
  text_n = 0;
  gfx_style_label(&st, &c, "", 7);
  expect_eq(text_n, 0);
  gfx_host_reset();
}

static void check_focus_ring(void)
{
  struct gfx_style st = *gfx_theme(GFX_W_BUTTON);
  grid_reset();
  gfx_focus_ring(10, 10, 10, 10, &st);
  expect_eq(grid[7][15], st.focus);
  expect_eq(grid[8][15], st.focus);
  expect_eq(grid[9][15], 0); /* 1px clear of the widget */
  st.flags |= GFX_STYLE_NO_FOCUS;
  grid_reset();
  gfx_focus_ring(10, 10, 10, 10, &st);
  expect_eq(grid[7][15], 0);
  gfx_host_reset();
}

static void check_state_index(void)
{
  expect_eq(gfx_state_index(BTN_HOVER | GFX_FOCUSED), BTN_HOVER);
  expect_eq(gfx_state_index(BTN_DISABLED), BTN_DISABLED);
  expect_eq(gfx_state_index(-3), BTN_NORMAL);
  expect_eq(gfx_state_index(9), BTN_NORMAL);
}

int main(void)
{
  test("theme get, set and restore", check_theme);
  test("margin, border, padding and size", check_box_model);
  test("fonts inherit from the base", check_font_inheritance);
  test("rounded fill", check_round_fill);
  test("rounded outline", check_round_rect);
  test("box painting", check_paint);
  test("label alignment and clipping", check_label_align);
  test("focus ring", check_focus_ring);
  test("state index", check_state_index);
  return 0;
}
