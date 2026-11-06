/* imgdec.c — shared image decoder (see imgdec.h). Dispatches by content to the
 * SVG rasterizer, the WebP decoder and the PNG decoder, always producing RGBA.
 */
#include "imgdec.h"
#include "owos.h"
#include "svg.h"
#include "upng.h"
#include "webp.h"

#define IMG_AREA_CAP (4096u * 4096u) /* reject absurd dimensions before allocating */

int image_sniff(const unsigned char *data, int len)
{
  if (!data || len < 4)
    return IMG_FMT_NONE;
  if (len >= 5 && svg_probe((const char *)data, len))
    return IMG_FMT_SVG;
  if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
      data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
    return IMG_FMT_WEBP;
  if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
    return IMG_FMT_PNG;
  return IMG_FMT_NONE;
}

/* streaming source over an in-memory buffer, for upng's pull API */
struct imgrd {
  const unsigned char *d;
  int len, pos;
};
static int imgrd_get(void *c)
{
  struct imgrd *r = c;
  return r->pos < r->len ? r->d[r->pos++] : -1;
}

struct imgfill {
  unsigned char *buf;
  unsigned iw, ih;
};
static int imgfill_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct imgfill *f = c;
  if (y < f->ih) {
    unsigned n = (w < f->iw ? w : f->iw) * 4;
    memcpy(f->buf + (unsigned long)y * f->iw * 4, rgba, n);
  }
  return 0;
}

static unsigned char *decode_svg(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *ow,
                                 unsigned *oh)
{
  int sw = 0, sh = 0;
  if (!svg_size((const char *)data, len, &sw, &sh) || sw <= 0 || sh <= 0) {
    sw = 128;
    sh = 128;
  }
  if (max_edge <= 0)
    max_edge = 512;
  int maxe = sw > sh ? sw : sh;
  if (maxe > max_edge) {
    sw = sw * max_edge / maxe;
    sh = sh * max_edge / maxe;
  }
  if (sw < 1)
    sw = 1;
  if (sh < 1)
    sh = 1;
  unsigned *argb = malloc((unsigned long)sw * sh * 4);
  if (!argb)
    return 0;
  if (!svg_render((const char *)data, len, sw, sh, argb, current_color)) {
    free(argb);
    return 0;
  }
  unsigned char *buf = malloc((unsigned long)sw * sh * 4);
  if (!buf) {
    free(argb);
    return 0;
  }
  for (int i = 0; i < sw * sh; i++) { /* ARGB int -> RGBA bytes */
    unsigned p = argb[i];
    buf[i * 4 + 0] = (p >> 16) & 0xff;
    buf[i * 4 + 1] = (p >> 8) & 0xff;
    buf[i * 4 + 2] = p & 0xff;
    buf[i * 4 + 3] = (p >> 24) & 0xff;
  }
  free(argb);
  *ow = (unsigned)sw;
  *oh = (unsigned)sh;
  return buf;
}

static unsigned char *decode_png(const unsigned char *data, int len, unsigned *ow, unsigned *oh)
{
  struct imgrd r = {data, len, 0};
  unsigned w = 0, h = 0;
  if (upng_dimensions(imgrd_get, &r, &w, &h) != UPNG_OK || !w || !h)
    return 0;
  if ((unsigned long)w * h > IMG_AREA_CAP)
    return 0;
  unsigned char *buf = malloc((unsigned long)w * h * 4);
  if (!buf)
    return 0;
  memset(buf, 0, (unsigned long)w * h * 4);
  r.pos = 0;
  struct imgfill f = {buf, w, h};
  if (upng_decode(imgrd_get, &r, imgfill_row, &f, 0, 0) != UPNG_OK) {
    free(buf);
    return 0;
  }
  *ow = w;
  *oh = h;
  return buf;
}

unsigned char *image_decode_rgba(const unsigned char *data, int len, unsigned current_color, int max_edge, unsigned *w,
                                 unsigned *h)
{
  switch (image_sniff(data, len)) {
  case IMG_FMT_SVG:
    return decode_svg(data, len, current_color, max_edge, w, h);
  case IMG_FMT_WEBP: {
    unsigned char *rgba = 0;
    unsigned ww = 0, hh = 0;
    if (webp_decode(data, len, &rgba, &ww, &hh) == WEBP_OK && rgba && ww && hh &&
        (unsigned long)ww * hh <= IMG_AREA_CAP) {
      *w = ww;
      *h = hh;
      return rgba;
    }
    free(rgba);
    return 0;
  }
  case IMG_FMT_PNG:
    return decode_png(data, len, w, h);
  default:
    return 0;
  }
}
