/* imganim.c - see imganim.h. */
#include "imganim.h"
#include "gif.h"
#include "owos.h"
#include "webp.h"

/* Browsers clamp tiny GIF/WebP delays (0 or 10 ms) to 100 ms. */
#define MIN_FAST_DELAY_MS 10
#define CLAMPED_DELAY_MS 100

int imganim_sniff(const unsigned char *data, int len)
{
  if (len >= 6 && !memcmp(data, "GIF8", 4) && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
    return IMGANIM_GIF;
  struct webp_info info;
  if (webp_probe(data, len, &info) == WEBP_OK && info.animation)
    return IMGANIM_WEBP;
  return IMGANIM_NONE;
}

struct imganim *imganim_open(const unsigned char *data, int len)
{
  int fmt = imganim_sniff(data, len);
  if (fmt == IMGANIM_NONE)
    return 0;
  struct imganim *a = calloc(1, sizeof *a);
  if (!a)
    return 0;
  a->fmt = fmt;
  int rc;
  if (fmt == IMGANIM_GIF) {
    struct gif *g;
    rc = gif_open(&g, data, len, &a->w, &a->h);
    a->dec = g;
    a->loops_left = rc == GIF_OK ? gif_loops(g) : 0;
  } else {
    struct webp_anim *w;
    rc = webp_anim_open(&w, data, len, &a->w, &a->h);
    a->dec = w;
    a->loops_left = rc == WEBP_OK ? webp_anim_loops(w) : 0;
  }
  if (rc != 0 || !(a->rgba = calloc(a->w * a->h, 4))) {
    imganim_close(a);
    return 0;
  }
  return a;
}

static int decoder_next(struct imganim *a, int *delay)
{
  return a->fmt == IMGANIM_GIF ? gif_next(a->dec, a->rgba, delay) : webp_anim_next(a->dec, a->rgba, delay);
}

static void decoder_rewind(struct imganim *a)
{
  if (a->fmt == IMGANIM_GIF)
    gif_rewind(a->dec, a->rgba);
  else
    webp_anim_rewind(a->dec, a->rgba);
}

int imganim_step(struct imganim *a, unsigned *delay_ms)
{
  int delay = 0, rc = decoder_next(a, &delay);
  if (rc != 1) {
    /* End of a pass: a file with a single frame is a still; otherwise loop while plays remain (0 = forever). */
    if (a->pass_frames < 2 || (a->loops_left && --a->loops_left == 0))
      return 0;
    decoder_rewind(a);
    a->pass_frames = 0;
    rc = decoder_next(a, &delay);
    if (rc != 1)
      return 0;
  }
  a->pass_frames++;
  *delay_ms = delay <= MIN_FAST_DELAY_MS ? CLAMPED_DELAY_MS : (unsigned)delay;
  return 1;
}

void imganim_close(struct imganim *a)
{
  if (!a)
    return;
  if (a->dec) {
    if (a->fmt == IMGANIM_GIF)
      gif_close(a->dec);
    else
      webp_anim_close(a->dec);
  }
  free(a->rgba);
  free(a);
}
