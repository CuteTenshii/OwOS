/* screen.c: framebuffer binding, clipping and HiDPI present */
#include "comp.h"

/* screen + back buffer */
unsigned *back;           /* surface the desktop is drawn into */
static unsigned *logical; /* heap surface used when ui_scale > 1 */
int screen_w, screen_h;   /* visible dimensions */
int fb_pitch;             /* back-buffer stride in pixels (== gfx surface w) */

/* HiDPI: the desktop is drawn at logical size (hw / ui_scale) and replicated up on present */
static unsigned *hw;      /* mapped framebuffer shadow at hardware size */
int hw_w, hw_h, hw_pitch; /* hardware dimensions (pixels) */
int ui_scale = 1;
bool scale_stale; /* the kernel's active scale differs from ui_scale */

int work_bottom(void) { return screen_h - WM_BAR_H; }

void clamp_frame_position(int *x, int *y, int w, int h)
{
  if (*x + w > screen_w)
    *x = screen_w - w;
  if (*y + h > work_bottom())
    *y = work_bottom() - h;
  if (*x < 0)
    *x = 0;
  if (*y < 0)
    *y = 0;
}

void fit_frame_to_work_area(int *x, int *y, int *w, int *h)
{
  if (*w > screen_w)
    *w = screen_w;
  if (*h > work_bottom())
    *h = work_bottom();
  clamp_frame_position(x, y, *w, *h);
}

/* Mirror of the active gfx clip rect */
int clip_x0, clip_y0, clip_x1, clip_y1;
void set_clip(int x, int y, int w, int h)
{
  int x1 = x + w, y1 = y + h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x1 > screen_w)
    x1 = screen_w;
  if (y1 > screen_h)
    y1 = screen_h;
  clip_x0 = x;
  clip_y0 = y;
  clip_x1 = x1 > x ? x1 : x;
  clip_y1 = y1 > y ? y1 : y;
  gfx_set_clip(x, y, w, h);
}
void clip_full(void)
{
  clip_x0 = 0;
  clip_y0 = 0;
  clip_x1 = screen_w;
  clip_y1 = screen_h;
  gfx_clip_none();
}

/* The kernel's active UI scale (setting, or EDID-derived when auto) */
int scale_now(void)
{
  char v[8];
  if (node_read("/sys/class/graphics/fb0/scale_active", v, sizeof v) <= 0)
    return 1;
  int n = scan_uint(v);
  return n < 1 ? 1 : n > 4 ? 4 : n;
}

/* Rebind to the framebuffer's current geometry: remap the back buffer */
int bind_mode(void)
{
  struct fbinfo fi;
  if (fb_info(&fi) < 0)
    return -1;
  hw = fb_map();
  if (!hw)
    return -1;
  hw_w = fi.w;
  hw_h = fi.h;
  hw_pitch = fi.pitch;
  ui_scale = scale_now();
  scale_stale = false;
  back = hw;
  screen_w = hw_w;
  screen_h = hw_h;
  fb_pitch = hw_pitch;
  if (ui_scale > 1) {
    screen_w = hw_w / ui_scale;
    screen_h = hw_h / ui_scale;
    fb_pitch = screen_w;
    free(logical);
    logical = malloc((unsigned)screen_w * (unsigned)screen_h * 4);
    if (logical)
      back = logical;
    else {
      ui_scale = 1;
      screen_w = hw_w;
      screen_h = hw_h;
      fb_pitch = hw_pitch;
    }
    for (int y = 0; y < hw_h; y++)
      memset(hw + (long)y * hw_pitch, 0, (unsigned)hw_w * 4);
  }
  gfx_init(back, fb_pitch, screen_h); /* surface stride = pitch */
  input_set_bounds(screen_w - 1, screen_h - 1);
  clip_full();
  force_full = true;
  return 0;
}

/* Flush logical rects to the display, replicating each pixel ui_scale x ui_scale */
void present(const struct fbrect *r, int n)
{
  if (ui_scale == 1) {
    fb_present_rects(r, n);
    return;
  }
  int s = ui_scale;
  struct fbrect out[MAXDMG];
  if (n > MAXDMG)
    n = MAXDMG;
  for (int k = 0; k < n; k++) {
    for (int y = r[k].y; y < r[k].y + r[k].h; y++) {
      unsigned *dst = hw + (long)(y * s) * hw_pitch + r[k].x * s;
      const unsigned *src = back + (long)y * fb_pitch + r[k].x;
      for (int x = 0; x < r[k].w; x++)
        for (int i = 0; i < s; i++)
          *dst++ = src[x];
      dst = hw + (long)(y * s) * hw_pitch + r[k].x * s;
      for (int i = 1; i < s; i++)
        memcpy(dst + (long)i * hw_pitch, dst, (unsigned)(r[k].w * s) * 4);
    }
    out[k] = (struct fbrect){r[k].x * s, r[k].y * s, r[k].w * s, r[k].h * s};
  }
  fb_present_rects(out, n);
}
