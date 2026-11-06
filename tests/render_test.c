/* Host tests for the browser's render helpers (user/apps/browser/render_util.h and
 * layout_box_sig): each is checked against the plain per-pixel / linear-scan
 * computation it replaced.
 * Standalone: cc -std=gnu11 -Iuser/apps/browser -Iuser/lib -Itests -include tests/test.h \
 *   -DOWOS_TEST_NAME=render -Dmain=owos_main_render -c tests/render_test.c; see scripts/run-tests.sh */
#define OWOS_SUITE_TEST
#include "layout.h"
#include "render_util.h"
#include <stdlib.h>
#include <string.h>

#define ST_NOWRAP 0x01
#define ST_BOLD 0x10
#define ST_UNDER 0x20
#define ST_ITALIC 0x40

static unsigned rng = 12345;
static unsigned rnd(void)
{
  rng = rng * 1664525u + 1013904223u;
  return rng >> 8;
}

/* ---- reference implementations (the code the helpers replaced) ---- */

static unsigned ref_over(unsigned src, unsigned dst)
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

static unsigned ref_blend(const unsigned char *p, unsigned dst)
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

static unsigned ref_lerp(unsigned c0, unsigned c1, int t)
{
  int a0 = (c0 >> 24) & 255, r0 = (c0 >> 16) & 255, g0 = (c0 >> 8) & 255, b0 = c0 & 255;
  int a1 = (c1 >> 24) & 255, r1 = (c1 >> 16) & 255, g1 = (c1 >> 8) & 255, b1 = c1 & 255;
  unsigned a = a0 + (a1 - a0) * t / 1000, r = r0 + (r1 - r0) * t / 1000, g = g0 + (g1 - g0) * t / 1000,
           b = b0 + (b1 - b0) * t / 1000;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

static unsigned ref_grad_at(const struct ru_gstops *g, int t1000)
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
      return ref_lerp((unsigned)g->col[i - 1], (unsigned)g->col[i], seg);
    }
  return (unsigned)g->col[g->n - 1];
}

static void ref_stops(const struct css_bg_image *bi, struct ru_gstops *g)
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
    if (g->pos[i] < 0) {
      int k = i + 1;
      while (k < n && g->pos[k] < 0)
        k++;
      int next = (k < n) ? g->pos[k] : 100, span = k - (i - 1);
      g->pos[i] = g->pos[i - 1] + (next - g->pos[i - 1]) / (span > 0 ? span : 1);
    }
  for (int i = 1; i < n; i++)
    if (g->pos[i] < g->pos[i - 1])
      g->pos[i] = g->pos[i - 1];
}

static unsigned ref_isqrt(unsigned v)
{
  unsigned r = 0;
  while ((r + 1) * (r + 1) <= v && r < 65535)
    r++;
  return r;
}

static void random_stops(struct css_bg_image *bi)
{
  memset(bi, 0, sizeof *bi);
  bi->nstops = (unsigned char)(1 + rnd() % CSS_BG_STOPS);
  int pos = 0;
  for (int i = 0; i < bi->nstops; i++) {
    unsigned a = rnd() % 3 == 0 ? rnd() & 255 : 255;
    bi->stops[i].color = (int)((a << 24) | (rnd() & 0xffffff));
    if (rnd() % 3 == 0)
      bi->stops[i].pos = -1;
    else {
      pos += (int)(rnd() % 40);
      bi->stops[i].pos = (short)(pos > 100 ? 100 : pos);
    }
  }
}

static void fill_random(unsigned *p, int n)
{
  for (int i = 0; i < n; i++)
    p[i] = rnd() & 0xffffff;
}

