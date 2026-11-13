#include "imgdec.h"
#include "main_int.h"
#include "webp.h"

/* ------------------------------------------------------------- image draw */

/* PNG byte source: pulls straight out of the already-fetched body buffer, so the decoder streams */
struct rawrd {
  int pos;
};
static int rawrd_get(void *c)
{
  struct rawrd *r = c;
  if (r->pos >= B->pg->raw_len)
    return -1;
  return (unsigned char)B->pg->raw[r->pos++];
}

/* Source-column table for scaling a row of iw pixels to dw, shared by every blit (main thread */
static unsigned *g_xm;
static unsigned g_xm_cap, g_xm_dw, g_xm_iw;
const unsigned *xmap_get(unsigned dw, unsigned iw)
{
  if (!dw)
    return 0;
  if (dw > g_xm_cap) {
    unsigned *p = realloc(g_xm, (unsigned long)dw * sizeof *p);
    if (!p)
      return 0;
    g_xm = p;
    g_xm_cap = dw;
    g_xm_dw = 0;
  }
  if (g_xm_dw != dw || g_xm_iw != iw) {
    ru_xmap_fill(g_xm, dw, iw);
    g_xm_dw = dw;
    g_xm_iw = iw;
  }
  return g_xm;
}

/* Destination columns [lo,hi) of a dw-wide image drawn at x0 that land inside the surface. */
static bool clip_cols(int x0, unsigned dw, unsigned *lo, unsigned *hi)
{
  int a = x0 < 0 ? -x0 : 0, b = BW - x0 < (int)dw ? BW - x0 : (int)dw;
  if (b <= a)
    return false;
  *lo = (unsigned)a;
  *hi = (unsigned)b;
  return true;
}

/* Decode state: a fit-to-pane letterbox, blitting each PNG scanline directly into the window */
struct imgblit {
  int px, py;              /* pane top-left (window coords)            */
  unsigned iw, ih;         /* source image size                       */
  unsigned dw, dh, ox, oy; /* drawn size + offset within the pane      */
  unsigned next_r;         /* next dest row awaiting its source row    */
  const unsigned *xm;
};

static int img_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct imgblit *v = c;
  unsigned *fb = browser_surface();
  unsigned lo, hi;
  while (v->next_r < v->dh && (v->next_r * v->ih / v->dh) == y) {
    int dy = v->py + (int)v->oy + (int)v->next_r;
    int dx = v->px + (int)v->ox;
    if (dy >= 0 && dy < BH && clip_cols(dx, v->dw, &lo, &hi))
      ru_blit_row_rgba(fb + (unsigned long)dy * BW + dx, rgba, v->xm, v->dw, v->iw, lo, hi);
    v->next_r++;
  }
  return 0;
}

/* Row-copy sink: write each decoded PNG scanline into a full RGBA buffer (the full-page image */
struct imgc_fill {
  unsigned char *buf;
  unsigned iw, ih;
};
static int imgc_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct imgc_fill *d = c;
  if (y < d->ih) {
    unsigned n = (w < d->iw ? w : d->iw) * 4;
    memcpy(d->buf + (unsigned long)y * d->iw * 4, rgba, n);
  }
  return 0;
}

/* Decoded full-page image cache (MODE_IMAGE) */
#define DIMG_MAX_PX (16u * 1024 * 1024)
static struct {
  struct page *pg;
  const char *src;
  int src_len;
  unsigned iw, ih;
  unsigned char *rgba;
  struct imganim *anim; /* playing GIF / animated WebP reading from src, else NULL */
  unsigned next_ms;
} g_dimg;

static void dimg_reset(void)
{
  imganim_close(g_dimg.anim);
  g_dimg.anim = 0;
  free(g_dimg.rgba);
  g_dimg.rgba = 0;
  g_dimg.iw = g_dimg.ih = 0;
}

