/* imgdec.h — shared raster-image decoder for the browser.
 *
 * One sniff-and-decode path for the formats the browser rasters to RGBA (SVG,
 * WebP, PNG), used by <img> (resource.c), CSS background images (html.c) and
 * any other consumer, so the magic-byte sniff and per-format decode live in one
 * place rather than being copy-pasted per call site. The favicon path stays
 * separate: it also handles ICO/DIB and decode-and-scales straight into a fixed
 * 16x16 buffer.
 */
#ifndef OWOS_BROWSER_IMGDEC_H
#define OWOS_BROWSER_IMGDEC_H

enum { IMG_FMT_NONE, IMG_FMT_SVG, IMG_FMT_WEBP, IMG_FMT_PNG };

/* Classify a buffer by content (magic bytes / SVG probe). IMG_FMT_NONE =
 * neither. */
int image_sniff(const unsigned char *data, int len);

/* Decode a supported image into a fresh RGBA buffer (byte order R,G,B,A; caller
 * free()s), setting *w and *h. `current_color` resolves an SVG 'currentColor';
 * `max_edge` bounds the rasterized SVG's longer side (<=0 = a sane default).
 * NULL on an unsupported or failed decode. */
unsigned char *image_decode_rgba(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *w,
                                 unsigned *h);

#endif /* OWOS_BROWSER_IMGDEC_H */
