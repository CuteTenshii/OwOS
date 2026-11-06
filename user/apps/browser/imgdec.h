/* imgdec.h - shared raster-image decoder for the browser */
#ifndef OWOS_BROWSER_IMGDEC_H
#define OWOS_BROWSER_IMGDEC_H

enum { IMG_FMT_NONE, IMG_FMT_SVG, IMG_FMT_WEBP, IMG_FMT_PNG };

/* Classify a buffer by content (magic bytes / SVG probe) */
int image_sniff(const unsigned char *data, int len);

/* Decode a supported image into a fresh RGBA buffer (byte order R,G,B,A */
unsigned char *image_decode_rgba(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *w,
                                 unsigned *h);

#endif /* OWOS_BROWSER_IMGDEC_H */
