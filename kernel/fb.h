/* fb.h — VESA linear-framebuffer driver (Bochs/QEMU VBE DISPI). */
#ifndef OWOS_FB_H
#define OWOS_FB_H

#include <stdint.h>

/* Try to switch into a 32-bpp linear graphics mode at the requested size.
 * Returns 0 on success (graphics mode active, framebuffer mapped) or -1 if no
 * Bochs-DISPI-capable adapter is present, in which case the kernel stays in
 * VGA text mode. */
int fb_init(uint32_t want_w, uint32_t want_h);

/* Adopt a framebuffer a bootloader already configured (GRUB Multiboot / UEFI),
 * given its physical base, geometry, byte pitch, and bpp. Only 32-bpp direct
 * RGB is supported. Returns 0 on success, -1 otherwise. Preferred over fb_init
 * on real hardware, where we cannot drive the GPU to set a mode ourselves. */
int fb_adopt(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch_bytes, uint32_t bpp);

int      fb_active(void);          /* 1 once a graphics mode is up            */
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t *fb_ptr(void);            /* the back buffer drawing primitives target */

/* Copy the damaged region of the back buffer to the visible framebuffer.
 * Drawing happens off-screen (no tearing) and only the rectangle touched since
 * the last present is copied to VRAM — essential for speed on real GPUs, where
 * VRAM writes are slow. */
void fb_present(void);

/* Clip rectangle for drawing primitives. The compositor sets it to a damaged
 * region and redraws the scene clipped to it, so only that area is touched and
 * presented. fb_clip_full restores the whole screen. */
void fb_set_clip(int x, int y, int w, int h);
void fb_clip_full(void);

/* Pack 8-bit channels into the framebuffer's XRGB8888 pixel format. */
static inline uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void fb_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
/* Draw a 1px outline rectangle. */
void fb_rect(int x, int y, int w, int h, uint32_t color);
/* Copy `h` rows of `w` pixels from src (row stride `src_pitch` in pixels) to
 * screen position (x,y). Used by the compositor to blit window buffers. */
void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch);

/* Render one 8x16 glyph (CP437 byte) at pixel (x,y). If bg has bit 24 set it
 * is treated as transparent (only foreground pixels are drawn). */
#define FB_TRANSPARENT 0x01000000u
void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg);
/* Draw a NUL-terminated string of 8x16 glyphs left to right. */
void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

#endif /* OWOS_FB_H */
