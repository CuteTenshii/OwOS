/* fbcon.c: 8x16 text console drawn straight into the framebuffer; when the screen is full it clears and restarts at the
 * top. */
#include "fbcon.h"
#include "font8x16.h"
#include <stddef.h>

#define CELL_W 8
#define CELL_H 16
#define FG 0xffffffffu
#define CACHE_LINE_MIN 64

static struct fb_info fb;
static uint32_t scale, cols, rows, col, row;

static int streq(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

enum fb_format fb_format_parse(const char *name)
{
  if (streq(name, "a8r8g8b8") || streq(name, "x8r8g8b8"))
    return FB_XRGB8888;
  if (streq(name, "a8b8g8r8") || streq(name, "x8b8g8r8"))
    return FB_XBGR8888;
  if (streq(name, "r5g6b5"))
    return FB_RGB565;
  return FB_NONE;
}

static uint32_t bytes_per_pixel(void) { return fb.format == FB_RGB565 ? 2 : 4; }

/* The display engine reads memory directly, so push written lines out of the data cache. */
static void flush(uintptr_t addr, size_t len)
{
#ifdef __aarch64__
  uintptr_t end = addr + len;
  for (uintptr_t p = addr & ~(uintptr_t)(CACHE_LINE_MIN - 1); p < end; p += CACHE_LINE_MIN)
    __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
  __asm__ volatile("dsb sy" : : : "memory");
#else
  (void)addr;
  (void)len;
#endif
}

static void put_pixel(uintptr_t at, uint32_t on)
{
  if (fb.format == FB_RGB565)
    *(volatile uint16_t *)at = on ? 0xffff : 0;
  else
    *(volatile uint32_t *)at = on ? FG : 0;
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t on)
{
  for (uint32_t j = 0; j < h; j++)
    for (uint32_t i = 0; i < w; i++)
      put_pixel(fb.base + (uintptr_t)(y + j) * fb.stride + (uintptr_t)(x + i) * bytes_per_pixel(), on);
}

static void clear_screen(void)
{
  fill(0, 0, fb.width, fb.height, 0);
  flush(fb.base, (size_t)fb.stride * fb.height);
  col = row = 0;
}

static void draw_glyph(uint32_t c, uint32_t cx, uint32_t cy)
{
  uint32_t x0 = cx * CELL_W * scale, y0 = cy * CELL_H * scale;
  for (uint32_t gy = 0; gy < CELL_H; gy++) {
    uint8_t bits = font8x16[c & 0xff][gy];
    for (uint32_t gx = 0; gx < CELL_W; gx++)
      fill(x0 + gx * scale, y0 + gy * scale, scale, scale, (bits >> (7 - gx)) & 1);
  }
  for (uint32_t y = y0; y < y0 + CELL_H * scale; y++)
    flush(fb.base + (uintptr_t)y * fb.stride + (uintptr_t)x0 * bytes_per_pixel(),
          (size_t)CELL_W * scale * bytes_per_pixel());
}

int fbcon_init(const struct fb_info *info)
{
  if (!info->base || info->format == FB_NONE || info->width < CELL_W || info->height < CELL_H ||
      info->stride < info->width * (info->format == FB_RGB565 ? 2u : 4u))
    return -1;
  fb = *info;
  scale = fb.width >= 720 ? fb.width / 360 : 1;
  cols = fb.width / (CELL_W * scale);
  rows = fb.height / (CELL_H * scale);
  clear_screen();
  return 0;
}

void fbcon_putc(char c)
{
  if (!fb.base)
    return;
  if (c == '\r') {
    col = 0;
    return;
  }
  if (c == '\n') {
    col = 0;
    row++;
  } else {
    if (col == cols) {
      col = 0;
      row++;
    }
    if (row == rows)
      clear_screen();
    draw_glyph((uint8_t)c, col++, row);
    return;
  }
  if (row == rows)
    clear_screen();
}