/* Decode a non-PNG page body into g_dimg.rgba (GIF and animated WebP keep their player for image_anim_tick) */
static void dimg_decode_other(void)
{
  unsigned iw = 0, ih = 0, delay = 0;
  const unsigned char *raw = (const unsigned char *)B->pg->raw;
  struct imganim *a = imganim_open(raw, B->pg->raw_len);
  if (a && (unsigned long)a->w * a->h <= DIMG_MAX_PX && imganim_step(a, &delay)) {
    g_dimg.rgba = malloc((unsigned long)a->w * a->h * 4);
    if (g_dimg.rgba) {
      memcpy(g_dimg.rgba, a->rgba, (unsigned long)a->w * a->h * 4);
      g_dimg.iw = a->w;
      g_dimg.ih = a->h;
      g_dimg.anim = a;
      g_dimg.next_ms = msec() + delay;
      return;
    }
  }
  imganim_close(a);
  unsigned char *rgba;
  if (webp_decode(raw, B->pg->raw_len, &rgba, &iw, &ih) == WEBP_OK) {
    if ((unsigned long)iw * ih <= DIMG_MAX_PX) {
      g_dimg.rgba = rgba;
      g_dimg.iw = iw;
      g_dimg.ih = ih;
    } else {
      free(rgba);
    }
  }
}

/* Advance a playing full-page GIF / animated WebP; repaints the page when the frame changes. */
void image_anim_tick(void)
{
  if (!g_dimg.anim || g_dimg.pg != B->pg || g_dimg.src != B->pg->raw || g_dimg.src_len != B->pg->raw_len ||
      (int)(msec() - g_dimg.next_ms) < 0)
    return;
  unsigned delay;
  if (!imganim_step(g_dimg.anim, &delay)) {
    imganim_close(g_dimg.anim);
    g_dimg.anim = 0;
    return;
  }
  memcpy(g_dimg.rgba, g_dimg.anim->rgba, (unsigned long)g_dimg.iw * g_dimg.ih * 4);
  g_dimg.next_ms = msec() + delay;
  B->seq++;
}

/* Streaming decode straight to the pane (no held copy) */
static void draw_image_stream(int px, int py, int pw, int ph)
{
  struct rawrd r = {0};
  unsigned iw = 0, ih = 0;
  if (png_dimensions(rawrd_get, &r, &iw, &ih) != 0 || !iw || !ih) {
    gfx_ui_text(px + 8, py + 8, "Cannot decode image (unreadable or oversized interlaced PNG).", 0xb04040);
    return;
  }
  struct imgblit v;
  v.px = px;
  v.py = py;
  v.iw = iw;
  v.ih = ih;
  v.next_r = 0;
  if (iw <= (unsigned)pw && ih <= (unsigned)ph) {
    v.dw = iw;
    v.dh = ih; /* fits: show 1:1     */
  } else if ((unsigned long)iw * ph > (unsigned long)ih * pw) {
    v.dw = pw;
    v.dh = ih * pw / iw; /* width-bound        */
  } else {
    v.dh = ph;
    v.dw = iw * ph / ih; /* height-bound       */
  }
  if (!v.dw)
    v.dw = 1;
  if (!v.dh)
    v.dh = 1;
  v.ox = ((unsigned)pw - v.dw) / 2;
  v.oy = ((unsigned)ph - v.dh) / 2;
  v.xm = xmap_get(v.dw, iw);

  struct rawrd r2 = {0};
  png_decode(rawrd_get, &r2, img_row, &v, 0, 0);
}

