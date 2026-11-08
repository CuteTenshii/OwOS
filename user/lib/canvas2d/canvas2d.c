/* canvas2d.c - surface, state stack, paints, fills, strokes, clipping, images and PNG export. */
#include "c2int.h"
#include "crc32.h"
#include "math.h"
#include "owos.h"

#define MAX_AREA (4 * 1024 * 1024)
#define MAX_DEPTH 512

/* -------------------------------------------------------------------- paints */

static void paint_unref(struct c2d_paint *p)
{
  c2d_gradient_unref(p->grad);
  c2d_pattern_unref(p->pat);
  p->grad = 0;
  p->pat = 0;
}

static void paint_ref(const struct c2d_paint *p)
{
  if (p->grad)
    p->grad->refs++;
  if (p->pat)
    p->pat->refs++;
}

void c2d_set_paint_color(struct c2d_paint *p, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
  paint_unref(p);
  p->kind = C2D_COLOR;
  p->r = r;
  p->g = g;
  p->b = b;
  p->a = a;
}

void c2d_set_paint_gradient(struct c2d_paint *p, struct c2d_gradient *g)
{
  g->refs++;
  paint_unref(p);
  p->kind = g->kind;
  p->grad = g;
}

void c2d_set_paint_pattern(struct c2d_paint *p, struct c2d_pattern *pat)
{
  pat->refs++;
  paint_unref(p);
  p->kind = C2D_PATTERN;
  p->pat = pat;
}

struct c2d_gradient *c2d_gradient_new(int kind, float x0, float y0, float r0, float x1, float y1, float r1)
{
  struct c2d_gradient *g = calloc(1, sizeof *g);
  if (!g)
    return 0;
  g->refs = 1;
  g->kind = kind;
  g->x0 = x0;
  g->y0 = y0;
  g->r0 = r0;
  g->x1 = x1;
  g->y1 = y1;
  g->r1 = r1;
  return g;
}

bool c2d_gradient_add_stop(struct c2d_gradient *g, float off, unsigned char r, unsigned char gr, unsigned char b,
                           unsigned char a)
{
  if (!c2d_grow((void **)&g->stop, &g->cap, g->nstop + 1, sizeof *g->stop))
    return false;
  int i = g->nstop++;
  for (; i > 0 && g->stop[i - 1].off > off; i--)
    g->stop[i] = g->stop[i - 1];
  g->stop[i] = (struct c2d_stop){off, r, gr, b, a};
  return true;
}

void c2d_gradient_unref(struct c2d_gradient *g)
{
  if (!g || --g->refs > 0)
    return;
  free(g->stop);
  free(g);
}

struct c2d_pattern *c2d_pattern_new(const unsigned char *rgba, int w, int h, bool rx, bool ry)
{
  if (w <= 0 || h <= 0 || (unsigned long)w * h > MAX_AREA)
    return 0;
  struct c2d_pattern *p = calloc(1, sizeof *p);
  if (!p)
    return 0;
  p->rgba = malloc((unsigned long)w * h * 4);
  if (!p->rgba) {
    free(p);
    return 0;
  }
  memcpy(p->rgba, rgba, (unsigned long)w * h * 4);
  p->refs = 1;
  p->w = w;
  p->h = h;
  p->repeat_x = rx;
  p->repeat_y = ry;
  return p;
}

void c2d_pattern_unref(struct c2d_pattern *p)
{
  if (!p || --p->refs > 0)
    return;
  free(p->rgba);
  free(p);
}

/* --------------------------------------------------------------------- state */

static void mask_unref(struct c2d_mask *m)
{
  if (m && --m->refs <= 0)
    free(m);
}

static void state_init(struct c2d_state *s)
{
  memset(s, 0, sizeof *s);
  s->m = C2D_IDENTITY;
  s->fill.kind = s->stroke.kind = C2D_COLOR;
  s->fill.a = s->stroke.a = 255;
  s->line_width = 1;
  s->miter_limit = 10;
  s->alpha = 1;
  s->comp = C2D_SOURCE_OVER;
  s->font_px = 10;
  s->smoothing = true;
}

static void state_free(struct c2d_state *s)
{
  paint_unref(&s->fill);
  paint_unref(&s->stroke);
  free(s->dash);
  mask_unref(s->clip);
}

