/* fb.h: VESA linear-framebuffer driver (Bochs/QEMU VBE DISPI). */
#ifndef OWOS_FB_H
#define OWOS_FB_H

#include <stdint.h>

/* Switch to a 32-bpp linear mode at the requested size. Returns 0, or -1 if no
 * Bochs-DISPI adapter is present (the kernel stays in VGA text mode). */
int fb_init(uint32_t want_w, uint32_t want_h);

/* Adopt a bootloader-configured framebuffer (phys base, geometry, byte pitch, bpp).
 * 32-bpp direct RGB only. Returns 0, or -1. Preferred over fb_init on real hardware. */
int fb_adopt(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch_bytes, uint32_t bpp);

int fb_active(void); /* 1 once a graphics mode is up            */
uint32_t fb_width(void);
uint32_t fb_height(void);
/* Compositor primitives (SYS_FB_*). fb_backbuffer_phys is 0 in text/single-buffered
 * mode. fb_present_rects flushes back-buffer rects to VRAM via the normal device path. */
uint32_t fb_backbuffer_phys(void);
uint32_t fb_backbuffer_bytes(void);
void fb_dims(int *w, int *h, int *pitch, int *bpp);
struct fb_rect {
  int x, y, w, h;
};
void fb_present_rects(const struct fb_rect *r, int n); /* one WC drain for the batch */

/* Compositor frame-pacing target in Hz, the software present cadence, not a
 * hardware mode. Tunable through /sys/class/graphics/fb0/rate and read by
 * /bin/compositor to pace its loop; fb_set_target_hz clamps to a sane band. */
int fb_target_hz(void);
void fb_set_target_hz(int hz);

/* Runtime mode switching, DISPI path only; fb_can_set_mode is 0 for an adopted
 * (GRUB/UEFI) framebuffer. fb_set_mode returns 0, or -1 if unknown/unsupported.
 * Call from the compositor thread. */
int fb_can_set_mode(void);
int fb_mode_count(void);
int fb_mode_get(int i, uint32_t *w, uint32_t *h);
int fb_set_mode(uint32_t w, uint32_t h);

/* Copy the damaged region of the back buffer to the visible framebuffer. */
void fb_present(void);

/* Callback with each screen rect copied to the framebuffer, for adapters needing an
 * explicit flush (VMware SVGA II). NULL clears it. */
void fb_set_present_hook(void (*cb)(int x, int y, int w, int h));

/* Software brightness (fb_present dims pixels; no backlight register). MAX is a
 * verbatim fast path. fb_brightness_step re-renders the whole screen. IRQ-safe;
 * no-op in text mode or single-buffered. */
#define FB_BRIGHT_MAX 8
#define FB_BRIGHT_MIN 2
void fb_brightness_step(int dir); /* +1 = brighter, -1 = dimmer (clamped) */
int fb_brightness(void);          /* current level FB_BRIGHT_MIN..FB_BRIGHT_MAX */

/* Pack 8-bit channels into the framebuffer's XRGB8888 pixel format. */
static inline uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Console glyph cell. The font is an antialiased coverage atlas baked offline
 * by tools/genfont.c into font_aa.h; change these and you must re-bake. The
 * console grid geometry in app_term.c follows automatically via these names. */
#define FONT_W 9
#define FONT_H 18

/* Render one FONT_W x FONT_H glyph (CP437 byte) at pixel (x,y), alpha-blending
 * the atlas coverage between fg and bg. If bg has bit 24 set it is transparent
 * (the coverage is blended over whatever is already in the back buffer). */
#define FB_TRANSPARENT 0x01000000u
void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg);
/* Draw a NUL-terminated string of glyphs left to right (FONT_W advance). */
void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Draw one character to the framebuffer boot console; fed by vga.c while verbose and before the WM is up. */
void fb_con_putc(char c);

#endif /* OWOS_FB_H */
