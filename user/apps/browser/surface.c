#include "browser.h"

extern int BW, BH;

static unsigned *g_surface;
static int g_surface_px;

static unsigned *g_redirect;

void browser_surface_redirect(unsigned *to) { g_redirect = to; }

unsigned *browser_surface(void)
{
  if (g_redirect)
    return g_redirect;
  int need = BW * BH;
  if (need > g_surface_px) {
    unsigned *n = realloc(g_surface, (unsigned)need * 4);
    if (n) {
      g_surface = n;
      g_surface_px = need;
    }
  }
  return g_surface ? g_surface : (unsigned *)WIN_FB_ADDR;
}

int browser_surface_ready(void) { return g_surface && BW * BH <= g_surface_px; }

void browser_present(int id)
{
  if (g_surface) {
    int px = BW * BH;
    if (px > g_surface_px)
      px = g_surface_px;
    memcpy((unsigned *)WIN_FB_ADDR, g_surface, (unsigned)px * 4);
  }
  win_present(id);
}

void browser_blit_rect(int x, int y, int w, int h)
{
  if (!g_surface)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > BW)
    w = BW - x;
  if (y + h > BH)
    h = BH - y;
  if (w > 0 && h > 0)
    for (int row = 0; row < h; row++)
      memcpy((unsigned *)WIN_FB_ADDR + (unsigned long)(y + row) * BW + x, g_surface + (unsigned long)(y + row) * BW + x,
             (unsigned)w * 4);
}