static bool state_dup(struct c2d_state *dst, const struct c2d_state *src)
{
  *dst = *src;
  paint_ref(&dst->fill);
  paint_ref(&dst->stroke);
  if (src->clip)
    src->clip->refs++;
  dst->dash = 0;
  if (src->ndash) {
    dst->dash = malloc((unsigned)src->ndash * sizeof *dst->dash);
    if (!dst->dash) {
      dst->ndash = 0;
      return false;
    }
    memcpy(dst->dash, src->dash, (unsigned)src->ndash * sizeof *dst->dash);
  }
  return true;
}

void c2d_reset_state(struct c2d *c)
{
  while (c->depth >= 0)
    state_free(&c->st[c->depth--]);
  c->depth = 0;
  state_init(&c->st[0]);
  c2d_path_clear(&c->path);
}

struct c2d *c2d_new(int w, int h)
{
  if (w <= 0 || h <= 0 || (unsigned long)w * (unsigned long)h > MAX_AREA)
    return 0;
  struct c2d *c = calloc(1, sizeof *c);
  if (!c)
    return 0;
  c->px = calloc((unsigned)w * (unsigned)h, 4);
  c->st = calloc(1, sizeof *c->st);
  c->stcap = 1;
  if (!c->px || !c->st) {
    free(c->px);
    free(c->st);
    free(c);
    return 0;
  }
  c->w = w;
  c->h = h;
  state_init(&c->st[0]);
  return c;
}

void c2d_free(struct c2d *c)
{
  if (!c)
    return;
  while (c->depth >= 0)
    state_free(&c->st[c->depth--]);
  c2d_path_free(&c->path);
  free(c->st);
  free(c->px);
  free(c);
}

bool c2d_resize(struct c2d *c, int w, int h)
{
  if (w <= 0 || h <= 0 || (unsigned long)w * (unsigned long)h > MAX_AREA)
    return false;
  unsigned char *np = calloc((unsigned)w * (unsigned)h, 4);
  if (!np)
    return false;
  free(c->px);
  c->px = np;
  c->w = w;
  c->h = h;
  c2d_reset_state(c);
  return true;
}

void c2d_save(struct c2d *c)
{
  if (c->depth + 1 >= MAX_DEPTH)
    return;
  if (c->depth + 2 > c->stcap && !c2d_grow((void **)&c->st, &c->stcap, c->depth + 2, sizeof *c->st))
    return;
  state_dup(&c->st[c->depth + 1], &c->st[c->depth]);
  c->depth++;
}

void c2d_restore(struct c2d *c)
{
  if (c->depth == 0)
    return;
  state_free(&c->st[c->depth]);
  c->depth--;
}

bool c2d_set_dash(struct c2d *c, const float *d, int n)
{
  struct c2d_state *s = &c->st[c->depth];
  float *nd = 0;
  if (n > 0) {
    nd = malloc((unsigned)n * sizeof *nd);
    if (!nd)
      return false;
    memcpy(nd, d, (unsigned)n * sizeof *nd);
  }
  free(s->dash);
  s->dash = nd;
  s->ndash = n;
  return true;
}

/* ------------------------------------------------------------------- drawing */

static float mat_scale(const struct c2d_mat *m) { return (float)sqrt(fabs((double)m->a * m->d - (double)m->b * m->c)); }

static void fill_outline(struct c2d *c, const struct c2d_path *p, bool evenodd, const struct c2d_paint *paint)
{
  struct c2d_state *st = &c->st[c->depth];
  int x0, y0, x1, y1;
  struct c2d_src src;
  if (!c2d_path_bounds(p, 1, c->w, c->h, &x0, &y0, &x1, &y1) || !c2d_src_from_paint(&src, paint, &st->m, st->smoothing))
    return;
  int bw = x1 - x0, bh = y1 - y0;
  unsigned char *cov = malloc((unsigned long)bw * bh);
  if (!cov)
    return;
  if (c2d_rasterize(p, evenodd, x0, y0, bw, bh, cov))
    c2d_draw_cov(c, cov, x0, y0, bw, bh, &src, st->alpha, st->comp, true);
  free(cov);
}

void c2d_fill(struct c2d *c, const struct c2d_path *p, bool evenodd)
{
  fill_outline(c, p, evenodd, &c->st[c->depth].fill);
}

