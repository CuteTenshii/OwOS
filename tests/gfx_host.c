/* Host stand-ins for the gfx primitives and fonts; see gfx_host.h */
#include "gfx_host.h"
#include "gfx.h"
#include <string.h>

void (*gfx_host_fill)(int x, int y, int w, int h, unsigned color);
void (*gfx_host_pixel)(int x, int y, unsigned color);
void (*gfx_host_text)(int x, int y, const char *s, unsigned fg);

void gfx_host_reset(void)
{
  gfx_host_fill = 0;
  gfx_host_pixel = 0;
  gfx_host_text = 0;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned color)
{
  if (gfx_host_fill && w > 0 && h > 0)
    gfx_host_fill(x, y, w, h, color);
}

void gfx_rect(int x, int y, int w, int h, unsigned color) { gfx_round_rect(x, y, w, h, 0, 1, color); }

void gfx_pixel(int x, int y, unsigned color)
{
  if (gfx_host_pixel)
    gfx_host_pixel(x, y, color);
}

int gfx_font_cw(unsigned cp, const struct gfx_font *f)
{
  (void)cp;
  (void)f;
  return GFX_HOST_CW;
}

int gfx_font_width(const char *s, const struct gfx_font *f)
{
  (void)f;
  return (int)strlen(s) * GFX_HOST_CW;
}

int gfx_font_height(const struct gfx_font *f)
{
  struct gfx_font r;
  gfx_font_resolve(f, &r);
  return r.px * 6 / 5;
}

int gfx_font_ascent(const struct gfx_font *f)
{
  struct gfx_font r;
  gfx_font_resolve(f, &r);
  return r.px;
}

void gfx_font_text(int x, int y, const char *s, unsigned fg, const struct gfx_font *f)
{
  (void)f;
  if (gfx_host_text)
    gfx_host_text(x, y, s, fg);
}

void gfx_font_text_clip(int x, int y, const char *s, unsigned fg, const struct gfx_font *f, int maxw)
{
  if (maxw > 0)
    gfx_font_text(x, y, s, fg, f);
}
