/* svg.h: rasterize SVG into an ARGB bitmap (markup parsed by dom.c). */
#ifndef OWOS_SVG_H
#define OWOS_SVG_H

/* Does the buffer look like SVG (an <svg> root)? */
int svg_probe(const char *data, int len);

/* Intrinsic size (viewBox, else width/height) into w,h. Returns 1, or 0 if none. */
int svg_size(const char *data, int len, int *w, int *h);

/* Render into w*h 0xAARRGGBB pixels, aspect preserved. current_color (0xRRGGBB) resolves currentColor. */
int svg_render(const char *data, int len, int w, int h, unsigned *out, unsigned current_color);

/* Fonts <text> is drawn with (sans, mono); by default the Liberation faces under /etc/fonts are loaded on first use. */
struct font;
void svg_set_fonts(struct font *sans, struct font *mono);

#endif /* OWOS_SVG_H */
