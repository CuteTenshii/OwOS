/* fbcon_test.c: the AArch64 framebuffer text console (kernel/arch/aarch64/fbcon.c) drawing into host memory. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/fbcon.h"
#include "../kernel/font8x16.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 64

static uint32_t px32(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y)
{
  return *(const uint32_t *)(fb + (size_t)y * stride + x * 4);
}

static struct fb_info make(uint8_t *mem, uint32_t w, uint32_t h, uint32_t stride, enum fb_format f)
{
  return (struct fb_info){(uintptr_t)mem, w, h, stride, f};
}

static void check_formats(void)
{
  expect_eq(fb_format_parse("a8r8g8b8"), FB_XRGB8888);
  expect_eq(fb_format_parse("x8r8g8b8"), FB_XRGB8888);
  expect_eq(fb_format_parse("a8b8g8r8"), FB_XBGR8888);
  expect_eq(fb_format_parse("r5g6b5"), FB_RGB565);
  expect_eq(fb_format_parse("r8g8b8"), FB_NONE);
  expect_eq(fb_format_parse(""), FB_NONE);
}

static void check_init_rejects(void)
{
  uint8_t mem[W * H * 4];
  struct fb_info ok = make(mem, W, H, W * 4, FB_XRGB8888);
  struct fb_info bad = ok;
  bad.base = 0;
  expect_eq(fbcon_init(&bad), -1);
  bad = ok;
  bad.format = FB_NONE;
  expect_eq(fbcon_init(&bad), -1);
  bad = ok;
  bad.stride = W * 4 - 4; /* a row shorter than the visible width */
  expect_eq(fbcon_init(&bad), -1);
  bad = ok;
  bad.height = 8;
  expect_eq(fbcon_init(&bad), -1);
  expect_eq(fbcon_init(&ok), 0);
}

static void check_glyph(void)
{
  uint8_t *mem = malloc(W * H * 4);
  memset(mem, 0xaa, W * H * 4);
  struct fb_info fb = make(mem, W, H, W * 4, FB_XRGB8888);
  expect_eq(fbcon_init(&fb), 0);
  expect_eq(px32(mem, W * 4, 100, 30), 0); /* init cleared the screen */

  fbcon_putc('A');
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 8; x++)
      expect_eq(px32(mem, W * 4, (uint32_t)x, (uint32_t)y), ((font8x16['A'][y] >> (7 - x)) & 1) ? 0xffffffffu : 0);

  fbcon_putc('B'); /* the cursor moved one cell right */
  expect_eq(px32(mem, W * 4, 8 + 2, 5), ((font8x16['B'][5] >> 5) & 1) ? 0xffffffffu : 0);
  free(mem);
}

static void check_cursor_motion(void)
{
  uint8_t *mem = malloc(W * H * 4);
  struct fb_info fb = make(mem, W, H, W * 4, FB_XRGB8888);
  expect_eq(fbcon_init(&fb), 0);
  const char *s = "AB\r\nC";
  while (*s)
    fbcon_putc(*s++);
  /* C is at column 0 of row 1 */
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 8; x++)
      expect_eq(px32(mem, W * 4, (uint32_t)x, 16 + (uint32_t)y), ((font8x16['C'][y] >> (7 - x)) & 1) ? 0xffffffffu : 0);

  expect_eq(fbcon_init(&fb), 0);
  for (int i = 0; i < W / 8 + 1; i++) /* one past the last column wraps to the next row */
    fbcon_putc('#');
  expect_eq(px32(mem, W * 4, 3, 16 + 2), ((font8x16['#'][2] >> 4) & 1) ? 0xffffffffu : 0);
  free(mem);
}

static void check_full_screen_restarts(void)
{
  uint8_t *mem = malloc(W * H * 4);
  struct fb_info fb = make(mem, W, H, W * 4, FB_XRGB8888);
  expect_eq(fbcon_init(&fb), 0);
  for (int row = 0; row < H / 16; row++)
    fbcon_putc('#'), fbcon_putc('\n');
  /* the screen is now full; the next character clears it and starts at the top */
  fbcon_putc('A');
  for (int y = 0; y < H; y++)
    for (int x = 0; x < 8; x++) {
      uint32_t want = y < 16 && ((font8x16['A'][y] >> (7 - x)) & 1) ? 0xffffffffu : 0;
      expect_eq(px32(mem, W * 4, (uint32_t)x, (uint32_t)y), want);
    }
  free(mem);
}

static void check_scaling_and_stride(void)
{
  uint32_t w = 720, h = 64, stride = w * 4 + 64;
  uint8_t *mem = malloc((size_t)stride * h);
  memset(mem, 0x55, (size_t)stride * h);
  struct fb_info fb = make(mem, w, h, stride, FB_XRGB8888);
  expect_eq(fbcon_init(&fb), 0);
  fbcon_putc('A'); /* 720 wide: every font pixel is a 2x2 block */
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 16; x++)
      expect_eq(px32(mem, stride, (uint32_t)x, (uint32_t)y),
                ((font8x16['A'][y / 2] >> (7 - x / 2)) & 1) ? 0xffffffffu : 0);
  expect_eq(mem[w * 4], 0x55); /* row padding is never written */
  expect_eq(mem[stride * 10 + w * 4 + 63], 0x55);
  free(mem);
}

static void check_rgb565(void)
{
  uint8_t *mem = calloc(W * H, 2);
  struct fb_info fb = make(mem, W, H, W * 2, FB_RGB565);
  expect_eq(fbcon_init(&fb), 0);
  fbcon_putc('A');
  int lit = 0;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 8; x++) {
      uint16_t p = *(uint16_t *)(mem + (size_t)y * W * 2 + (size_t)x * 2);
      expect_eq(p, ((font8x16['A'][y] >> (7 - x)) & 1) ? 0xffff : 0);
      lit += p != 0;
    }
  expect_eq(lit > 10, 1);
  expect_eq(*(uint16_t *)(mem + 8 * 2), 0); /* the next cell is untouched */
  free(mem);
}

int main(void)
{
  test("simple-framebuffer format names", check_formats);
  test("unusable layouts rejected", check_init_rejects);
  test("glyphs match the font and init clears", check_glyph);
  test("newline, carriage return and wrap", check_cursor_motion);
  test("full screen clears and restarts", check_full_screen_restarts);
  test("wide screens scale glyphs, padding untouched", check_scaling_and_stride);
  test("RGB565 pixels", check_rgb565);
  return 0;
}
