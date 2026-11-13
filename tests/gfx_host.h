/* Host stand-ins for the gfx drawing primitives and fonts (tests/gfx_host.c), so the widget and style code in
 * user/lib/gfx links without a framebuffer. Every glyph is GFX_HOST_CW wide; a font's line is 6/5 of its px. A
 * suite installs the hooks it wants to watch and clears them with gfx_host_reset(). */
#ifndef OWOS_GFX_HOST_H
#define OWOS_GFX_HOST_H

#define GFX_HOST_CW 8

extern void (*gfx_host_fill)(int x, int y, int w, int h, unsigned color);
extern void (*gfx_host_pixel)(int x, int y, unsigned color);
extern void (*gfx_host_text)(int x, int y, const char *s, unsigned fg);

void gfx_host_reset(void);

#endif
