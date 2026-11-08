/* fb.c: linear framebuffer driver over the Bochs/QEMU VBE DISPI interface.
 * Sets a 32-bpp mode via PCI BAR0 with no BIOS call; fb_init returns -1 if no
 * DISPI adapter answers (the kernel keeps its VGA text console). */

#include "fb.h"
#include "edid.h"
#include "font_aa.h"
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "mtrr.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include <stdbool.h>
#include <stddef.h>

#define VBE_DISPI_INDEX 0x01CE
#define VBE_DISPI_DATA 0x01CF
#define VBE_DISPI_ID 0x0
#define VBE_DISPI_XRES 0x1
#define VBE_DISPI_YRES 0x2
#define VBE_DISPI_BPP 0x3
#define VBE_DISPI_ENABLE 0x4
#define VBE_DISPI_VWIDTH 0x6
#define VBE_DISPI_VHEIGHT 0x7
#define VBE_DISPI_XOFF 0x8
#define VBE_DISPI_YOFF 0x9

#define VBE_ENABLED 0x01
#define VBE_LFB_ENABLED 0x40

static uint32_t *front; /* the visible linear framebuffer (XRGB8888)    */
static uint32_t *fb;    /* off-screen back buffer; primitives draw here */
static uint32_t fb_w, fb_h;
static uint32_t fb_pitch;  /* pixels per row                                */
static uint32_t fb_pixels; /* visible pixels in the current mode (pitch*h)   */
static uint32_t fb_cap;    /* pixels mapped/allocated (>= fb_pixels)         */
static bool active;
static bool single;   /* 1 when there is no back buffer (draw direct)   */
static bool can_mode; /* 1 = DISPI path: the mode can be reprogrammed   */

/* Runtime-switchable modes (DISPI only); buffers are sized for the largest, keep it last. */
static const struct {
  uint16_t w, h;
} fb_modes[] = {
    {800, 600},  {1024, 768}, {1280, 720}, {1280, 800},  {1280, 1024},
    {1366, 768}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080},
};
#define FB_NMODES ((int)(sizeof fb_modes / sizeof fb_modes[0]))
#define FB_MAX_W 1920u
#define FB_MAX_H 1080u

/* Display brightness FB_BRIGHT_MIN..FB_BRIGHT_MAX (max = undimmed), a per-channel scale on present. */
static int brightness = FB_BRIGHT_MAX;

/* Compositor frame-pacing target in Hz (not a hardware mode); set via /sys/class/graphics/fb0/rate. */
#define FB_HZ_MIN 20
#define FB_HZ_MAX 240
static int target_hz = 60;

/* UI scale: 0 = auto from the monitor's DPI, else an integer factor; via fb0/scale. */
#define FB_SCALE_MAX 4
#define FB_LOGICAL_MIN_W 640
#define FB_LOGICAL_MIN_H 400
static int ui_scale;

/* Scale a pixel by brightness (>>3, MAX is 8); red and blue share one multiply in 16-bit lanes. */
static inline uint32_t dim_px(uint32_t p)
{
  uint32_t k = (uint32_t)brightness;
  uint32_t rb = (((p & 0x00FF00FFu) * k) >> 3) & 0x00FF00FFu;
  uint32_t g = (((p & 0x0000FF00u) * k) >> 3) & 0x0000FF00u;
  return rb | g;
}

/* 32-bpp run fill (rep stosl); memset cannot broadcast a 32-bit pixel. */
static inline void fill32(uint32_t *d, uint32_t v, int n)
{
  int d0, d1;
  __asm__ volatile("rep ; stosl" : "=&D"(d0), "=&c"(d1) : "a"(v), "0"(d), "1"(n) : "memory");
}

/* Damage box since the last present; empty when dirty_x1 <= dirty_x0. */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

