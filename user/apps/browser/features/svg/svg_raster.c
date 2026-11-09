/* svg_raster.c: exact-coverage scanline rasterizer. */
#include "features/svg/svg_int.h"

/* compositing */
static void put_px(unsigned *d, unsigned rgb, unsigned a)
{
  if (!a)
    return;
  if (a >= 255) {
    *d = 0xFF000000u | rgb;
    return;
  }
  unsigned dv = *d, ia = 255 - a;
  unsigned na = a + (((dv >> 24) * ia + 127) / 255);
  unsigned nr = ((((rgb >> 16) & 255) * a + 127) / 255) + (((dv >> 16 & 255) * ia + 127) / 255);
  unsigned ng = ((((rgb >> 8) & 255) * a + 127) / 255) + (((dv >> 8 & 255) * ia + 127) / 255);
  unsigned nb = (((rgb & 255) * a + 127) / 255) + (((dv & 255) * ia + 127) / 255);
  if (na > 255)
    na = 255;
  *d = (na << 24) | ((nr > na ? na : nr) << 16) | ((ng > na ? na : ng) << 8) | (nb > na ? na : nb);
}

static bool ensure_xcap(struct ctx *cx, int n)
{
  if (n < cx->xcap)
    return true;
  int nc = cx->xcap ? cx->xcap * 2 : 256;
  float *nx = realloc(cx->xs, (unsigned)nc * sizeof *nx);
  if (nx)
    cx->xs = nx;
  int *nw = realloc(cx->wd, (unsigned)nc * sizeof *nw);
  if (nw)
    cx->wd = nw;
  if (!nx || !nw)
    return false;
  cx->xcap = nc;
  return true;
}

static void add_span(float *acc, float xa, float xb, float wgt)
{
  int ia = (int)xa, ib = (int)xb;
  if (ia == ib) {
    acc[ia] += (xb - xa) * wgt;
    return;
  }
  acc[ia] += ((float)(ia + 1) - xa) * wgt;
  for (int i = ia + 1; i < ib; i++)
    acc[i] += wgt;
  acc[ib] += (xb - (float)ib) * wgt;
}

/* Blend one pixel of coverage `a` (0..255) from source `s`. */
static void shade(struct ctx *cx, int idx, int x, int y, int a, const struct src *s)
{
  if (s->tex || s->grad) {
    unsigned c =
        s->tex ? svg_tex_px(s, (float)x + 0.5f, (float)y + 0.5f) : svg_grad_px(s, (float)x + 0.5f, (float)y + 0.5f);
    put_px(&cx->buf[idx], c & 0xFFFFFF, (unsigned)(a * (int)(c >> 24) / 255 * s->alpha / 255));
  } else {
    put_px(&cx->buf[idx], s->rgb, (unsigned)(a * s->alpha / 255));
  }
}

/* Blend a coverage bitmap (bw x bh at bx, by) through the active clip. */
void svg_blend_mask(struct ctx *cx, const unsigned char *cov, int bx, int by, int bw, int bh, const struct src *s)
{
  for (int y = by < 0 ? 0 : by; y < by + bh && y < cx->h; y++)
    for (int x = bx < 0 ? 0 : bx; x < bx + bw && x < cx->w; x++) {
      int a = cov[(y - by) * bw + (x - bx)];
      if (!a)
        continue;
      int idx = y * cx->w + x;
      if (cx->clip)
        a = a * cx->clip[idx] / 255;
      shade(cx, idx, x, y, a, s);
    }
}

void svg_composite_layer(const struct ctx *cx, unsigned *dst, const unsigned *layer, float opacity)
{
  unsigned o = (unsigned)(opacity * 255 + 0.5f);
  for (int i = 0; i < cx->w * cx->h; i++) {
    unsigned l = layer[i];
    if (!l)
      continue;
    unsigned a = ((l >> 24) * o + 127) / 255, r = (((l >> 16) & 255) * o + 127) / 255,
             g = (((l >> 8) & 255) * o + 127) / 255, b = ((l & 255) * o + 127) / 255;
    unsigned dv = dst[i], ia = 255 - a;
    unsigned na = a + (((dv >> 24) * ia + 127) / 255);
    unsigned nr = r + (((dv >> 16 & 255) * ia + 127) / 255), ng = g + (((dv >> 8 & 255) * ia + 127) / 255),
             nb = b + (((dv & 255) * ia + 127) / 255);
    if (na > 255)
      na = 255;
    dst[i] = (na << 24) | ((nr > na ? na : nr) << 16) | ((ng > na ? na : ng) << 8) | (nb > na ? na : nb);
  }
}

