#include "main_int.h"

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
} g_dimg;

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
    free(g_dimg.rgba);
    g_dimg.rgba = 0;
    g_dimg.iw = g_dimg.ih = 0;
    g_dimg.pg = B->pg;
    g_dimg.src = B->pg->raw;
    g_dimg.src_len = B->pg->raw_len;
    struct rawrd r = {0};
    unsigned iw = 0, ih = 0;
    if (png_dimensions(rawrd_get, &r, &iw, &ih) == 0 && iw && ih && (unsigned long)iw * ih <= DIMG_MAX_PX) {
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
    draw_image_stream(px, py, pw, ph);
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
  free(g_dimg.rgba);
  g_dimg.rgba = 0;
  g_dimg.pg = 0;
  g_dimg.src = 0;
  g_dimg.src_len = 0;
  g_dimg.iw = g_dimg.ih = 0;
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

void draw_inline_image(struct inl_img *im, int py, int ct, int cb, int paw)
{
  /* Draw with the exact geometry the row was reserved with (line_x/line_w), not the full pane */
  const struct ltab *t = &g_fr->lt;
  int row = im->start_row;
  bool recorded = (row >= 0 && row < t->n);
  int x0 = recorded ? t->x[row] : 4;
  int rw = recorded ? t->w[row] : (paw < 1 ? 1 : paw);
  int dw = 0, dh = 0;
  img_disp_size(im, rw, &dw, &dh);
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  if (!recorded)
    return;
  int y0 = py + t->y[row] - B->pg->scroll;
  int ry0 = y0 < ct ? ct - y0 : 0, ry1 = y0 + dh > cb ? cb - y0 : dh;
  unsigned lo, hi;
  if (ry1 <= ry0 || !clip_cols(x0, (unsigned)dw, &lo, &hi))
    return;
  unsigned *fb = browser_surface();

  /* SVGs live raw in img_pool */
  if (im->is_argb) {
    const unsigned char *src = B->img_pool + im->coff;
    const unsigned *xm = xmap_get((unsigned)dw, im->iw);
    for (int ry = ry0; ry < ry1; ry++) {
      const unsigned char *srow = src + (unsigned long)((unsigned)ry * im->ih / (unsigned)dh) * im->iw * 4;
      ru_blit_row_rgba(fb + (unsigned long)(y0 + ry) * BW + x0, srow, xm, (unsigned)dw, im->iw, lo, hi);
    }
    return;
  }
  const struct ru_img *c = imgc_get(im, (unsigned)dw, (unsigned)dh);
  if (c) {
    const unsigned *xm = c->cw == (unsigned)dw ? 0 : xmap_get((unsigned)dw, c->cw);
    for (int ry = ry0; ry < ry1; ry++) {
      const unsigned *srow = c->px + (unsigned long)((unsigned)ry * c->ch / (unsigned)dh) * c->cw;
      ru_blit_row_words(fb + (unsigned long)(y0 + ry) * BW + x0, srow, xm, (unsigned)dw, c->cw, c->opaque, lo, hi);
    }
    return;
  }
  /* Fallback (too large to hold / decode failed): stream it once. */
  struct inlblit v = {x0, y0, dw, dh, ct, cb, im->iw, im->ih, 0, xmap_get((unsigned)dw, im->iw), lo, hi};
  struct poolrd r = {0, im->coff, im->clen};
  png_decode(poolrd_get, &r, inl_row, &v, 0, 0);
}

/* A placeholder box for an image that couldn't be fetched/decoded. */
void draw_alt_box(struct inl_img *im, int py, int ct, int cb)
{
  int y0 = py + g_fr->lt.y[im->start_row] - B->pg->scroll;
  if (y0 + LINE_H <= ct || y0 >= cb)
    return; /* keep it in-pane    */
  const char *t = im->alt[0] ? im->alt : "[image]";
  int w = gfx_ui_width(t) + 8;
  gfx_fill_rect(4, y0, w, LINE_H, 0xe6e9ef);
  gfx_rect(4, y0, w, LINE_H, 0xb4bac4);
  gfx_ui_text(8, y0 + 1, t, 0x7a828c);
}