/* Draw B->pg->raw as a PNG into the content pane (px,py,pw,ph), fit-to-pane with aspect preserved */
void draw_image(int px, int py, int pw, int ph, unsigned bg)
{
  (void)bg;
  if (g_dimg.pg != B->pg || g_dimg.src != B->pg->raw || g_dimg.src_len != B->pg->raw_len) {
    dimg_reset();
    g_dimg.pg = B->pg;
    g_dimg.src = B->pg->raw;
    g_dimg.src_len = B->pg->raw_len;
    struct rawrd r = {0};
    unsigned iw = 0, ih = 0;
    if (image_sniff((const unsigned char *)B->pg->raw, B->pg->raw_len) != IMG_FMT_PNG) {
      dimg_decode_other();
    } else if (png_dimensions(rawrd_get, &r, &iw, &ih) == 0 && iw && ih && (unsigned long)iw * ih <= DIMG_MAX_PX) {
      unsigned char *buf = malloc((unsigned long)iw * ih * 4);
      if (buf) {
        memset(buf, 0, (unsigned long)iw * ih * 4);
        struct rawrd r2 = {0};
        struct imgc_fill f = {buf, iw, ih};
        if (png_decode(rawrd_get, &r2, imgc_row, &f, 0, 0) == 0) {
          g_dimg.rgba = buf;
          g_dimg.iw = iw;
          g_dimg.ih = ih;
        } else
          free(buf);
      }
    }
  }
  if (!g_dimg.rgba) {
    if (image_sniff((const unsigned char *)B->pg->raw, B->pg->raw_len) == IMG_FMT_PNG)
      draw_image_stream(px, py, pw, ph);
    else
      gfx_ui_text(px + 8, py + 8, "Cannot decode image.", 0xb04040);
    return;
  } /* too big / unreadable */

  unsigned iw = g_dimg.iw, ih = g_dimg.ih, dw, dh;
  if (iw <= (unsigned)pw && ih <= (unsigned)ph) {
    dw = iw;
    dh = ih; /* fits: show 1:1     */
  } else if ((unsigned long)iw * ph > (unsigned long)ih * pw) {
    dw = pw;
    dh = ih * pw / iw; /* width-bound        */
  } else {
    dh = ph;
    dw = iw * ph / ih; /* height-bound       */
  }
  if (!dw)
    dw = 1;
  if (!dh)
    dh = 1;
  unsigned ox = ((unsigned)pw - dw) / 2, oy = ((unsigned)ph - dh) / 2;
  unsigned *fb = browser_surface();
  const unsigned *xm = xmap_get(dw, iw);
  unsigned lo, hi;
  if (!clip_cols(px + (int)ox, dw, &lo, &hi))
    return;
  for (unsigned ry = 0; ry < dh; ry++) {
    int dy = py + (int)oy + (int)ry;
    if (dy < 0 || dy >= BH)
      continue;
    const unsigned char *srow = g_dimg.rgba + (unsigned long)(ry * ih / dh) * iw * 4;
    ru_blit_row_rgba(fb + (unsigned long)dy * BW + px + ox, srow, xm, dw, iw, lo, hi);
  }
}

/* ------------------------------------------------------- inline page images */

/* Byte reader over an inline image's PNG bytes in img_pool. */
struct poolrd {
  int pos, off, len;
};
static int poolrd_get(void *c)
{
  struct poolrd *r = c;
  return r->pos < r->len ? B->img_pool[r->off + r->pos++] : -1;
}

/* Blit one inline image left-aligned at (x0,y0), width dw, height dh */
struct inlblit {
  int x0, y0, dw, dh, clip_top, clip_bot;
  unsigned iw, ih, next_r;
  const unsigned *xm;
  unsigned lo, hi;
};

static int inl_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct inlblit *v = c;
  unsigned *fb = browser_surface();
  while (v->next_r < (unsigned)v->dh && (v->next_r * v->ih / (unsigned)v->dh) == y) {
    int dy = v->y0 + (int)v->next_r;
    if (dy >= v->clip_top && dy < v->clip_bot)
      ru_blit_row_rgba(fb + (unsigned long)dy * BW + v->x0, rgba, v->xm, (unsigned)v->dw, v->iw, v->lo, v->hi);
    v->next_r++;
  }
  return 0;
}

/* Decoded-PNG cache */
#define IMGC_BUDGET (16ul << 20)
#define IMGC_NATURAL_MAX_PX (1024u * 1024u) /* cache at natural size up to this */
static struct ru_imgcache g_imgc = {.budget = IMGC_BUDGET};
static struct page *g_imgc_pg;

