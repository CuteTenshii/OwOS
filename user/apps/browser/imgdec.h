/* imgdec.h - shared raster-image decoder for the browser */
#ifndef OWOS_BROWSER_IMGDEC_H
#define OWOS_BROWSER_IMGDEC_H

#include "imganim.h"

enum { IMG_FMT_NONE, IMG_FMT_SVG, IMG_FMT_WEBP, IMG_FMT_PNG, IMG_FMT_GIF };

/* Classify a buffer by content (magic bytes / SVG probe) */
int image_sniff(const unsigned char *data, int len);

/* Decode a supported image into a fresh RGBA buffer (byte order R,G,B,A */
unsigned char *image_decode_rgba(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *w,
                                 unsigned *h);

/* Decode the first frame of a GIF or animated WebP, keeping the player at that frame so playback can continue: a
 * fresh RGBA buffer, plus *anim (the player, reading from *keep, a private copy of the file) and *delay_ms (how long
 * the first frame shows). The caller owns all three; NULL (nothing allocated) if the file is not decodable. */
unsigned char *image_decode_first_frame(const unsigned char *data, int len, struct imganim **anim, unsigned char **keep,
                                        unsigned *w, unsigned *h, unsigned *delay_ms);

#endif /* OWOS_BROWSER_IMGDEC_H */