static void stroke_with(struct c2d *c, const struct c2d_path *p, const struct c2d_paint *paint)
{
  struct c2d_state *st = &c->st[c->depth];
  float sc = mat_scale(&st->m), width = st->line_width * sc;
  if (width <= 0 || sc == 0)
    return;
  float *dash = 0;
  if (st->ndash) {
    dash = malloc((unsigned)st->ndash * sizeof *dash);
    if (!dash)
      return;
    for (int i = 0; i < st->ndash; i++)
      dash[i] = st->dash[i] * sc;
  }
  struct c2d_path outline;
  memset(&outline, 0, sizeof outline);
  if (c2d_stroke_to_path(p, width, st->cap, st->join, st->miter_limit, dash, st->ndash, st->dash_off * sc, &outline))
    fill_outline(c, &outline, false, paint);
  c2d_path_free(&outline);
  free(dash);
}

void c2d_stroke(struct c2d *c, const struct c2d_path *p) { stroke_with(c, p, &c->st[c->depth].stroke); }

void c2d_clip(struct c2d *c, const struct c2d_path *p, bool evenodd)
{
  struct c2d_state *st = &c->st[c->depth];
  unsigned long n = (unsigned long)c->w * c->h;
  struct c2d_mask *m = malloc(sizeof *m + n);
  if (!m)
    return;
  m->refs = 1;
  memset(m->px, 0, n);
  int x0, y0, x1, y1;
  if (c2d_path_bounds(p, 1, c->w, c->h, &x0, &y0, &x1, &y1)) {
    int bw = x1 - x0, bh = y1 - y0;
    unsigned char *cov = malloc((unsigned long)bw * bh);
    if (cov && c2d_rasterize(p, evenodd, x0, y0, bw, bh, cov)) {
      for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
          unsigned long i = (unsigned long)(y0 + y) * c->w + x0 + x;
          unsigned cv = cov[(unsigned long)y * bw + x];
          m->px[i] = (unsigned char)(st->clip ? (cv * st->clip->px[i] + 127) / 255 : cv);
        }
    }
    free(cov);
  }
  mask_unref(st->clip);
  st->clip = m;
}

/* Device box of an axis-aligned rectangle whose edges all sit on pixel boundaries. */
static bool aligned_box(const struct c2d_mat *m, float x, float y, float w, float h, int *bx, int *by, int *bw, int *bh)
{
  if (m->b != 0 || m->c != 0)
    return false;
  float ax = m->a * x + m->e, bx2 = m->a * (x + w) + m->e, ay = m->d * y + m->f, by2 = m->d * (y + h) + m->f;
  float lx = ax < bx2 ? ax : bx2, hx = ax < bx2 ? bx2 : ax, ly = ay < by2 ? ay : by2, hy = ay < by2 ? by2 : ay;
  if (fabs(lx - floor(lx + 0.5)) > 1e-3 || fabs(hx - floor(hx + 0.5)) > 1e-3 || fabs(ly - floor(ly + 0.5)) > 1e-3 ||
      fabs(hy - floor(hy + 0.5)) > 1e-3)
    return false;
  *bx = (int)floor(lx + 0.5);
  *by = (int)floor(ly + 0.5);
  *bw = (int)floor(hx + 0.5) - *bx;
  *bh = (int)floor(hy + 0.5) - *by;
  return true;
}

static void box_draw(struct c2d *c, int bx, int by, int bw, int bh, const struct c2d_paint *paint, float alpha,
                     int comp, bool shadow)
{
  struct c2d_state *st = &c->st[c->depth];
  struct c2d_src src;
  int x0 = bx < 0 ? 0 : bx, y0 = by < 0 ? 0 : by, x1 = bx + bw > c->w ? c->w : bx + bw,
      y1 = by + bh > c->h ? c->h : by + bh;
  if (x1 <= x0 || y1 <= y0 || !c2d_src_from_paint(&src, paint, &st->m, st->smoothing))
    return;
  c2d_draw_cov(c, 0, x0, y0, x1 - x0, y1 - y0, &src, alpha, comp, shadow);
}

void c2d_fill_rect(struct c2d *c, float x, float y, float w, float h)
{
  struct c2d_state *st = &c->st[c->depth];
  int bx, by, bw, bh;
  if (w == 0 || h == 0 || x - x != 0 || y - y != 0 || w - w != 0 || h - h != 0)
    return;
  if (aligned_box(&st->m, x, y, w, h, &bx, &by, &bw, &bh) && bw > 0 && bh > 0) {
    box_draw(c, bx, by, bw, bh, &st->fill, st->alpha, st->comp, true);
    return;
  }
  struct c2d_path p;
  memset(&p, 0, sizeof p);
  c2d_path_rect(&p, &st->m, x, y, w, h);
  c2d_fill(c, &p, false);
  c2d_path_free(&p);
}