static int imgc_scale_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct ru_scaler *sc = c;
  if (w >= sc->iw)
    ru_scaler_row(sc, y, rgba);
  return 0;
}

void imgc_clear(void)
{
  ru_imgcache_clear(&g_imgc);
  dimg_reset();
  g_dimg.pg = 0;
  g_dimg.src = 0;
  g_dimg.src_len = 0;
}

/* Decoded pixels for inline PNG `im` shown dw x dh (decoding + caching on first use) */
static const struct ru_img *imgc_get(struct inl_img *im, unsigned dw, unsigned dh)
{
  if (g_imgc_pg != B->pg) {
    imgc_clear();
    g_imgc_pg = B->pg;
  }
  if (!im->uid || !im->iw || !im->ih)
    return 0;
  const struct ru_img *hit = ru_imgcache_find(&g_imgc, im->uid, im->iw, im->ih, dw, dh);
  if (hit)
    return hit;
  bool natural = (unsigned long)im->iw * im->ih <= IMGC_NATURAL_MAX_PX;
  unsigned cw = natural ? im->iw : dw, ch = natural ? im->ih : dh;
  if (!cw || !ch || cw > IMGC_BUDGET / 4 / ch)
    return 0;
  unsigned long bytes = (unsigned long)cw * ch * 4;
  unsigned *px = calloc(1, bytes);
  if (!px)
    return 0;
  struct ru_scaler sc = {px, xmap_get(cw, im->iw), im->iw, im->ih, cw, ch, 0, true};
  struct poolrd r = {0, im->coff, im->clen};
  if (png_decode(poolrd_get, &r, imgc_scale_row, &sc, 0, 0) != 0) {
    free(px);
    return 0;
  }
  bool opaque = sc.opaque && sc.next_r == ch;
  if (opaque)
    ru_words_strip_alpha(px, (unsigned long)cw * ch);
  struct ru_img e = {im->uid, im->iw, im->ih, cw, ch, px, bytes, 0, opaque};
  if (!ru_imgcache_put(&g_imgc, &e)) {
    free(px);
    return 0;
  }
  return ru_imgcache_find(&g_imgc, im->uid, im->iw, im->ih, dw, dh);
}

/* The plain blit of the fitted image (fw x fh at ox,oy in a dw x dh box at x0,y0) into rows [ry0,ry1), columns [lo,hi).
 */
static bool blit_image(struct inl_img *im, unsigned *fb, int stride, int x0, int y0, int dw, int dh, int fw, int fh,
                       int ox, int oy, int ry0, int ry1, unsigned lo, unsigned hi, int ct, int cb)
{
  /* SVGs and animated frames live raw in img_pool */
  if (im->is_argb) {
    if (im->uid)
      animimg_seen(im->uid);
    const unsigned char *src = B->img_pool + im->coff;
    const unsigned *xm = xmap_get((unsigned)fw, im->iw);
    for (int ry = ry0; ry < ry1; ry++) {
      const unsigned char *srow = src + (unsigned long)((unsigned)(ry - oy) * im->ih / (unsigned)fh) * im->iw * 4;
      ru_blit_row_rgba(fb + (long)(y0 + ry) * stride + x0 + ox, srow, xm, (unsigned)fw, im->iw, lo, hi);
    }
    return true;
  }
  const struct ru_img *c = imgc_get(im, (unsigned)fw, (unsigned)fh);
  if (c) {
    const unsigned *xm = c->cw == (unsigned)fw ? 0 : xmap_get((unsigned)fw, c->cw);
    for (int ry = ry0; ry < ry1; ry++) {
      const unsigned *srow = c->px + (unsigned long)((unsigned)(ry - oy) * c->ch / (unsigned)fh) * c->cw;
      ru_blit_row_words(fb + (long)(y0 + ry) * stride + x0 + ox, srow, xm, (unsigned)fw, c->cw, c->opaque, lo, hi);
    }
    return true;
  }
  /* Fallback (too large to hold / decode failed): stream it once to the window, filling the box. */
  if (fb != browser_surface())
    return false;
  unsigned flo, fhi;
  if (!clip_cols(x0, (unsigned)dw, &flo, &fhi))
    return true;
  struct inlblit v = {x0, y0, dw, dh, ct, cb, im->iw, im->ih, 0, xmap_get((unsigned)dw, im->iw), flo, fhi};
  struct poolrd r = {0, im->coff, im->clen};
  png_decode(poolrd_get, &r, inl_row, &v, 0, 0);
  return true;
}