/* Intersect (x,y,w,h) with the screen into out[], returns 0 if empty. */
static bool clip(int x, int y, int w, int h, int *ox, int *oy, int *ow, int *oh)
{
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w;
  if (x1 > (int)fb_w)
    x1 = (int)fb_w;
  int y1 = y + h;
  if (y1 > (int)fb_h)
    y1 = (int)fb_h;
  if (x0 >= x1 || y0 >= y1)
    return false;
  *ox = x0;
  *oy = y0;
  *ow = x1 - x0;
  *oh = y1 - y0;
  return true;
}

/* Grow the damage box to include the (already clipped) rectangle. */
static void mark(int x, int y, int w, int h)
{
  if (dirty_x1 <= dirty_x0) {
    dirty_x0 = x;
    dirty_y0 = y;
    dirty_x1 = x + w;
    dirty_y1 = y + h;
    return;
  }
  if (x < dirty_x0)
    dirty_x0 = x;
  if (y < dirty_y0)
    dirty_y0 = y;
  if (x + w > dirty_x1)
    dirty_x1 = x + w;
  if (y + h > dirty_y1)
    dirty_y1 = y + h;
}

static void alloc_backbuffer(void);

/* Optional per-rect flush hook for paravirtual adapters (VMware SVGA II). */
static void (*present_hook)(int, int, int, int);

void fb_set_present_hook(void (*cb)(int, int, int, int)) { present_hook = cb; }

/* Make the framebuffer write-combining (PAT entry 1). Harmless under QEMU. */
static void enable_wc(void)
{
  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  if (!(d & (1u << 16))) /* CPUID.01H:EDX.PAT, no PAT, give up      */
    return;

  /* Set PAT entry 1 (MSR 0x277) to WC (0x01), leaving the firmware's other entries intact. */
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
  lo = (lo & ~0x0000FF00u) | (0x01u << 8);
  __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x277));

  __asm__ volatile("wbinvd" ::: "memory");
  paging_mark_wc((uint32_t)(uintptr_t)front, fb_cap * 4);

  /* A firmware MTRR marking the fb UC overrides PAT-WC; also pin the range WC (no-op on QEMU). */
  mtrr_set_wc((uint32_t)(uintptr_t)front, fb_cap * 4);
}

