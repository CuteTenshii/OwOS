/* render_util.h - pure rendering helpers shared by main.c and the host tests */
#ifndef OWOS_RENDER_UTIL_H
#define OWOS_RENDER_UTIL_H

#include <stdbool.h>

#include "css.h"

/* ---- gradients ---- */

#define RU_GRAD_N 1001

/* Gradient stops with positions normalised to percent (autos filled in). */
struct ru_gstops {
  int col[CSS_BG_STOPS];
  int pos[CSS_BG_STOPS];
  int n;
};

static inline void ru_resolve_stops(const struct css_bg_image *bi, struct ru_gstops *g)
{
  int n = bi->nstops;
  if (n > CSS_BG_STOPS)
    n = CSS_BG_STOPS;
  g->n = n;
  for (int i = 0; i < n; i++) {
    g->col[i] = bi->stops[i].color;
    g->pos[i] = bi->stops[i].pos;
  }
  if (n > 0) {
    if (g->pos[0] < 0)
      g->pos[0] = 0;
    if (g->pos[n - 1] < 0)
      g->pos[n - 1] = 100;
  }
  for (int i = 1; i < n - 1; i++)
    if (g->pos[i] < 0) { /* spread autos toward the next set stop */
      int k = i + 1;
      while (k < n && g->pos[k] < 0)
        k++;
      int next = (k < n) ? g->pos[k] : 100, span = k - (i - 1);
      g->pos[i] = g->pos[i - 1] + (next - g->pos[i - 1]) / (span > 0 ? span : 1);
    }
  for (int i = 1; i < n; i++)
    if (g->pos[i] < g->pos[i - 1])
      g->pos[i] = g->pos[i - 1]; /* monotone */
}

static inline unsigned ru_lerp_argb(unsigned c0, unsigned c1, int t) /* t in 0..1000 */
{
  int a0 = (c0 >> 24) & 255, r0 = (c0 >> 16) & 255, g0 = (c0 >> 8) & 255, b0 = c0 & 255;
  int a1 = (c1 >> 24) & 255, r1 = (c1 >> 16) & 255, g1 = (c1 >> 8) & 255, b1 = c1 & 255;
  unsigned a = a0 + (a1 - a0) * t / 1000, r = r0 + (r1 - r0) * t / 1000, g = g0 + (g1 - g0) * t / 1000,
           b = b0 + (b1 - b0) * t / 1000;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline unsigned ru_grad_at(const struct ru_gstops *g, int t1000) /* argb at param t (0..1000) */
{
  if (g->n == 0)
    return 0;
  int t100 = t1000 / 10;
  if (t100 <= g->pos[0])
    return (unsigned)g->col[0];
  if (t100 >= g->pos[g->n - 1])
    return (unsigned)g->col[g->n - 1];
  for (int i = 1; i < g->n; i++)
    if (t100 <= g->pos[i]) {
      int lo = g->pos[i - 1], hi = g->pos[i];
      int seg = hi > lo ? (t1000 - lo * 10) * 1000 / ((hi - lo) * 10) : 0;
      if (seg < 0)
        seg = 0;
      if (seg > 1000)
        seg = 1000;
      return ru_lerp_argb((unsigned)g->col[i - 1], (unsigned)g->col[i], seg);
    }
  return (unsigned)g->col[g->n - 1];
}

/* Composite a straight-ARGB source colour over a framebuffer pixel. */
static inline unsigned ru_over_argb(unsigned src, unsigned dst)
{
  unsigned a = (src >> 24) & 255;
  if (a == 255)
    return src & 0xffffff;
  if (a == 0)
    return dst;
  unsigned ia = 255 - a;
  unsigned r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia) / 255;
  unsigned g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia) / 255;
  unsigned b = ((src & 255) * a + (dst & 255) * ia) / 255;
  return (r << 16) | (g << 8) | b;
}

static inline unsigned ru_isqrt(unsigned v)
{
  unsigned r = 0, b = 1u << 30;
  while (b > v)
    b >>= 2;
  while (b) {
    if (v >= r + b) {
      v -= r + b;
      r = (r >> 1) + b;
    } else
      r >>= 1;
    b >>= 2;
  }
  return r;
}