/* An image under a transform: painted over black and over white to recover each pixel's alpha, then drawn through the
 * map. False when the image cannot be painted off-screen, so the caller draws it plainly. */
static bool draw_transformed(struct inl_img *im, int x, int y, int fw, int fh, int ct, int cb)
{
  unsigned long n = (unsigned long)fw * (unsigned long)fh;
  unsigned *over_black = malloc(n * 4 * 2);
  if (!over_black)
    return false;
  unsigned *over_white = over_black + n;
  for (unsigned long i = 0; i < n; i++) {
    over_black[i] = 0;
    over_white[i] = 0xffffff;
  }
  if (!blit_image(im, over_black, fw, 0, 0, fw, fh, fw, fh, 0, 0, 0, fh, 0, (unsigned)fw, 0, fh) ||
      !blit_image(im, over_white, fw, 0, 0, fw, fh, fw, fh, 0, 0, 0, fh, 0, (unsigned)fw, 0, fh)) {
    free(over_black);
    return false;
  }
  struct lext fe;
  memset(&fe, 0, sizeof fe);
  fe.flt_bright = im->fbright;
  fe.flt_gray = im->fgray;
  fe.flt_invert = im->finvert;
  bool filt = im->fbright != 1000 || im->fgray || im->finvert;
  for (unsigned long i = 0; i < n; i++) { /* black: c*a, white: c*a + 255*(1-a) */
    unsigned b = over_black[i], w = over_white[i], d = 0;
    for (int sh = 0; sh < 24; sh += 8) {
      unsigned dc = ((w >> sh) & 0xff) - ((b >> sh) & 0xff);
      d = dc > d ? dc : d;
    }
    unsigned a = 255 - d, c = 0;
    if (a) {
      for (int sh = 0; sh < 24; sh += 8) {
        unsigned v = ((b >> sh) & 0xff) * 255 / a;
        c |= (v > 255 ? 255 : v) << sh;
      }
      if (filt)
        c = ru_filter_rgb(&fe, c);
    }
    over_black[i] = (a << 24) | c;
  }
  struct ru_xf xf = {im->xf.m[0] / 4096.0,
                     im->xf.m[1] / 4096.0,
                     im->xf.m[2] / 4096.0,
                     im->xf.m[3] / 4096.0,
                     x + im->xf.ox + im->xf.oxp * fw / 100.0,
                     y + im->xf.oy + im->xf.oyp * fh / 100.0,
                     im->xf.tx + im->xf.txp * fw / 100.0,
                     im->xf.ty + im->xf.typ * fh / 100.0};
  ru_affine_draw_argb(browser_surface(), BW, 0, ct, BW, cb, over_black, fw, fh, x, y, &xf);
  free(over_black);
  return true;
}

/* Coverage (0..255) of pixel (x,y) by a w*h box with corners rounded to radius r */
static unsigned rrect_cov(int x, int y, int w, int h, int r)
{
  if (x < 0 || y < 0 || x >= w || y >= h)
    return 0;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r <= 0)
    return 255;
  int cx = 16 * x + 8, cy = 16 * y + 8, dx = 0, dy = 0; /* pixel centre, 1/16 px */
  if (cx < 16 * r)
    dx = 16 * r - cx;
  else if (cx > 16 * (w - r))
    dx = cx - 16 * (w - r);
  if (cy < 16 * r)
    dy = 16 * r - cy;
  else if (cy > 16 * (h - r))
    dy = cy - 16 * (h - r);
  if (!dx || !dy)
    return 255;
  int over = (int)ru_isqrt((unsigned)(dx * dx + dy * dy)) - 16 * r + 8; /* 1/16 px past the arc */
  return over <= 0 ? 255 : over >= 16 ? 0 : (unsigned)(255 - over * 16);
}

