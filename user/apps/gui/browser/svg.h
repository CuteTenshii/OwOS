/* svg.h — rasterize SVG into an ARGB bitmap. The SVG markup is parsed by the
 * shared DOM parser (dom.c); this renders the shapes. */
#ifndef OWOS_SVG_H
#define OWOS_SVG_H

/* Does this byte buffer look like SVG (an <svg> root, possibly behind an xml
 * declaration, doctype or comment)? */
int svg_probe(const char *data, int len);

/* Intrinsic size (viewBox, else width/height): fills w,h. Returns 1, or 0 if none. */
int svg_size(const char *data, int len, int *w, int *h);

/* Render `len` bytes of SVG into the caller's w*h buffer of 0xAARRGGBB pixels.
 * Returns 1 on success (out filled), 0 if it isn't usable SVG. Aspect ratio is
 * preserved (centered); edges are antialiased by supersampling. `current_color`
 * (0xRRGGBB) is what `currentColor` resolves to — pass the element's CSS color
 * for inline <svg>, or a default (e.g. black) for a standalone image. */
int svg_render(const char *data, int len, int w, int h, unsigned *out,
               unsigned current_color);

#endif /* OWOS_SVG_H */