void c2d_stroke_rect(struct c2d *c, float x, float y, float w, float h)
{
  struct c2d_state *st = &c->st[c->depth];
  if (x - x != 0 || y - y != 0 || w - w != 0 || h - h != 0 || (w == 0 && h == 0))
    return;
  struct c2d_path p;
  memset(&p, 0, sizeof p);
  c2d_path_rect(&p, &st->m, x, y, w, h);
  c2d_stroke(c, &p);
  c2d_path_free(&p);
}

void c2d_clear_rect(struct c2d *c, float x, float y, float w, float h)
{
  struct c2d_state *st = &c->st[c->depth];
  struct c2d_paint opaque = {C2D_COLOR, 0, 0, 0, 255, 0, 0};
  int bx, by, bw, bh;
  if (w == 0 || h == 0 || x - x != 0 || y - y != 0 || w - w != 0 || h - h != 0)
    return;
  if (aligned_box(&st->m, x, y, w, h, &bx, &by, &bw, &bh)) {
    box_draw(c, bx, by, bw, bh, &opaque, 1, C2D_DEST_OUT, false);
    return;
  }
  struct c2d_path p;
  struct c2d_src src;
  int x0, y0, x1, y1;
  memset(&p, 0, sizeof p);
  c2d_path_rect(&p, &st->m, x, y, w, h);
  if (c2d_path_bounds(&p, 1, c->w, c->h, &x0, &y0, &x1, &y1) && c2d_src_from_paint(&src, &opaque, &st->m, false)) {
    unsigned char *cov = malloc((unsigned long)(x1 - x0) * (y1 - y0));
    if (cov && c2d_rasterize(&p, false, x0, y0, x1 - x0, y1 - y0, cov))
      c2d_draw_cov(c, cov, x0, y0, x1 - x0, y1 - y0, &src, 1, C2D_DEST_OUT, false);
    free(cov);
  }
  c2d_path_free(&p);
}

void c2d_draw_image(struct c2d *c, const unsigned char *rgba, int iw, int ih, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh)
{
  struct c2d_state *st = &c->st[c->depth];
  if (!rgba || iw <= 0 || ih <= 0 || sw == 0 || sh == 0 || dw == 0 || dh == 0)
    return;
  if (sw < 0) {
    sx += sw;
    sw = -sw;
    dx += dw;
    dw = -dw;
  }
  if (sh < 0) {
    sy += sh;
    sh = -sh;
    dy += dh;
    dh = -dh;
  }
  /* Clip the source rectangle to the image and shrink the destination to match */
  float kx = dw / sw, ky = dh / sh;
  if (sx < 0) {
    dx -= sx * kx;
    dw += sx * kx;
    sw += sx;
    sx = 0;
  }
  if (sy < 0) {
    dy -= sy * ky;
    dh += sy * ky;
    sh += sy;
    sy = 0;
  }
  if (sx + sw > (float)iw) {
    dw -= (sx + sw - (float)iw) * kx;
    sw = (float)iw - sx;
  }
  if (sy + sh > (float)ih) {
    dh -= (sy + sh - (float)ih) * ky;
    sh = (float)ih - sy;
  }
  if (sw <= 0 || sh <= 0 || dw == 0 || dh == 0)
    return;
  struct c2d_mat to_dev =
      c2d_mat_mul(st->m, (struct c2d_mat){dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh});
  struct c2d_src src;
  memset(&src, 0, sizeof src);
  src.kind = C2D_PATTERN;
  src.img = rgba;
  src.iw = iw;
  src.ih = ih;
  src.cx0 = (int)floor(sx);
  src.cy0 = (int)floor(sy);
  src.cx1 = (int)ceil(sx + sw);
  src.cy1 = (int)ceil(sy + sh);
  src.edge_clamp = true;
  src.smooth = st->smoothing;
  if (!c2d_mat_invert(&to_dev, &src.inv))
    return;
  struct c2d_path p;
  memset(&p, 0, sizeof p);
  c2d_path_rect(&p, &st->m, dx, dy, dw, dh);
  int x0, y0, x1, y1;
  if (c2d_path_bounds(&p, 1, c->w, c->h, &x0, &y0, &x1, &y1)) {
    unsigned char *cov = malloc((unsigned long)(x1 - x0) * (y1 - y0));
    if (cov && c2d_rasterize(&p, false, x0, y0, x1 - x0, y1 - y0, cov))
      c2d_draw_cov(c, cov, x0, y0, x1 - x0, y1 - y0, &src, st->alpha, st->comp, true);
    free(cov);
  }
  c2d_path_free(&p);
}