int main(void)
{
  test("gradient colour table matches per-t grad_at");
  {
    static struct ru_glut lut;
    for (int iter = 0; iter < 300; iter++) {
      struct css_bg_image bi;
      random_stops(&bi);
      struct ru_gstops g, rg;
      ru_resolve_stops(&bi, &g);
      ref_stops(&bi, &rg);
      expect(g.n == rg.n && !memcmp(g.pos, rg.pos, sizeof(int) * (size_t)g.n) &&
             !memcmp(g.col, rg.col, sizeof(int) * (size_t)g.n));
      const struct ru_glut *l = ru_glut_get(&lut, &g);
      bool opaque = true;
      int bad = 0;
      for (int t = 0; t < RU_GRAD_N; t++) {
        unsigned want = ref_grad_at(&rg, t);
        bad += l->c[t] != want;
        opaque = opaque && (want >> 24) == 255;
      }
      expect(bad == 0);
      expect(l->opaque == opaque);
    }
    struct css_bg_image a, b;
    random_stops(&a);
    do
      random_stops(&b);
    while (a.nstops == b.nstops && !memcmp(a.stops, b.stops, sizeof(struct css_gstop) * a.nstops));
    struct ru_gstops ga, gb;
    ru_resolve_stops(&a, &ga);
    ru_resolve_stops(&b, &gb);
    ru_glut_get(&lut, &ga);
    const struct ru_glut *lb = ru_glut_get(&lut, &gb);
    int bad = 0;
    for (int t = 0; t < RU_GRAD_N; t++)
      bad += lb->c[t] != ru_grad_at(&gb, t);
    expect(bad == 0); /* a different stop set rebuilds the table */
  }

  test("linear gradient row matches the per-pixel divide");
  {
    static struct ru_glut lut;
    int bad = 0, rows = 0;
    for (int iter = 0; iter < 400; iter++) {
      struct css_bg_image bi;
      random_stops(&bi);
      struct ru_gstops g;
      ru_resolve_stops(&bi, &g);
      const struct ru_glut *l = ru_glut_get(&lut, &g);
      int bw = 1 + (int)(rnd() % 400), bh = 1 + (int)(rnd() % 300);
      int dx = rnd() % 5 == 0 ? 0 : (int)(rnd() % 2049) - 1024;
      int dy = (int)(rnd() % 2049) - 1024;
      long lq = (long)(dx < 0 ? -dx : dx) * bw + (long)(dy < 0 ? -dy : dy) * bh;
      if (lq < 1)
        lq = 1;
      int bx = (int)(rnd() % 50) - 25, cx = bw / 2, cy = bh / 2;
      int ry = (int)(rnd() % (unsigned)bh);
      long rowdot = (long)(ry - cy) * dy;
      int x0 = bx < 0 ? 0 : bx, x1 = bx + bw;
      unsigned a[500], b[500];
      fill_random(a, 500);
      memcpy(b, a, sizeof a);
      ru_grad_linear_row(a, x0, x1, bx, cx, dx, rowdot, lq, l);
      for (int xx = x0; xx < x1; xx++) {
        long dot = (long)(xx - bx - cx) * dx + rowdot;
        int t = 500 + (int)(dot * 1000 / lq);
        t = t < 0 ? 0 : t > 1000 ? 1000 : t;
        b[xx] = ref_over(ref_grad_at(&g, t), b[xx]);
      }
      bad += memcmp(a, b, sizeof a) != 0;
      rows++;
    }
    expect(rows == 400 && bad == 0);
  }

  test("radial gradient row matches the per-pixel isqrt path");
  {
    static struct ru_glut lut;
    int bad = 0;
    for (int iter = 0; iter < 200; iter++) {
      struct css_bg_image bi;
      random_stops(&bi);
      struct ru_gstops g;
      ru_resolve_stops(&bi, &g);
      const struct ru_glut *l = ru_glut_get(&lut, &g);
      int bw = 2 + (int)(rnd() % 200), bh = 2 + (int)(rnd() % 200);
      int hw = bw / 2 ? bw / 2 : 1, hh = bh / 2 ? bh / 2 : 1;
      bool circle = rnd() & 1;
      unsigned rad = circle ? ru_isqrt((unsigned)(hw * hw + hh * hh)) : 0;
      int ry = (int)(rnd() % (unsigned)bh) - bh / 2;
      unsigned a[256], b[256];
      fill_random(a, 256);
      memcpy(b, a, sizeof a);
      ru_grad_radial_row(a, 0, bw, 0, bw / 2, ry, circle, hw, hh, rad, l);
      for (int xx = 0; xx < bw; xx++) {
        int rx = xx - bw / 2, t;
        if (circle) {
          unsigned d = ref_isqrt((unsigned)(rx * rx + ry * ry));
          t = rad ? (int)((long)d * 1000 / rad) : 1000;
        } else {
          long nx = (long)rx * 1000 / hw, ny = (long)ry * 1000 / hh;
          t = (int)ref_isqrt((unsigned)(nx * nx + ny * ny));
        }
        t = t < 0 ? 0 : t > 1000 ? 1000 : t;
        b[xx] = ref_over(ref_grad_at(&g, t), b[xx]);
      }
      bad += memcmp(a, b, sizeof a) != 0;
    }
    expect(bad == 0);
  }

  test("isqrt is the floor square root");
  {
    int bad = 0;
    for (unsigned v = 0; v < 70000; v++)
      bad += ru_isqrt(v) != ref_isqrt(v);
    for (int i = 0; i < 2000; i++) {
      unsigned v = rnd() * 251u;
      unsigned r = ru_isqrt(v);
      bad += !((unsigned long)r * r <= v && (unsigned long)(r + 1) * (r + 1) > v);
    }
    expect(bad == 0);
  }

  test("scaled row blits match the per-pixel divide and blend");
  {
    int bad = 0;
    for (int iter = 0; iter < 600; iter++) {
      unsigned iw = 1 + rnd() % 64, dw = 1 + rnd() % 90;
      if (rnd() % 4 == 0)
        dw = iw;
      unsigned char src[64 * 4];
      unsigned mode = rnd() % 3; /* 0 mixed alpha, 1 opaque, 2 transparent-heavy */
      for (unsigned i = 0; i < iw; i++) {
        src[i * 4] = (unsigned char)rnd();
        src[i * 4 + 1] = (unsigned char)rnd();
        src[i * 4 + 2] = (unsigned char)rnd();
        unsigned r = rnd() % 4;
        src[i * 4 + 3] = mode == 1   ? 255
                         : mode == 2 ? (r == 0   ? 255
                                        : r == 1 ? 0
                                                 : (unsigned char)rnd())
                                     : (r < 2 ? 255 : (unsigned char)rnd());
      }
      unsigned xm[128];
      ru_xmap_fill(xm, dw, iw);
      unsigned lo = rnd() % dw, hi = lo + rnd() % (dw - lo + 1);
      unsigned want[128], got[128], gotw[128], goto_[128];
      fill_random(want, 128);
      memcpy(got, want, sizeof want);
      memcpy(gotw, want, sizeof want);
      memcpy(goto_, want, sizeof want);
      for (unsigned x = lo; x < hi; x++)
        want[x] = ref_blend(src + (unsigned long)(x * iw / dw) * 4, want[x]);
      ru_blit_row_rgba(got, src, xm, dw, iw, lo, hi);
      bad += memcmp(got, want, sizeof want) != 0;
      memcpy(got, goto_, sizeof got);
      ru_blit_row_rgba(got, src, 0, dw, iw, lo, hi); /* no map: divide per pixel */
      bad += memcmp(got, want, sizeof want) != 0;

      unsigned words[64];
      bool opaque = true;
      for (unsigned i = 0; i < iw; i++) {
        words[i] = ru_rgba_word(src + i * 4);
        opaque = opaque && src[i * 4 + 3] == 255;
      }
      ru_blit_row_words(gotw, words, xm, dw, iw, false, lo, hi);
      bad += memcmp(gotw, want, sizeof want) != 0;
      if (opaque) {
        ru_words_strip_alpha(words, iw);
        memcpy(gotw, goto_, sizeof gotw);
        ru_blit_row_words(gotw, words, xm, dw, iw, true, lo, hi);
        bad += memcmp(gotw, want, sizeof want) != 0;
      }
    }
    expect(bad == 0);
  }

  test("resampling into a cache buffer equals sampling the source");
  {
    int bad = 0;
    for (int iter = 0; iter < 300; iter++) {
      unsigned iw = 1 + rnd() % 40, ih = 1 + rnd() % 40, cw = 1 + rnd() % 50, ch = 1 + rnd() % 50;
      unsigned char *img = malloc((size_t)iw * ih * 4);
      bool opaque = rnd() % 2;
      for (unsigned i = 0; i < iw * ih; i++) {
        img[i * 4] = (unsigned char)rnd();
        img[i * 4 + 1] = (unsigned char)rnd();
        img[i * 4 + 2] = (unsigned char)rnd();
        img[i * 4 + 3] = opaque ? 255 : (rnd() % 3 ? 255 : (unsigned char)rnd());
      }
      unsigned *dst = calloc((size_t)cw * ch, 4);
      unsigned xm[64];
      ru_xmap_fill(xm, cw, iw);
      struct ru_scaler sc = {dst, xm, iw, ih, cw, ch, 0, true};
      for (unsigned y = 0; y < ih; y++)
        ru_scaler_row(&sc, y, img + (size_t)y * iw * 4);
      bad += sc.next_r != ch;
      for (unsigned y = 0; y < ch; y++)
        for (unsigned x = 0; x < cw; x++) {
          const unsigned char *p = img + ((size_t)(y * ih / ch) * iw + x * iw / cw) * 4;
          bad += dst[y * cw + x] != ru_rgba_word(p);
        }
      /* opaque only reflects pixels actually sampled */
      bool sampled_opaque = true;
      for (unsigned y = 0; y < ch; y++)
        for (unsigned x = 0; x < cw; x++)
          sampled_opaque = sampled_opaque && img[((size_t)(y * ih / ch) * iw + x * iw / cw) * 4 + 3] == 255;
      bad += sc.opaque != sampled_opaque;
      free(dst);
      free(img);
    }
    expect(bad == 0);
  }

  test("image cache evicts least recently used within its byte budget");
  {
    struct ru_imgcache c = {.budget = 1000};
    struct ru_img e = {.iw = 10, .ih = 10, .cw = 10, .ch = 10, .bytes = 400};
    e.uid = 1;
    e.px = malloc(4);
    expect(ru_imgcache_put(&c, &e));
    e.uid = 2;
    e.px = malloc(4);
    expect(ru_imgcache_put(&c, &e));
    expect(c.bytes == 800 && c.n == 2);
    expect(ru_imgcache_find(&c, 1, 10, 10, 5, 5) != 0); /* touch 1: 2 is now the oldest */
    e.uid = 3;
    e.px = malloc(4);
    expect(ru_imgcache_put(&c, &e));
    expect(c.n == 2 && c.bytes == 800);
    expect(ru_imgcache_find(&c, 2, 10, 10, 10, 10) == 0);
    expect(ru_imgcache_find(&c, 1, 10, 10, 10, 10) != 0 && ru_imgcache_find(&c, 3, 10, 10, 10, 10) != 0);
    struct ru_img big = {.uid = 4, .iw = 50, .ih = 50, .cw = 50, .ch = 50, .bytes = 1200};
    big.px = malloc(4);
    expect(!ru_imgcache_put(&c, &big)); /* can never fit: caller keeps the buffer */
    free(big.px);
    expect(c.n == 2);
    struct ru_img s = {.uid = 1, .iw = 40, .ih = 40, .cw = 20, .ch = 20, .bytes = 100};
    s.px = malloc(4);
    expect(ru_imgcache_put(&c, &s));
    expect(ru_imgcache_find(&c, 1, 40, 40, 20, 20) != 0); /* scaled entry serves only its own size */
    expect(ru_imgcache_find(&c, 1, 40, 40, 30, 30) == 0);
    expect(ru_imgcache_find(&c, 1, 10, 10, 77, 77) != 0); /* natural entry serves any size */
    expect(c.bytes <= c.budget);
    ru_imgcache_clear(&c);
    expect(c.n == 0 && c.bytes == 0);
    free(c.e);
  }

  test("glyph table matches a reference map and bounds its bitmap bytes");
  {
    struct ru_gtab t = {.budget = 0};
    int keys = 3000;
    unsigned *cps = malloc(sizeof(unsigned) * (size_t)keys), *pxs = malloc(sizeof(unsigned) * (size_t)keys);
    int bad = 0;
    for (int i = 0; i < keys; i++) {
      cps[i] = rnd() % 4000;
      pxs[i] = rnd() % 5 ? 10 + rnd() % 40 : 0;
      struct ru_gent g = {.cp = cps[i], .px = pxs[i], .w = 3, .h = 2, .adv = (int)(cps[i] * 7 + pxs[i])};
      if (ru_gtab_find(&t, cps[i], pxs[i]))
        continue;
      g.bm = malloc(6);
      memset(g.bm, (int)(cps[i] & 255), 6);
      bad += ru_gtab_put(&t, &g) == 0;
    }
    for (int i = 0; i < keys; i++) {
      struct ru_gent *g = ru_gtab_find(&t, cps[i], pxs[i]);
      bad += !g || g->adv != (int)(cps[i] * 7 + pxs[i]) || !g->bm || g->bm[5] != (unsigned char)(cps[i] & 255);
    }
    bad += ru_gtab_find(&t, 99999, 15) != 0;
    expect(bad == 0);
    ru_gtab_clear(&t);
    expect(ru_gtab_find(&t, cps[0], pxs[0]) == 0 && t.n == 0);
    free(t.e);

    struct ru_gtab b = {.budget = 500};
    bad = 0;
    for (int i = 0; i < 400; i++) {
      struct ru_gent g = {.cp = (unsigned)i, .px = 15, .w = 10, .h = 10, .adv = i};
      g.bm = malloc(100);
      struct ru_gent *p = ru_gtab_put(&b, &g);
      bad += !p || p->adv != i || b.bytes > b.budget;
    }
    expect(bad == 0);
    expect(ru_gtab_find(&b, 399, 15) != 0 && ru_gtab_find(&b, 0, 15) == 0);
    ru_gtab_clear(&b);
    free(b.e);
    free(cps);
    free(pxs);
  }

  test("line scan matches the separate max-px and line-height scans");
  {
    enum { N = 400 };
    char text[N];
    unsigned char fsz[N], bp[N], lh[N];
    for (int i = 0; i < N; i++) {
      unsigned r = rnd() % 8;
      text[i] = r == 0 ? (char)0xc3 : r == 1 ? (char)0xe2 : r == 2 ? (char)0x80 : (char)('a' + rnd() % 26);
      fsz[i] = rnd() % 4 == 0 ? (unsigned char)(8 + rnd() % 30) : 0;
      bp[i] = (unsigned char)(rnd() % 4 == 0 ? rnd() & 7 : 0);
      lh[i] = rnd() % 5 == 0 ? (unsigned char)(rnd() % 40) : 0;
    }
    int bad = 0;
    for (int iter = 0; iter < 500; iter++) {
      int off = (int)(rnd() % N), end = off + (int)(rnd() % (unsigned)(N - off + 1));
      int mx = 15, lm = 0;
      for (int p = off; p < end;) {
        static const int ref_px[8] = {0, 18, 17, 16, 15, 14, 13, 0};
        int sz = bp[p] & 7;
        int g = fsz[p] ? fsz[p] : (sz >= 1 && sz <= 6) ? ref_px[sz] : 0;
        int eff = g ? g : 15;
        if (eff > mx)
          mx = eff;
        if (lh[p] > lm)
          lm = lh[p];
        unsigned char c = (unsigned char)text[p];
        p += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
      }
      int gm, gl;
      ru_line_scan(text, fsz, bp, lh, off, end, 15, &gm, &gl);
      bad += gm != mx || gl != lm;
    }
    expect(bad == 0);
  }

  test("image anchor index finds the lowest image at a text offset");
  {
    int bad = 0;
    for (int iter = 0; iter < 300; iter++) {
      int n = (int)(rnd() % 40);
      int pos[40];
      for (int i = 0; i < n; i++)
        pos[i] = (int)(rnd() % 30);
      struct ru_imgidx ix;
      bad += !ru_imgidx_build(&ix, pos, n);
      for (int p = -1; p < 32; p++) {
        int want = -1;
        for (int q = 0; q < n && want < 0; q++)
          if (pos[q] == p)
            want = q;
        bad += ru_imgidx_find(&ix, p) != want;
      }
      free(ix.ord);
    }
    expect(bad == 0);
  }

  test("z bands with a vertical index visit the same boxes in the same order");
  {
    int bad = 0, visited = 0;
    for (int iter = 0; iter < 300; iter++) {
      int n = 1 + (int)(rnd() % 80);
      int z[80], top[80], bot[80];
      for (int i = 0; i < n; i++) {
        unsigned r = rnd() % 6;
        z[i] = r == 0 ? CSS_Z_AUTO : r == 1 ? -(int)(rnd() % 5) : r == 2 ? (int)(rnd() % 5) : 0;
        top[i] = (int)(rnd() % 600) - 50;
        bot[i] = top[i] + (int)(rnd() % 120);
      }
      struct ru_bands b = {0};
      bad += !ru_bands_build(&b, z, top, bot, n, CSS_Z_AUTO);
      for (int q = 0; q < 6; q++) {
        int vtop = (int)(rnd() % 600) - 20, vbot = vtop + 1 + (int)(rnd() % 200);
        for (int band = 0; band < 3; band++) {
          int want[80], nw = 0, got[80], ng = 0;
          for (int i = 0; i < n; i++) {
            if (ru_z_band(z[i], CSS_Z_AUTO) != band)
              continue;
            if (bot[i] <= vtop || top[i] >= vbot)
              continue;
            want[nw++] = i;
          }
          int lo, hi;
          ru_bands_span(&b, band, vtop, vbot, &lo, &hi);
          for (int p = lo; p < hi; p++) {
            int i = b.idx[p];
            if (bot[i] <= vtop || top[i] >= vbot)
              continue;
            got[ng++] = i;
          }
          bad += ng != nw || memcmp(got, want, sizeof(int) * (size_t)nw) != 0;
          visited += nw;
        }
      }
      ru_bands_free(&b);
    }
    expect(bad == 0 && visited > 0);
  }

  test("scroll plan plus strip repaint equals a full repaint");
  {
    enum { H = 24 };
    int bad = 0;
    for (int s0 = 0; s0 < 70; s0++)
      for (int s1 = 0; s1 < 70; s1++) {
        int pane[H], want[H];
        for (int r = 0; r < H; r++) {
          pane[r] = (s0 + r) * 7 + 1;
          want[r] = (s1 + r) * 7 + 1;
        }
        struct ru_scroll_plan p;
        bool ok = ru_scroll_plan(s0, s1, H, &p);
        bool expect_ok = s1 != s0 && abs(s1 - s0) < H;
        bad += ok != expect_ok;
        if (!ok)
          continue;
        memmove(pane + p.dst, pane + p.src, sizeof(int) * (size_t)p.rows);
        for (int r = p.strip; r < p.strip + p.strip_n; r++)
          pane[r] = (s1 + r) * 7 + 1;
        bad += memcmp(pane, want, sizeof pane) != 0;
        bad += p.rows + p.strip_n != H;
      }
    expect(bad == 0);
  }

  test("selection damage covers every line whose highlight changed");
  {
    int bad = 0, changed_cases = 0;
    for (int iter = 0; iter < 4000; iter++) {
      struct ru_selr a = {0}, b = {0};
      a.has = rnd() % 5 != 0;
      b.has = rnd() % 5 != 0;
      a.l0 = (int)(rnd() % 10);
      a.l1 = a.l0 + (int)(rnd() % (unsigned)(10 - a.l0));
      b.l0 = (int)(rnd() % 10);
      b.l1 = b.l0 + (int)(rnd() % (unsigned)(10 - b.l0));
      a.c0 = (int)(rnd() % 4);
      a.c1 = (int)(rnd() % 4);
      b.c0 = (int)(rnd() % 4);
      b.c1 = (int)(rnd() % 4);
      if (a.l0 == a.l1 && a.c0 > a.c1) {
        int t = a.c0;
        a.c0 = a.c1;
        a.c1 = t;
      }
      if (b.l0 == b.l1 && b.c0 > b.c1) {
        int t = b.c0;
        b.c0 = b.c1;
        b.c1 = t;
      }
      int lo = 0, hi = -1;
      bool any = ru_sel_damage(&a, &b, &lo, &hi);
      bool diff_any = false;
      for (int line = 0; line < 10; line++) {
        int sa0 = -1, sa1 = -1, sb0 = -1, sb1 = -1;
        if (a.has && line >= a.l0 && line <= a.l1) {
          sa0 = line == a.l0 ? a.c0 : 0;
          sa1 = line == a.l1 ? a.c1 : 100;
        }
        if (b.has && line >= b.l0 && line <= b.l1) {
          sb0 = line == b.l0 ? b.c0 : 0;
          sb1 = line == b.l1 ? b.c1 : 100;
        }
        bool differs = sa0 != sb0 || sa1 != sb1;
        if (differs) {
          diff_any = true;
          bad += !any || line < lo || line > hi;
        }
      }
      bad += diff_any && !any;
      changed_cases += diff_any;
    }
    expect(bad == 0 && changed_cases > 100);
  }

  test("layout signatures react to wrapping inputs only");
  {
    enum { N = 64 };
    char text[N];
    unsigned char fsz[N], lh[N], bp[N], attr[N];
    for (int i = 0; i < N; i++) {
      text[i] = (char)('a' + i % 26);
      fsz[i] = (unsigned char)(i % 3 ? 0 : 20);
      lh[i] = 0;
      bp[i] = 0;
      attr[i] = 0;
    }
    unsigned base = ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP);
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) == base);
    text[10] = 'Z';
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) != base);
    text[10] = (char)('a' + 10);
    fsz[7] = 33;
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) != base);
    fsz[7] = (unsigned char)(7 % 3 ? 0 : 20);
    lh[3] = 22;
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) != base);
    lh[3] = 0;
    bp[5] = 2;
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) != base);
    bp[5] = 0;
    attr[9] = ST_NOWRAP;
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) != base);
    attr[9] = ST_BOLD | ST_UNDER | ST_ITALIC;
    expect(ru_text_sig(text, fsz, lh, bp, attr, N, ST_NOWRAP) == base);
    expect(ru_text_sig(text, fsz, lh, bp, attr, N - 1, ST_NOWRAP) != base);

    struct lbox b;
    memset(&b, 0, sizeof b);
    unsigned h0 = layout_box_sig(&b, 1);
    expect(layout_box_sig(&b, 1) == h0);
    b.bg = 0x123456;
    b.border_color = 0xabcdef;
    b.opacity = 40;
    b.radius = 6;
    b.sh_color = 0x80000000u;
    b.outline_w = 2;
    b.z_index = 3;
    expect(layout_box_sig(&b, 1) == h0); /* paint-only fields are ignored */
    struct lbox c = b;
    c.margin.t = 5;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.padding.l = 2;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.spec_w = 100;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.kind = LB_FLEX;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.flex_grow = 2;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.cols_t[3].v = 40;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.t1 = 9;
    expect(layout_box_sig(&c, 1) != h0);
    c = b;
    c.off_t = 4;
    expect(layout_box_sig(&c, 1) != h0);
  }
  return 0;
}