static unsigned mix_rgb(unsigned a, unsigned b, unsigned t) /* a..b by t/255 */
{
  unsigned it = 255 - t;
  return ((((a >> 16) & 0xff) * it + ((b >> 16) & 0xff) * t) / 255) << 16 |
         ((((a >> 8) & 0xff) * it + ((b >> 8) & 0xff) * t) / 255) << 8 | (((a & 0xff) * it + (b & 0xff) * t) / 255);
}

/* Round the corners of the image just drawn over `bg` (the pixels saved before it) and ring it with its border:
 * the border box is bw x by, w*h, its rows [r0,r1) on screen. */
static void frame_image(const struct inl_img *im, const unsigned *bg, int bx, int by, int w, int h, int r0, int r1)
{
  unsigned *fb = browser_surface();
  int b = im->bw, r = im->radius;
  for (int y = r0; y < r1; y++)
    for (int x = 0; x < w; x++) {
      int sx = bx + x;
      if (sx < 0 || sx >= BW)
        continue;
      unsigned *p = fb + (long)(by + y) * BW + sx;
      unsigned under = bg[(long)(y - r0) * w + x];
      unsigned co = rrect_cov(x, y, w, h, r), ci = b ? rrect_cov(x - b, y - b, w - 2 * b, h - 2 * b, r - b) : co;
      unsigned ring = b ? mix_rgb(under, im->bcol, co) : under;
      *p = mix_rgb(ring, *p, ci);
    }
}

void draw_inline_image(struct inl_img *im, int py, int ct, int cb, int paw)
{
  if (im->hidden)
    return;
  /* Draw with the exact geometry the row was reserved with (line_x/line_w), not the full pane */
  const struct ltab *t = &g_fr->lt;
  int row = im->start_row;
  if (row < 0 || row >= t->n)
    return;
  int x0 = im->lx >= 0 ? im->lx : t->x[row];
  int rw = im->lx >= 0 ? im->lw : t->w[row];
  if (rw < 1)
    rw = paw < 1 ? 1 : paw;
  int bdw = 0, bdh = 0;
  img_disp_size(im, rw, &bdw, &bdh);
  if (rw > bdw)
    x0 += im->halign == 1 ? (rw - bdw) / 2 : im->halign == 2 ? rw - bdw : 0;
  draw_image_at(im, x0, py + t->y[row] - B->pg->scroll, bdw, bdh, ct, cb);
}

