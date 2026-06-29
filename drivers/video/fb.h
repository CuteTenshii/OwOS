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
uint32_t fb_pitch_pixels(void);    /* back buffer row stride in pixels (>= width) */

/* Runtime resolution switching, available only on the DISPI path (fb_init): we
 * can reprogram QEMU's std-vga adapter, but a framebuffer adopted from a
 * bootloader (fb_adopt: GRUB/UEFI GOP) is fixed and fb_can_set_mode returns 0.
 * fb_mode_get enumerates the offered modes (all <= the capacity mapped at init);
 * fb_set_mode reprograms to one of them and returns 0, or -1 if the mode is
 * unknown / mode-setting is unsupported. Must be called from the compositor
 * thread — it reshapes the buffers the WM draws into. */
int fb_can_set_mode(void);
int fb_mode_count(void);
int fb_mode_get(int i, uint32_t *w, uint32_t *h);
int fb_set_mode(uint32_t w, uint32_t h);

/* Copy the damaged region of the back buffer to the visible framebuffer.
 * Drawing happens off-screen (no tearing) and only the rectangle touched since
 * the last present is copied to VRAM — essential for speed on real GPUs, where
 * VRAM writes are slow. */
void fb_present(void);

/* Register a callback invoked by fb_present (and the full-screen redraw paths)
 * with the rectangle that was just copied to the visible framebuffer, in screen
 * pixels. A paravirtual adapter that needs an explicit "flush this region to the
 * host" message — VMware SVGA II — uses this; pass NULL to clear it. A plain
 * LFB (QEMU DISPI, GRUB/UEFI adopt) needs no hook and leaves it unset. */
void fb_set_present_hook(void (*cb)(int x, int y, int w, int h));

/* Software display brightness, driven by the keyboard's brightness keys (there
 * is no panel-backlight register we can reach, so fb_present dims pixels as it
 * copies the back buffer to the screen). Levels run FB_BRIGHT_MIN..FB_BRIGHT_MAX;
 * at MAX the copy is a verbatim fast path. fb_brightness_step adjusts the level
 * and re-renders the whole screen at once so the change shows immediately. Safe
 * to call from an IRQ; a no-op in text mode or single-buffered fallback. */
#define FB_BRIGHT_MAX 8
#define FB_BRIGHT_MIN 2
void fb_brightness_step(int dir);  /* +1 = brighter, -1 = dimmer (clamped)      */
int  fb_brightness(void);          /* current level FB_BRIGHT_MIN..FB_BRIGHT_MAX */

/* Clip rectangle for drawing primitives. The compositor sets it to a damaged
 * region and redraws the scene clipped to it, so only that area is touched and
 * presented. fb_clip_full restores the whole screen. */
void fb_set_clip(int x, int y, int w, int h);
void fb_clip_full(void);
/* Read back the active (clamped) clip rectangle. */
void fb_get_clip(int *x, int *y, int *w, int *h);

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

/* Proportional sans-serif font for window chrome (titles, taskbar, menus).
 * Variable per-glyph advance; the cell is UIFONT_W x UIFONT_H but glyphs only
 * occupy their own width. fb_ui_text blends fg over the existing back-buffer
 * pixels (fill the background first), so there is no bg argument. fb_ui_width
 * returns the pixel width of a string for centring / right-alignment. */
#define UIFONT_W 16
#define UIFONT_H 18
void fb_ui_text(int x, int y, const char *s, uint32_t fg);
int  fb_ui_width(const char *s);

/* Framebuffer boot console: draw one character to a scrolling text console on
 * the framebuffer. Used during boot (graphics mode, before the WM exists) to
 * show the verbose bring-up log on screen; vga.c feeds it from its console
 * funnel while verbose and the compositor isn't up yet. */
void fb_con_putc(char c);

#endif /* OWOS_FB_H */