/* Fill device-space subpaths (implicitly closed). Writes into the canvas, or max-merges into `mout`. */
void svg_raster_fill(struct ctx *cx, const struct path *p, bool evenodd, const struct src *s, unsigned char *mout)
{
  if (p->n < 3 || !cx->acc)
    return;
  float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
  for (int i = 0; i < p->n; i++) {
    if (p->x[i] < minx)
      minx = p->x[i];
    if (p->x[i] > maxx)
      maxx = p->x[i];
    if (p->y[i] < miny)
      miny = p->y[i];
    if (p->y[i] > maxy)
      maxy = p->y[i];
  }
  if (!(minx - minx == 0 && maxx - maxx == 0 && miny - miny == 0 && maxy - maxy == 0)) /* inf or NaN */
    return;
  if (minx < 0)
    minx = 0;
  if (miny < 0)
    miny = 0;
  if (maxx > (float)cx->w)
    maxx = (float)cx->w;
  if (maxy > (float)cx->h)
    maxy = (float)cx->h;
  if (minx >= maxx || miny >= maxy)
    return;
  int x0 = (int)minx, x1 = (int)maxx + 1, y0 = (int)miny, y1 = (int)maxy + 1;
  if (x1 > cx->w)
    x1 = cx->w;
  if (y1 > cx->h)
    y1 = cx->h;
  float fx0 = (float)x0, fx1 = (float)x1;
  float *acc = cx->acc;
  const unsigned char *clip = mout ? 0 : cx->clip;

  for (int y = y0; y < y1; y++) {
    for (int x = x0; x <= x1; x++)
      acc[x] = 0;
    bool any = false;
    for (int k = 0; k < SUBS; k++) {
      float yc = (float)y + ((float)k + 0.5f) / SUBS;
      int nx = 0;
      for (int sb = 0; sb < p->ns; sb++) {
        int a = p->sub[sb], b = svg_sub_end(p, sb);
        if (b - a < 2)
          continue;
        for (int i = a; i < b; i++) {
          int j = i + 1 == b ? a : i + 1;
          float ya = p->y[i], yb = p->y[j];
          if ((ya <= yc) == (yb <= yc))
            continue;
          if (!ensure_xcap(cx, nx))
            break;
          cx->xs[nx] = p->x[i] + (yc - ya) / (yb - ya) * (p->x[j] - p->x[i]);
          cx->wd[nx] = yb > ya ? 1 : -1;
          nx++;
        }
      }
      float *xs = cx->xs;
      int *wd = cx->wd;
      for (int i = 1; i < nx; i++) {
        float kx = xs[i];
        int kw = wd[i], j = i - 1;
        while (j >= 0 && xs[j] > kx) {
          xs[j + 1] = xs[j];
          wd[j + 1] = wd[j];
          j--;
        }
        xs[j + 1] = kx;
        wd[j + 1] = kw;
      }
      int wind = 0;
      for (int i = 0; i + 1 < nx; i++) {
        wind += wd[i];
        if (!(evenodd ? (i & 1) == 0 : wind != 0))
          continue;
        float xa = xs[i] < fx0 ? fx0 : xs[i], xb = xs[i + 1] > fx1 ? fx1 : xs[i + 1];
        if (xb > xa) {
          add_span(acc, xa, xb, 1.0f / SUBS);
          any = true;
        }
      }
    }
    if (!any)
      continue;
    for (int x = x0; x < x1; x++) {
      int a = (int)(acc[x] * 255.0f + 0.5f);
      if (a <= 0)
        continue;
      if (a > 255)
        a = 255;
      int idx = y * cx->w + x;
      if (mout) {
        if (a > mout[idx])
          mout[idx] = (unsigned char)a;
        continue;
      }
      if (clip)
        a = a * clip[idx] / 255;
      shade(cx, idx, x, y, a, s);
    }
  }
}
