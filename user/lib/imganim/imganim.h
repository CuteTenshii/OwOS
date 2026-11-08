/* imganim.h - frame player over the GIF and animated-WebP decoders.
 *
 * Both formats compose frames onto a persistent canvas; this wraps them behind one
 * open/step/close interface and applies the playback rules (loop count, the browser
 * minimum frame delay, single-frame files treated as stills).
 */
#ifndef OWOS_IMGANIM_H
#define OWOS_IMGANIM_H

enum { IMGANIM_NONE, IMGANIM_GIF, IMGANIM_WEBP };

struct imganim {
  unsigned w, h;
  unsigned char *rgba; /* the canvas after the latest step: w*h*4 straight RGBA */
  int fmt, loops_left;
  int pass_frames; /* frames composed in the current pass */
  void *dec;
};

/* IMGANIM_GIF for a GIF header, IMGANIM_WEBP for a WebP whose VP8X header flags animation, else IMGANIM_NONE. */
int imganim_sniff(const unsigned char *data, int len);

/* Open a GIF or animated WebP; no frame is composed yet. `data` must outlive the player. NULL on failure. */
struct imganim *imganim_open(const unsigned char *data, int len);

/* Compose the next frame into a->rgba and set *delay_ms (10 ms or less becomes 100 ms). Returns 1 on a new frame, 0
 * once playback is over (a->rgba keeps the last frame): single-frame files and files that have played their loop count.
 */
int imganim_step(struct imganim *a, unsigned *delay_ms);

void imganim_close(struct imganim *a);

#endif /* OWOS_IMGANIM_H */