/* Draw image `im` with its border box (bdw x bdh) at screen (bx,by), clipped to the rows [ct,cb). */
void draw_image_at(struct inl_img *im, int bx, int by, int bdw, int bdh, int ct, int cb)
{
  if (im->hidden)
    return;
  int bw = im->bw > 0 ? im->bw : 0; /* the picture sits bw inside the border box */
  int dw = bdw - 2 * bw, dh = bdh - 2 * bw;
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  int x0 = bx + bw, y0 = by + bw;
  /* Rounded or bordered: keep what lies under the border box, so the corners can show it again. */
  unsigned *under = 0;
  int fr0 = 0, fr1 = 0, fbx = x0 - bw, fby = y0 - bw;
  if ((im->radius > 0 || bw > 0) && !im->xf.on) {
    fr0 = fby < ct ? ct - fby : 0;
    fr1 = fby + bdh > cb ? cb - fby : bdh;
    if (fby + fr0 < 0)
      fr0 = -fby;
    if (fby + fr1 > BH)
      fr1 = BH - fby;
    if (fr1 > fr0 && (under = malloc((unsigned long)bdw * (unsigned)(fr1 - fr0) * 4)))
      for (int y = fr0; y < fr1; y++)
        for (int x = 0; x < bdw; x++) {
          int sx = fbx + x;
          under[(long)(y - fr0) * bdw + x] = sx >= 0 && sx < BW ? browser_surface()[(long)(fby + y) * BW + sx] : 0;
        }
  }
  int ry0 = y0 < ct ? ct - y0 : 0, ry1 = y0 + dh > cb ? cb - y0 : dh;
  if (ry1 <= ry0) {
    if (under)
      frame_image(im, under, fbx, fby, bdw, bdh, fr0, fr1);
    free(under);
    return;
  }
  int fw = dw, fh = dh, ox = 0, oy = 0;
  if (im->fit && im->iw && im->ih)
    ru_fit_rect(im->fit, im->iw, im->ih, dw, dh, &fw, &fh, &ox, &oy);
  /* columns of the fitted image that land inside the box and the window */
  int lo_i = -ox > -(x0 + ox) ? -ox : -(x0 + ox), hi_i = dw - ox < BW - (x0 + ox) ? dw - ox : BW - (x0 + ox);
  unsigned lo = (unsigned)(lo_i < 0 ? 0 : lo_i), hi = (unsigned)(hi_i > fw ? fw : hi_i);
  if (hi_i <= lo_i || hi <= lo) {
    if (under)
      frame_image(im, under, fbx, fby, bdw, bdh, fr0, fr1);
    free(under);
    return;
  }
  unsigned *fb = browser_surface();
  if (ry0 < oy)
    ry0 = oy;
  if (ry1 > oy + fh)
    ry1 = oy + fh;

  if (im->xf.on && draw_transformed(im, x0 + ox, y0 + oy, fw, fh, ct, cb)) {
    free(under);
    return;
  }
  bool filt = im->fbright != 1000 || im->fgray || im->finvert;
  int vw = (int)(hi - lo), vh = ry1 - ry0;
  unsigned *base = fb + (long)(y0 + ry0) * BW + x0 + ox + lo;
  unsigned *save = filt ? malloc((unsigned long)vw * (unsigned long)vh * 4) : 0;
  if (save)
    for (int r = 0; r < vh; r++)
      memcpy(save + (long)r * vw, base + (long)r * BW, (unsigned long)vw * 4);
  blit_image(im, fb, BW, x0, y0, dw, dh, fw, fh, ox, oy, ry0, ry1, lo, hi, ct, cb);
  if (save) { /* pixels the blit changed are the image's; filter those and leave the backdrop alone */
    struct lext fe;
    memset(&fe, 0, sizeof fe);
    fe.flt_bright = im->fbright;
    fe.flt_gray = im->fgray;
    fe.flt_invert = im->finvert;
    for (int r = 0; r < vh; r++)
      for (int c = 0; c < vw; c++) {
        unsigned *p = base + (long)r * BW + c;
        if (*p != save[(long)r * vw + c])
          *p = ru_filter_rgb(&fe, *p);
      }
    free(save);
  }
  if (under)
    frame_image(im, under, fbx, fby, bdw, bdh, fr0, fr1);
  free(under);
}

/* A placeholder box for an image that couldn't be fetched/decoded. */
void draw_alt_box(struct inl_img *im, int py, int ct, int cb)
{
  if (im->hidden)
    return;
  int y0 = py + g_fr->lt.y[im->start_row] - B->pg->scroll;
  if (y0 + LINE_H <= ct || y0 >= cb)
    return; /* keep it in-pane    */
  const char *t = im->alt[0] ? im->alt : "[image]";
  int w = gfx_ui_width(t) + 8;
  gfx_fill_rect(4, y0, w, LINE_H, 0xe6e9ef);
  gfx_rect(4, y0, w, LINE_H, 0xb4bac4);
  gfx_ui_text(8, y0 + 1, t, 0x7a828c);
}
