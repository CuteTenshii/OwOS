/* fb.h: VESA linear-framebuffer driver (Bochs/QEMU VBE DISPI). */
#ifndef OWOS_FB_H
#define OWOS_FB_H

#include <stdint.h>

/* Switch to a 32-bpp linear mode of the given size. 0, or -1 if no Bochs-DISPI adapter. */
int fb_init(uint32_t want_w, uint32_t want_h);

/* Adopt a bootloader framebuffer (phys base, geometry, byte pitch, bpp); 32-bpp RGB only. 0 or -1. */
int fb_adopt(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch_bytes, uint32_t bpp);

int fb_active(void); /* 1 once a graphics mode is up            */
uint32_t fb_width(void);
uint32_t fb_height(void);
/* Compositor primitives (SYS_FB_*). fb_backbuffer_phys is 0 in text/single-buffered mode. */
uint32_t fb_backbuffer_phys(void);
uint32_t fb_backbuffer_bytes(void);
void fb_dims(int *w, int *h, int *pitch, int *bpp);
struct fb_rect {
  int x, y, w, h;
};
void fb_present_rects(const struct fb_rect *r, int n); /* one WC drain for the batch */

/* Compositor frame-pacing target in Hz (software present cadence); fb_set_target_hz clamps it. */
int fb_target_hz(void);
void fb_set_target_hz(int hz);

/* Desktop UI scale (integer pixel multiplier). The setting is 0 (auto, from the
 * monitor's EDID DPI) or 1..4; the active value is what the compositor applies,
 * reduced so the logical screen stays at least 640x400. */
int fb_ui_scale(void);
void fb_set_ui_scale(int s); /* ignores values outside 0..4 */
int fb_ui_scale_active(void);

/* Runtime mode switching: DISPI offers the fb_mode_get list; adapters registered with fb_set_mode_hooks accept any
 * size up to their capacity. Returns 0, or -1. Compositor thread only. */
int fb_can_set_mode(void);
int fb_mode_count(void);
int fb_mode_get(int i, uint32_t *w, uint32_t *h);
int fb_set_mode(uint32_t w, uint32_t h);

/* Adapter mode support. set reprograms the hardware to w x h and returns the new pitch in bytes (0 / -1);
 * poll (optional) reports the size the host wants, returning 1 when it changed. fb_set_capacity(pixels) before
 * fb_adopt reserves room for larger modes. fb_request_size queues a host-requested size (any thread);
 * fb_poll_resize, run from SYS_FB_INFO, applies it. */
void fb_set_mode_hooks(int (*set)(uint32_t w, uint32_t h, uint32_t *pitch_bytes),
                       int (*poll)(uint32_t *w, uint32_t *h));
void fb_set_capacity(uint32_t pixels);
void fb_request_size(uint32_t w, uint32_t h);
void fb_poll_resize(void);

/* Copy the damaged region of the back buffer to the visible framebuffer. */
void fb_present(void);

/* cb runs per rect copied to the framebuffer; flush (optional) once after each batch. NULL clears. */
void fb_set_present_hook(void (*cb)(int x, int y, int w, int h), void (*flush)(void));

/* Hardware cursor provided by the display adapter. fb_cursor_set defines it from a w*h ARGB bitmap (NULL hides it
 * and returns to software drawing); fb_cursor_move positions it, scaled by the scale given to set. IRQ-safe.
 * fb_cursor_set returns -1 when no adapter cursor exists. */
void fb_set_cursor_hooks(int (*set)(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int scale),
                         void (*move)(int x, int y));
int fb_cursor_set(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int scale);
void fb_cursor_move(int x, int y);

/* Software brightness (fb_present dims pixels). fb_brightness_step re-renders the screen. IRQ-safe. */
#define FB_BRIGHT_MAX 8
#define FB_BRIGHT_MIN 2
void fb_brightness_step(int dir); /* +1 = brighter, -1 = dimmer (clamped) */
int fb_brightness(void);          /* current level FB_BRIGHT_MIN..FB_BRIGHT_MAX */

/* Pack 8-bit channels into the framebuffer's XRGB8888 pixel format. */
static inline uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Console glyph cell; the font atlas is baked by tools/genfont.c into font_aa.h, re-bake on change. */
#define FONT_W 9
#define FONT_H 18

/* Draw one FONT_W x FONT_H glyph (CP437 byte) at (x,y); bg bit 24 set means transparent. */
#define FB_TRANSPARENT 0x01000000u
void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg);
/* Draw a NUL-terminated string of glyphs left to right (FONT_W advance). */
void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Draw one character to the boot console; fed by vga.c while verbose and before the WM is up. */
void fb_con_putc(char c);

#endif /* OWOS_FB_H */