/* Colour at every t in 0..1000 for one stop set, reused across boxes and frames. */
struct ru_glut {
  unsigned c[RU_GRAD_N];
  struct ru_gstops key;
  bool valid;
  bool opaque; /* every entry has alpha 255 */
};

static inline const struct ru_glut *ru_glut_get(struct ru_glut *l, const struct ru_gstops *g)
{
  bool same = l->valid && l->key.n == g->n;
  for (int i = 0; same && i < g->n; i++)
    same = l->key.col[i] == g->col[i] && l->key.pos[i] == g->pos[i];
  if (same)
    return l;
  l->key = *g;
  l->opaque = true;
  for (int t = 0; t < RU_GRAD_N; t++) {
    l->c[t] = ru_grad_at(g, t);
    if ((l->c[t] >> 24) != 255)
      l->opaque = false;
  }
  l->valid = true;
  return l;
}

static inline void ru_lut_put(unsigned *p, unsigned c, bool opaque)
{
  *p = opaque ? (c & 0xffffff) : ru_over_argb(c, *p);
}

static inline long ru_floordiv(long a, long b)
{
  long q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* One row of a linear gradient */
static inline void ru_grad_linear_row(unsigned *drow, int x0, int x1, int bx, int cx, int dx, long rowdot, long lq,
                                      const struct ru_glut *l)
{
  if (x1 <= x0)
    return;
  if (dx == 0) { /* vertical: one colour for the whole row */
    long t = 500 + rowdot * 1000 / lq;
    t = t < 0 ? 0 : t > 1000 ? 1000 : t;
    for (int xx = x0; xx < x1; xx++)
      ru_lut_put(&drow[xx], l->c[t], l->opaque);
    return;
  }
  long num = ((long)(x0 - bx - cx) * dx + rowdot) * 1000;
  long step = (long)dx * 1000;
  long q = ru_floordiv(num, lq), r = num - q * lq;
  long sq = ru_floordiv(step, lq), sr = step - sq * lq;
  for (int xx = x0; xx < x1; xx++) {
    long tq = (q < 0 && r != 0) ? q + 1 : q;
    long t = 500 + tq;
    if (t < 0)
      t = 0;
    if (t > 1000)
      t = 1000;
    ru_lut_put(&drow[xx], l->c[t], l->opaque);
    q += sq;
    r += sr;
    if (r >= lq) {
      r -= lq;
      q++;
    }
  }
}

/* One row of a radial gradient */
static inline void ru_grad_radial_row(unsigned *drow, int x0, int x1, int bx, int cx, int ry, bool circle, int hw,
                                      int hh, unsigned rad, const struct ru_glut *l)
{
  for (int xx = x0; xx < x1; xx++) {
    int rx = xx - bx - cx;
    int t;
    if (circle) {
      unsigned d = ru_isqrt((unsigned)(rx * rx + ry * ry));
      t = rad ? (int)((long)d * 1000 / rad) : 1000;
    } else {
      long nx = (long)rx * 1000 / hw, ny = (long)ry * 1000 / hh;
      t = (int)ru_isqrt((unsigned)(nx * nx + ny * ny));
    }
    if (t < 0)
      t = 0;
    if (t > 1000)
      t = 1000;
    ru_lut_put(&drow[xx], l->c[t], l->opaque);
  }
}

/* ---- image scaling and blending ---- */

/* Composite one RGBA source pixel over a framebuffer pixel. */
static inline unsigned ru_blend_rgba(const unsigned char *p, unsigned dst)
{
  unsigned a = p[3];
  if (a == 255)
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
  if (a == 0)
    return dst;
  unsigned ia = 255 - a;
  unsigned r = (p[0] * a + ((dst >> 16) & 0xff) * ia) / 255;
  unsigned g = (p[1] * a + ((dst >> 8) & 0xff) * ia) / 255;
  unsigned b = (p[2] * a + (dst & 0xff) * ia) / 255;
  return (r << 16) | (g << 8) | b;
}

static inline unsigned ru_rgba_word(const unsigned char *p)
{
  return ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

/* Source column for each of dw destination pixels. */
static inline void ru_xmap_fill(unsigned *xm, unsigned dw, unsigned iw)
{
  for (unsigned x = 0; x < dw; x++)
    xm[x] = x * iw / dw;
}

static inline unsigned ru_src_x(const unsigned *xm, unsigned x, unsigned iw, unsigned dw)
{
  return xm ? xm[x] : x * iw / dw;
}

/* Blend destination columns [lo,hi) of a dw-wide row from an RGBA byte row, nearest-neighbour */
static inline void ru_blit_row_rgba(unsigned *drow, const unsigned char *srow, const unsigned *xm, unsigned dw,
                                    unsigned iw, unsigned lo, unsigned hi)
{
  if (dw == iw) {
    for (unsigned x = lo; x < hi; x++)
      drow[x] = ru_blend_rgba(srow + (unsigned long)x * 4, drow[x]);
    return;
  }
  for (unsigned x = lo; x < hi; x++)
    drow[x] = ru_blend_rgba(srow + (unsigned long)ru_src_x(xm, x, iw, dw) * 4, drow[x]);
}

/* Same from a 0xAARRGGBB word row */
static inline void ru_blit_row_words(unsigned *drow, const unsigned *srow, const unsigned *xm, unsigned dw, unsigned iw,
                                     bool opaque, unsigned lo, unsigned hi)
{
  if (opaque && dw == iw) {
    memcpy(drow + lo, srow + lo, (unsigned long)(hi - lo) * 4);
    return;
  }
  if (opaque) {
    for (unsigned x = lo; x < hi; x++)
      drow[x] = srow[ru_src_x(xm, x, iw, dw)];
    return;
  }
  if (dw == iw) {
    for (unsigned x = lo; x < hi; x++)
      drow[x] = ru_over_argb(srow[x], drow[x]);
    return;
  }
  for (unsigned x = lo; x < hi; x++)
    drow[x] = ru_over_argb(srow[ru_src_x(xm, x, iw, dw)], drow[x]);
}

/* Nearest-neighbour resample of decoded PNG rows into a cw x ch word buffer, fed one source row */
struct ru_scaler {
  unsigned *dst;
  const unsigned *xm;
  unsigned iw, ih, cw, ch, next_r;
  bool opaque; /* stays true while every pixel is fully opaque */
};

static inline void ru_scaler_row(struct ru_scaler *s, unsigned y, const unsigned char *rgba)
{
  while (s->next_r < s->ch && (s->next_r * s->ih / s->ch) == y) {
    unsigned *drow = s->dst + (unsigned long)s->next_r * s->cw;
    for (unsigned x = 0; x < s->cw; x++) {
      const unsigned char *p = rgba + (unsigned long)ru_src_x(s->xm, x, s->iw, s->cw) * 4;
      if (p[3] != 255)
        s->opaque = false;
      drow[x] = ru_rgba_word(p);
    }
    s->next_r++;
  }
}

/* Drop the alpha byte of a finished fully-opaque buffer so rows can be copied straight into */
static inline void ru_words_strip_alpha(unsigned *px, unsigned long n)
{
  for (unsigned long i = 0; i < n; i++)
    px[i] &= 0xffffff;
}

/* ---- decoded image cache (byte budget, LRU) ---- */

struct ru_img {
  int uid;
  unsigned iw, ih; /* natural size */
  unsigned cw, ch;
  unsigned *px;
  unsigned long bytes;
  unsigned stamp;
  bool opaque;
};

struct ru_imgcache {
  struct ru_img *e;
  int n, cap;
  unsigned long bytes, budget;
  unsigned clock;
};

/* A natural-size entry serves any display size; a scaled one only its own. */
static inline struct ru_img *ru_imgcache_find(struct ru_imgcache *c, int uid, unsigned iw, unsigned ih, unsigned dw,
                                              unsigned dh)
{
  for (int i = 0; i < c->n; i++) {
    struct ru_img *e = &c->e[i];
    if (e->uid != uid || e->iw != iw || e->ih != ih)
      continue;
    if ((e->cw == iw && e->ch == ih) || (e->cw == dw && e->ch == dh)) {
      e->stamp = ++c->clock;
      return e;
    }
  }
  return 0;
}

static inline void ru_imgcache_drop(struct ru_imgcache *c, int i)
{
  c->bytes -= c->e[i].bytes;
  free(c->e[i].px);
  c->e[i] = c->e[--c->n];
}

/* Take ownership of e->px on success */
static inline bool ru_imgcache_put(struct ru_imgcache *c, const struct ru_img *e)
{
  if (e->bytes > c->budget)
    return false;
  for (int i = 0; i < c->n; i++)
    if (c->e[i].uid == e->uid && c->e[i].iw == e->iw && c->e[i].ih == e->ih && c->e[i].cw == e->cw &&
        c->e[i].ch == e->ch) {
      ru_imgcache_drop(c, i--);
    }
  while (c->n > 0 && c->bytes + e->bytes > c->budget) {
    int v = 0;
    for (int i = 1; i < c->n; i++)
      if ((int)(c->e[i].stamp - c->e[v].stamp) < 0)
        v = i;
    ru_imgcache_drop(c, v);
  }
  if (c->n >= c->cap) {
    int nc = c->cap ? c->cap * 2 : 8;
    struct ru_img *p = realloc(c->e, (unsigned long)nc * sizeof *p);
    if (!p)
      return false;
    c->e = p;
    c->cap = nc;
  }
  c->e[c->n] = *e;
  c->e[c->n].stamp = ++c->clock;
  c->n++;
  c->bytes += e->bytes;
  return true;
}

static inline void ru_imgcache_clear(struct ru_imgcache *c)
{
  for (int i = 0; i < c->n; i++)
    free(c->e[i].px);
  c->n = 0;
  c->bytes = 0;
}

/* ---- glyph and advance cache keyed by (codepoint, px) ---- */

struct ru_gent {
  unsigned cp, px;
  unsigned char *bm;
  int w, h, ox, oy, adv;
  bool used;
};

struct ru_gtab {
  struct ru_gent *e;
  unsigned cap, n;
  unsigned long bytes, budget;
};

static inline unsigned ru_ghash(unsigned cp, unsigned px)
{
  unsigned h = cp * 2654435761u ^ (px * 40503u);
  return h ^ (h >> 15);
}

static inline struct ru_gent *ru_gtab_find(const struct ru_gtab *t, unsigned cp, unsigned px)
{
  if (!t->cap)
    return 0;
  for (unsigned i = ru_ghash(cp, px) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
    if (!t->e[i].used)
      return 0;
    if (t->e[i].cp == cp && t->e[i].px == px)
      return &t->e[i];
  }
}

static inline void ru_gtab_clear(struct ru_gtab *t)
{
  for (unsigned i = 0; i < t->cap; i++) {
    free(t->e[i].bm);
    t->e[i].bm = 0;
    t->e[i].used = false;
  }
  t->n = 0;
  t->bytes = 0;
}

static inline void ru_gtab_slot(struct ru_gent *e, unsigned cap, const struct ru_gent *g)
{
  unsigned i = ru_ghash(g->cp, g->px) & (cap - 1);
  while (e[i].used)
    i = (i + 1) & (cap - 1);
  e[i] = *g;
}

/* Store a copy of *g (the table then owns g->bm) */
static inline struct ru_gent *ru_gtab_put(struct ru_gtab *t, const struct ru_gent *g)
{
  unsigned long add = (unsigned long)(g->w > 0 && g->h > 0 ? g->w * g->h : 0);
  if (t->budget && t->bytes + add > t->budget)
    ru_gtab_clear(t);
  if ((t->n + 1) * 10 > t->cap * 7) {
    unsigned nc = t->cap ? t->cap * 2 : 256;
    struct ru_gent *ne = calloc(nc, sizeof *ne);
    if (!ne)
      return 0;
    for (unsigned i = 0; i < t->cap; i++)
      if (t->e[i].used)
        ru_gtab_slot(ne, nc, &t->e[i]);
    free(t->e);
    t->e = ne;
    t->cap = nc;
  }
  struct ru_gent in = *g;
  in.used = true;
  ru_gtab_slot(t->e, t->cap, &in);
  t->n++;
  t->bytes += add;
  return ru_gtab_find(t, g->cp, g->px);
}

/* ---- line metrics ---- */

/* Heading pixel sizes (BP_SIZE 1..6 = h1..h6); 0 = ordinary body text, 7 = a horizontal rule. */
static const int ru_size_px[8] = {0, 18, 17, 16, 15, 14, 13, 0};

static inline int ru_glyph_px(unsigned char fsz, unsigned char bp)
{
  if (fsz)
    return fsz;
  int sz = bp & 7;
  return (sz >= 1 && sz <= 6) ? ru_size_px[sz] : 0;
}

static inline int ru_utf8_len(unsigned char c)
{
  return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}

/* Largest effective font px (floor body_px) and largest CSS line-height over the glyphs */
static inline void ru_line_scan(const char *text, const unsigned char *fsz, const unsigned char *bp,
                                const unsigned char *lh, int off, int end, int body_px, int *maxpx, int *lhmax)
{
  int mx = body_px, lm = 0;
  for (int p = off; p < end;) {
    int g = ru_glyph_px(fsz[p], bp[p]);
    int eff = g ? g : body_px;
    if (eff > mx)
      mx = eff;
    if (lh[p] > lm)
      lm = lh[p];
    p += ru_utf8_len((unsigned char)text[p]);
  }
  *maxpx = mx;
  *lhmax = lm;
}

/* ---- inline image lookup by anchor offset ---- */

/* Indices of imgs ordered by (text_pos, index) */
struct ru_imgidx {
  int *ord;
  const int *pos;
  int n;
};

static inline bool ru_imgidx_build(struct ru_imgidx *ix, const int *pos, int n)
{
  ix->ord = n ? malloc((unsigned long)n * sizeof *ix->ord) : 0;
  ix->pos = pos;
  ix->n = n;
  if (n && !ix->ord) {
    ix->n = 0;
    return false;
  }
  for (int i = 0; i < n; i++) { /* insertion sort: images arrive nearly in order */
    int v = i, j = i - 1;
    while (j >= 0 && pos[ix->ord[j]] > pos[v]) {
      ix->ord[j + 1] = ix->ord[j];
      j--;
    }
    ix->ord[j + 1] = v;
  }
  return true;
}

static inline int ru_imgidx_find(const struct ru_imgidx *ix, int text_pos)
{
  int lo = 0, hi = ix->n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (ix->pos[ix->ord[mid]] < text_pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < ix->n && ix->pos[ix->ord[lo]] == text_pos ? ix->ord[lo] : -1;
}

/* ---- paint order: z bands with vertical culling ---- */

/* Boxes bucketed by z band (below flow, flow, above flow), each band in document order */
struct ru_bands {
  int *idx, *pmax_bot, *smin_top; /* all bands, concatenated */
  int start[4];
};

static inline int ru_z_band(int z, int z_auto)
{
  int ez = (z == z_auto) ? 0 : z;
  return ez < 0 ? 0 : (ez == 0 ? 1 : 2);
}

static inline void ru_bands_free(struct ru_bands *b)
{
  free(b->idx);
  b->idx = b->pmax_bot = b->smin_top = 0;
  for (int i = 0; i < 4; i++)
    b->start[i] = 0;
}

static inline bool ru_bands_build(struct ru_bands *b, const int *z, const int *top, const int *bot, int n, int z_auto)
{
  ru_bands_free(b);
  if (n <= 0)
    return true;
  int *mem = malloc((unsigned long)n * 3 * sizeof *mem);
  if (!mem)
    return false;
  b->idx = mem;
  b->pmax_bot = mem + n;
  b->smin_top = mem + 2 * n;
  int cnt[3] = {0, 0, 0};
  for (int i = 0; i < n; i++)
    cnt[ru_z_band(z[i], z_auto)]++;
  b->start[0] = 0;
  for (int k = 0; k < 3; k++)
    b->start[k + 1] = b->start[k] + cnt[k];
  int fill[3] = {b->start[0], b->start[1], b->start[2]};
  for (int i = 0; i < n; i++)
    b->idx[fill[ru_z_band(z[i], z_auto)]++] = i;
  for (int k = 0; k < 3; k++) {
    int s = b->start[k], e = b->start[k + 1];
    int mx = 0;
    for (int p = s; p < e; p++) {
      int v = bot[b->idx[p]];
      mx = (p == s || v > mx) ? v : mx;
      b->pmax_bot[p] = mx;
    }
    int mn = 0;
    for (int p = e - 1; p >= s; p--) {
      int v = top[b->idx[p]];
      mn = (p == e - 1 || v < mn) ? v : mn;
      b->smin_top[p] = mn;
    }
  }
  return true;
}

/* Positions [*lo,*hi) of band k that can touch the doc-px range [vtop,vbot). */
static inline void ru_bands_span(const struct ru_bands *b, int k, int vtop, int vbot, int *lo, int *hi)
{
  int s = b->start[k], e = b->start[k + 1];
  int a = s, c = e;
  while (a < c) { /* first position whose running max bottom passes vtop */
    int m = (a + c) / 2;
    if (b->pmax_bot[m] > vtop)
      c = m;
    else
      a = m + 1;
  }
  *lo = a;
  int d = a, f = e;
  while (d < f) { /* first position whose running min top reaches vbot */
    int m = (d + f) / 2;
    if (b->smin_top[m] >= vbot)
      f = m;
    else
      d = m + 1;
  }
  *hi = d;
}

/* ---- scroll blit plan ---- */

/* Moving the pane from scroll `from` to `to` (phh rows tall) */
struct ru_scroll_plan {
  int src, dst, rows, strip, strip_n;
};

static inline bool ru_scroll_plan(int from, int to, int phh, struct ru_scroll_plan *p)
{
  int d = to - from;
  if (d == 0 || d >= phh || -d >= phh)
    return false;
  if (d > 0) { /* content moves up */
    p->src = d;
    p->dst = 0;
    p->rows = phh - d;
    p->strip = phh - d;
    p->strip_n = d;
  } else {
    p->src = 0;
    p->dst = -d;
    p->rows = phh + d;
    p->strip = 0;
    p->strip_n = -d;
  }
  return true;
}

/* ---- selection damage ---- */

struct ru_selr {
  bool has;
  int l0, c0, l1, c1; /* start line/column, end line/column */
};

static inline bool ru_sel_edge(int la, int ca, int lb, int cb, int *lo, int *hi, bool *any)
{
  if (la == lb && ca == cb)
    return false;
  int a = la < lb ? la : lb, b = la > lb ? la : lb;
  if (!*any || a < *lo)
    *lo = a;
  if (!*any || b > *hi)
    *hi = b;
  *any = true;
  return true;
}

/* Lines whose highlight can differ between two selections */
static inline bool ru_sel_damage(const struct ru_selr *a, const struct ru_selr *b, int *lo, int *hi)
{
  if (!a->has && !b->has)
    return false;
  if (!a->has || !b->has) {
    const struct ru_selr *s = a->has ? a : b;
    *lo = s->l0;
    *hi = s->l1;
    return true;
  }
  bool any = false;
  ru_sel_edge(a->l0, a->c0, b->l0, b->c0, lo, hi, &any);
  ru_sel_edge(a->l1, a->c1, b->l1, b->c1, lo, hi, &any);
  return any;
}

/* ---- layout signature ---- */

static inline unsigned ru_mix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 16777619u;
}

/* Hash of everything in the glyph stream that decides wrapping and line height. */
static inline unsigned ru_text_sig(const char *text, const unsigned char *fsz, const unsigned char *lh,
                                   const unsigned char *bp, const unsigned char *attr, int len, unsigned char nowrap)
{
  unsigned h = (unsigned)len;
  for (int i = 0; i < len; i++) {
    unsigned w = (unsigned char)text[i] | ((unsigned)fsz[i] << 8) | ((unsigned)lh[i] << 16) | ((unsigned)bp[i] << 24);
    h = ru_mix(h, w ^ ((attr[i] & nowrap) ? 0x55aa55aau : 0));
  }
  return h;
}

#endif /* OWOS_RENDER_UTIL_H */