void c2d_get_image_data(const struct c2d *c, int x, int y, int w, int h, unsigned char *out)
{
  memset(out, 0, (unsigned long)w * h * 4);
  for (int j = 0; j < h; j++) {
    int sy = y + j;
    if (sy < 0 || sy >= c->h)
      continue;
    int x0 = x < 0 ? -x : 0, x1 = x + w > c->w ? c->w - x : w;
    if (x1 > x0)
      memcpy(out + ((unsigned long)j * w + x0) * 4, c->px + ((unsigned long)sy * c->w + x + x0) * 4,
             (unsigned long)(x1 - x0) * 4);
  }
}

void c2d_put_image_data(struct c2d *c, const unsigned char *rgba, int w, int h, int dx, int dy, int sx, int sy, int sw,
                        int sh)
{
  if (sx < 0) {
    sw += sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    sy = 0;
  }
  if (sx + sw > w)
    sw = w - sx;
  if (sy + sh > h)
    sh = h - sy;
  for (int j = 0; j < sh; j++) {
    int ty = dy + sy + j;
    if (ty < 0 || ty >= c->h)
      continue;
    int i0 = 0, i1 = sw;
    if (dx + sx + i0 < 0)
      i0 = -(dx + sx);
    if (dx + sx + i1 > c->w)
      i1 = c->w - (dx + sx);
    if (i1 > i0)
      memcpy(c->px + ((unsigned long)ty * c->w + dx + sx + i0) * 4, rgba + ((unsigned long)(sy + j) * w + sx + i0) * 4,
             (unsigned long)(i1 - i0) * 4);
  }
}

/* ------------------------------------------------------------------ PNG export */

static void put32(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static unsigned char *png_chunk(unsigned char *o, const char *type, const unsigned char *data, unsigned n)
{
  put32(o, n);
  memcpy(o + 4, type, 4);
  if (n)
    memcpy(o + 8, data, n);
  put32(o + 8 + n, crc32(0, o + 4, n + 4));
  return o + 12 + n;
}

unsigned char *c2d_encode_png(const struct c2d *c, int *len)
{
  unsigned long row = (unsigned long)c->w * 4 + 1, raw = row * c->h;
  unsigned long nblocks = (raw + 65534) / 65535;
  unsigned long zlen = 2 + raw + nblocks * 5 + 4;
  unsigned char *z = malloc(zlen);
  unsigned char *png = malloc(8 + 12 + 13 + 12 + zlen + 12);
  if (!z || !png) {
    free(z);
    free(png);
    return 0;
  }
  unsigned char *zp = z;
  *zp++ = 0x78;
  *zp++ = 0x01;
  unsigned a = 1, b = 0;
  unsigned long pos = 0, left = raw;
  while (left) {
    unsigned n = left > 65535 ? 65535 : (unsigned)left;
    *zp++ = left == n;
    *zp++ = (unsigned char)n;
    *zp++ = (unsigned char)(n >> 8);
    *zp++ = (unsigned char)~n;
    *zp++ = (unsigned char)(~n >> 8);
    for (unsigned i = 0; i < n; i++, pos++) {
      unsigned long col = pos % row;
      unsigned char v = col == 0 ? 0 : c->px[(pos / row) * c->w * 4 + col - 1];
      *zp++ = v;
      a = (a + v) % 65521;
      b = (b + a) % 65521;
    }
    left -= n;
  }
  put32(zp, (b << 16) | a);
  zp += 4;
  unsigned char *o = png;
  memcpy(o, "\x89PNG\r\n\x1a\n", 8);
  o += 8;
  unsigned char ihdr[13];
  put32(ihdr, (unsigned)c->w);
  put32(ihdr + 4, (unsigned)c->h);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  o = png_chunk(o, "IHDR", ihdr, 13);
  o = png_chunk(o, "IDAT", z, (unsigned)(zp - z));
  o = png_chunk(o, "IEND", 0, 0);
  free(z);
  *len = (int)(o - png);
  return png;
}
