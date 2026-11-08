/* gif.h — a GIF87a/89a decoder with frame compositing for user programs.
 *
 * Untrusted-input parsing belongs in user space: a malformed or hostile GIF can only
 * corrupt this process, never the kernel. The decoder works on a complete in-memory
 * buffer and composes frames one at a time into a caller-owned RGBA canvas, so an
 * animation costs one canvas (plus a lazily allocated backup for disposal method 3)
 * no matter how many frames it has.
 *
 * Compositing follows browsers: the canvas starts transparent, disposal method 2
 * clears the frame's rectangle to transparent (not to the background colour),
 * method 3 restores what was there before the frame, and transparent-index pixels
 * leave the canvas untouched.
 */
#ifndef OWOS_GIF_H
#define OWOS_GIF_H

enum {
  GIF_OK = 0,
  GIF_FRAME = 1,    /* gif_next: a frame was composed                    */
  GIF_END = 0,      /* gif_next: no more frames                          */
  GIF_EBADSIG = -1, /* not a GIF                                         */
  GIF_ETRUNC = -3,  /* stream ended before the first frame               */
  GIF_EFORMAT = -4, /* corrupt block structure or LZW data               */
  GIF_ENOMEM = -5,  /* allocation failed                                 */
};

struct gif;

/* Parse the header and global palette. `data` must outlive the decoder. Sets *w and *h to the
 * logical screen size (1..16384 per side). Returns GIF_OK or a GIF_E* code. */
int gif_open(struct gif **out, const unsigned char *data, int len, unsigned *w, unsigned *h);

/* Compose the next frame into `canvas` (w*h*4 bytes, straight RGBA, caller-owned and zeroed
 * before the first call; it must persist between calls). *delay_ms (if non-NULL) receives the
 * frame's delay in milliseconds (0 when the frame carries no delay). Returns GIF_FRAME,
 * GIF_END (the canvas keeps the last frame), or a negative GIF_E* code for a corrupt stream.
 * A frame whose pixel data is cut short is composed as far as it decoded. */
int gif_next(struct gif *g, unsigned char *canvas, int *delay_ms);

/* Restart at the first frame: zeroes `canvas` and forgets any pending disposal. */
void gif_rewind(struct gif *g, unsigned char *canvas);

/* Total number of plays: 0 = forever, otherwise the count (1 when the file has no
 * NETSCAPE2.0 loop extension). A NETSCAPE count of N repeats N times after the first play. */
int gif_loops(const struct gif *g);

void gif_close(struct gif *g);

#endif /* OWOS_GIF_H */