static void dispi_write(uint16_t index, uint16_t value)
{
  outw(VBE_DISPI_INDEX, index);
  outw(VBE_DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t index)
{
  outw(VBE_DISPI_INDEX, index);
  return inw(VBE_DISPI_DATA);
}

/* Physical base of the first PCI display controller's BAR0, or 0 if none. */
static uint32_t find_lfb_phys(void)
{
  for (int bus = 0; bus < 256; bus++) {
    for (int slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read32(bus, slot, 0, 0x00);
      if ((id & 0xFFFF) == 0xFFFF)
        continue;
      uint32_t cls = pci_read32(bus, slot, 0, 0x08);
      if (((cls >> 24) & 0xFF) != 0x03)
        continue;
      uint32_t bar0 = pci_read32(bus, slot, 0, 0x10);
      if (bar0 & 0x1)
        continue;
      uint32_t cmd = pci_read32(bus, slot, 0, 0x04);
      pci_write32(bus, slot, 0, 0x04, cmd | 0x6);
      return bar0 & 0xFFFFFFF0u;
    }
  }
  return 0;
}

/* Adopt a bootloader (GRUB/UEFI GOP) framebuffer; pitch in bytes, 32-bpp RGB only. 0 or -1. */
int fb_adopt(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch_bytes, uint32_t bpp)
{
  if (bpp != 32 || phys == 0 || w == 0 || h == 0)
    return -1;
  if (pitch_bytes & 0x3)
    return -1;

  uint32_t pitch_px = pitch_bytes / 4;
  uint32_t pixels = pitch_px * h;

  /* Map the whole visible region; paging_identity_map allocates page tables as needed. */
  if (paging_identity_map(phys, pixels * 4) < 0)
    return -1;
  front = (uint32_t *)(uintptr_t)phys;

  fb_w = w;
  fb_h = h;
  fb_pitch = pitch_px;
  fb_pixels = pixels;
  fb_cap = pixels;
  can_mode = false;

  enable_wc();
  alloc_backbuffer();

  active = true;
  fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
  fb_present();
  return 0;
}

/* Cached-RAM back buffer (VRAM reads are slow); falls back to single-buffered. */
static void alloc_backbuffer(void)
{
  uint32_t bytes = fb_cap * 4;
  uint32_t bb = pmm_alloc_contig(bytes);
  if (bb && paging_identity_map(bb, bytes) == 0) {
    fb = (uint32_t *)(uintptr_t)bb;
    single = false;
  } else {
    fb = front;
    single = true;
  }
}

int fb_init(uint32_t want_w, uint32_t want_h)
{
  /* The DISPI ID register reads back 0xB0Cx on a capable adapter. */
  uint16_t magic = dispi_read(VBE_DISPI_ID);
  if ((magic & 0xFFF0) != 0xB0C0)
    return -1;

  uint32_t lfb = find_lfb_phys();
  if (!lfb)
    return -1;

  dispi_write(VBE_DISPI_ENABLE, 0);
  dispi_write(VBE_DISPI_XRES, (uint16_t)want_w);
  dispi_write(VBE_DISPI_YRES, (uint16_t)want_h);
  dispi_write(VBE_DISPI_BPP, 32);
  dispi_write(VBE_DISPI_VWIDTH, (uint16_t)want_w);
  dispi_write(VBE_DISPI_VHEIGHT, (uint16_t)want_h);
  dispi_write(VBE_DISPI_XOFF, 0);
  dispi_write(VBE_DISPI_YOFF, 0);
  dispi_write(VBE_DISPI_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);

  if (dispi_read(VBE_DISPI_XRES) != (uint16_t)want_w || dispi_read(VBE_DISPI_YRES) != (uint16_t)want_h) {
    dispi_write(VBE_DISPI_ENABLE, 0);
    return -1;
  }

  fb_w = want_w;
  fb_h = want_h;
  fb_pitch = want_w;
  fb_pixels = want_w * want_h;

  /* Back the LFB at the largest mode so fb_set_mode needs no remap/realloc. */
  fb_cap = FB_MAX_W * FB_MAX_H;
  if (fb_cap < fb_pixels)
    fb_cap = fb_pixels;

  if (paging_identity_map(lfb, fb_cap * 4) < 0)
    return -1;
  front = (uint32_t *)(uintptr_t)lfb;
  enable_wc();
  alloc_backbuffer();

  active = true;
  can_mode = true;
  fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
  fb_present();
  return 0;
}

int fb_can_set_mode(void) { return active && can_mode; }
int fb_mode_count(void) { return FB_NMODES; }

int fb_mode_get(int i, uint32_t *w, uint32_t *h)
{
  if (i < 0 || i >= FB_NMODES)
    return 0;
  *w = fb_modes[i].w;
  *h = fb_modes[i].h;
  return 1;
}

/* Reprogram DISPI to an offered mode, clear and present. Compositor thread only. */
int fb_set_mode(uint32_t w, uint32_t h)
{
  if (!active || !can_mode)
    return -1;

  bool known = false;
  for (int i = 0; i < FB_NMODES; i++)
    if (fb_modes[i].w == w && fb_modes[i].h == h) {
      known = true;
      break;
    }
  if (!known || w * h > fb_cap)
    return -1;
  if (w == fb_w && h == fb_h)
    return 0;

  dispi_write(VBE_DISPI_ENABLE, 0);
  dispi_write(VBE_DISPI_XRES, (uint16_t)w);
  dispi_write(VBE_DISPI_YRES, (uint16_t)h);
  dispi_write(VBE_DISPI_BPP, 32);
  dispi_write(VBE_DISPI_VWIDTH, (uint16_t)w);
  dispi_write(VBE_DISPI_VHEIGHT, (uint16_t)h);
  dispi_write(VBE_DISPI_XOFF, 0);
  dispi_write(VBE_DISPI_YOFF, 0);
  dispi_write(VBE_DISPI_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
  if (dispi_read(VBE_DISPI_XRES) != (uint16_t)w || dispi_read(VBE_DISPI_YRES) != (uint16_t)h)
    return -1;

  fb_w = w;
  fb_h = h;
  fb_pitch = w;
  fb_pixels = w * h;
  fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
  fb_present();
  return 0;
}

/* Copy [x,y,w,h) of the back buffer to VRAM, dimmed; rect must be pre-clipped. */
static void copy_region(int x, int y, int w, int h)
{
  if (single)
    return;
  bool dim = brightness < FB_BRIGHT_MAX;
  for (int row = y; row < y + h; row++) {
    const uint32_t *s = fb + (uint32_t)row * fb_pitch + x;
    uint32_t *d = front + (uint32_t)row * fb_pitch + x;
    if (dim) {
      for (int i = 0; i < w; i++)
        d[i] = dim_px(s[i]);
    } else {
      memcpy(d, s, (uint32_t)w * 4);
    }
  }
}

/* Drain the WC buffer; without SFENCE small presents stayed invisible on real hardware. */
static void drain_wc(void)
{
  if (!single)
    __asm__ volatile("sfence" ::: "memory");
}

static void present_full(void)
{
  copy_region(0, 0, (int)fb_w, (int)fb_h);
  drain_wc();
  if (present_hook)
    present_hook(0, 0, (int)fb_w, (int)fb_h);
}

void fb_present(void)
{
  if (dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0) {
    int rx = dirty_x0, ry = dirty_y0;
    int rw = dirty_x1 - dirty_x0, rh = dirty_y1 - dirty_y0;
    copy_region(rx, ry, rw, rh);
    drain_wc();
    if (present_hook)
      present_hook(rx, ry, rw, rh);
  }
  dirty_x1 = dirty_x0;
}

/* Flush every back-buffer rect clipped to the screen with a single WC drain. */
void fb_present_rects(const struct fb_rect *r, int n)
{
  if (!active)
    return;
  for (int i = 0; i < n; i++) {
    int x, y, w, h;
    if (clip(r[i].x, r[i].y, r[i].w, r[i].h, &x, &y, &w, &h))
      copy_region(x, y, w, h);
  }
  drain_wc();
  if (!present_hook)
    return;
  for (int i = 0; i < n; i++) {
    int x, y, w, h;
    if (clip(r[i].x, r[i].y, r[i].w, r[i].h, &x, &y, &w, &h))
      present_hook(x, y, w, h);
  }
}

/* Step brightness and re-render the whole screen (the WM only presents damage). IRQ-safe. */
void fb_brightness_step(int dir)
{
  if (!active || single)
    return;
  int nb = brightness + dir;
  if (nb < FB_BRIGHT_MIN)
    nb = FB_BRIGHT_MIN;
  if (nb > FB_BRIGHT_MAX)
    nb = FB_BRIGHT_MAX;
  if (nb == brightness)
    return;
  brightness = nb;
  present_full();
}

int fb_brightness(void) { return brightness; }

int fb_active(void) { return active; }
uint32_t fb_width(void) { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

/* Compositor primitives (SYS_FB_*); back buffer is identity-mapped, 0 if text or single-buffered. */
uint32_t fb_backbuffer_phys(void) { return (active && !single) ? (uint32_t)(uintptr_t)fb : 0; }

uint32_t fb_backbuffer_bytes(void) { return fb_cap * 4u; }

void fb_dims(int *w, int *h, int *pitch, int *bpp)
{
  if (w)
    *w = (int)fb_w;
  if (h)
    *h = (int)fb_h;
  if (pitch)
    *pitch = (int)fb_pitch;
  if (bpp)
    *bpp = 32;
}

int fb_ui_scale(void) { return ui_scale; }
void fb_set_ui_scale(int s) { ui_scale = s < 0 || s > FB_SCALE_MAX ? ui_scale : s; }

int fb_ui_scale_active(void)
{
  int s = ui_scale;
  if (s == 0) {
    int dpi = edid_dpi();
    s = dpi < 144 ? 1 : dpi < 240 ? 2 : dpi < 336 ? 3 : 4;
  }
  while (s > 1 && (fb_w / (uint32_t)s < FB_LOGICAL_MIN_W || fb_h / (uint32_t)s < FB_LOGICAL_MIN_H))
    s--;
  return s;
}

int fb_target_hz(void) { return target_hz; }
void fb_set_target_hz(int hz)
{
  if (hz < FB_HZ_MIN)
    hz = FB_HZ_MIN;
  if (hz > FB_HZ_MAX)
    hz = FB_HZ_MAX;
  target_hz = hz;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
  int cx, cy, cw, ch;
  if (!clip(x, y, w, h, &cx, &cy, &cw, &ch))
    return;
  for (int row = 0; row < ch; row++) {
    uint32_t *p = fb + (cy + row) * (int)fb_pitch + cx;
    fill32(p, color, cw);
  }
  mark(cx, cy, cw, ch);
}

/* Blend src (0..255 per channel) over dst by coverage a (0..255). */
static inline uint32_t blend(uint32_t fg, uint32_t bg, unsigned a)
{
  if (a == 0)
    return bg;
  if (a == 255)
    return fg;
  unsigned ia = 255 - a;
  unsigned r = ((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia + 128;
  unsigned g = ((fg >> 8) & 0xff) * a + ((bg >> 8) & 0xff) * ia + 128;
  unsigned b = (fg & 0xff) * a + (bg & 0xff) * ia + 128;
  return (((r + (r >> 8)) >> 8) << 16) | (((g + (g >> 8)) >> 8) << 8) | ((b + (b >> 8)) >> 8);
}

void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg)
{
  int cx, cy, cw, chh;
  if (!clip(x, y, FONT_W, FONT_H, &cx, &cy, &cw, &chh))
    return;
  bool transparent = (bg & FB_TRANSPARENT) != 0;
  for (int row = 0; row < chh; row++) {
    int sy = cy + row;
    const unsigned char *cov = font_aa[ch][sy - y];
    uint32_t *d = fb + sy * (int)fb_pitch;
    for (int col = 0; col < cw; col++) {
      int sx = cx + col;
      unsigned a = cov[sx - x];
      /* Transparent bg: blend over the cached back buffer pixel; opaque bg: fg over bg. */
      d[sx] = transparent ? blend(fg, d[sx], a) : blend(fg, bg, a);
    }
  }
  mark(cx, cy, cw, chh);
}

void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
  for (; *s; s++, x += FONT_W)
    fb_glyph(x, y, (unsigned char)*s, fg, bg);
}

/* Framebuffer boot console: scrolling text fed a char at a time by vga.c until the WM takes over. */

#define CON_MARGIN 4
static int con_x = CON_MARGIN, con_y = CON_MARGIN;

static void fb_con_newline(void)
{
  con_x = CON_MARGIN;
  con_y += FONT_H;
  if (con_y + FONT_H <= (int)fb_h)
    return;
  /* Scroll the back buffer up one text row and clear the new bottom row. */
  uint32_t move = fb_h - FONT_H;
  memmove(fb, fb + (uint32_t)FONT_H * fb_pitch, move * fb_pitch * 4);
  fill32(fb + move * fb_pitch, 0x000000, (int)((fb_h - move) * fb_pitch));
  con_y -= FONT_H;
  /* The scroll touched every row, so a damage-rect present won't do. */
  present_full();
}

void fb_con_putc(char c)
{
  if (!active)
    return;
  if (c == '\r')
    return;
  if (c == '\n') {
    fb_con_newline();
    return;
  }
  if (c == '\t') {
    do {
      fb_con_putc(' ');
    } while (((con_x - CON_MARGIN) / FONT_W) % 8);
    return;
  }
  if (c == '\b') {
    if (con_x > CON_MARGIN)
      con_x -= FONT_W;
    return;
  }
  if (con_x + FONT_W > (int)fb_w)
    fb_con_newline();
  fb_glyph(con_x, con_y, (unsigned char)c, 0x33ff66, 0x000000);
  fb_present();
  con_x += FONT_W;
}
