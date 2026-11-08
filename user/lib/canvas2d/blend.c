/* blend.c - paint sources, Porter-Duff / blend-mode compositing, clipping and shadows for canvas2d. */
#include "c2int.h"
#include "math.h"
#include "owos.h"

/* ------------------------------------------------------------------ sources */

void c2d_src_color(struct c2d_src *s, const unsigned char rgba[4])
{
  memset(s, 0, sizeof *s);
  s->kind = C2D_COLOR;
  memcpy(s->col, rgba, 4);
}

bool c2d_src_from_paint(struct c2d_src *s, const struct c2d_paint *p, const struct c2d_mat *ctm, bool smooth)
{
  memset(s, 0, sizeof *s);
  s->kind = p->kind;
  s->smooth = smooth;
  if (p->kind == C2D_COLOR) {
    s->col[0] = p->r;
    s->col[1] = p->g;
    s->col[2] = p->b;
    s->col[3] = p->a;
    return true;
  }
  if (!c2d_mat_invert(ctm, &s->inv))
    return false;
  if (p->kind == C2D_PATTERN) {
    if (!p->pat || !p->pat->rgba)
      return false;
    s->img = p->pat->rgba;
    s->iw = s->cx1 = p->pat->w;
    s->ih = s->cy1 = p->pat->h;
    s->repeat_x = p->pat->repeat_x;
    s->repeat_y = p->pat->repeat_y;
    return true;
  }
  s->grad = p->grad;
  return p->grad != 0;
}

static void gradient_color(const struct c2d_gradient *g, float t, unsigned char out[4])
{
  const struct c2d_stop *st = g->stop;
  int n = g->nstop;
  if (n == 0) {
    memset(out, 0, 4);
    return;
  }
  if (t <= st[0].off) {
    out[0] = st[0].r, out[1] = st[0].g, out[2] = st[0].b, out[3] = st[0].a;
    return;
  }
  for (int i = 1; i < n; i++) {
    if (t > st[i].off)
      continue;
    float span = st[i].off - st[i - 1].off;
    float k = span > 0 ? (t - st[i - 1].off) / span : 1;
    out[0] = (unsigned char)(st[i - 1].r + (st[i].r - st[i - 1].r) * k + 0.5f);
    out[1] = (unsigned char)(st[i - 1].g + (st[i].g - st[i - 1].g) * k + 0.5f);
    out[2] = (unsigned char)(st[i - 1].b + (st[i].b - st[i - 1].b) * k + 0.5f);
    out[3] = (unsigned char)(st[i - 1].a + (st[i].a - st[i - 1].a) * k + 0.5f);
    return;
  }
  out[0] = st[n - 1].r, out[1] = st[n - 1].g, out[2] = st[n - 1].b, out[3] = st[n - 1].a;
}

/* Position along the gradient axis for the point (x,y); false where nothing is painted. */
static bool gradient_t(const struct c2d_gradient *g, float x, float y, float *t)
{
  if (g->kind == C2D_LINEAR) {
    float dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
    if (l2 == 0)
      return false;
    *t = ((x - g->x0) * dx + (y - g->y0) * dy) / l2;
    return true;
  }
  float cdx = g->x1 - g->x0, cdy = g->y1 - g->y0, dr = g->r1 - g->r0;
  float pdx = x - g->x0, pdy = y - g->y0;
  float a = cdx * cdx + cdy * cdy - dr * dr;
  float b = pdx * cdx + pdy * cdy + g->r0 * dr;
  float cc = pdx * pdx + pdy * pdy - g->r0 * g->r0;
  float s;
  if (fabs(a) < 1e-6f) {
    if (fabs(b) < 1e-9f)
      return false;
    s = cc / (2 * b);
    if (g->r0 + s * dr < 0)
      return false;
  } else {
    float disc = b * b - a * cc;
    if (disc < 0)
      return false;
    float sq = (float)sqrt(disc), s1 = (b + sq) / a, s2 = (b - sq) / a;
    if (s1 < s2) {
      float tmp = s1;
      s1 = s2;
      s2 = tmp;
    }
    if (g->r0 + s1 * dr >= 0)
      s = s1;
    else if (g->r0 + s2 * dr >= 0)
      s = s2;
    else
      return false;
  }
  *t = s;
  return true;
}

static const unsigned char *texel(const struct c2d_src *s, int x, int y)
{
  if (s->repeat_x) {
    x %= s->iw;
    if (x < 0)
      x += s->iw;
  } else if (s->edge_clamp) {
    x = x < s->cx0 ? s->cx0 : x >= s->cx1 ? s->cx1 - 1 : x;
  } else if (x < 0 || x >= s->iw) {
    return 0;
  }
  if (s->repeat_y) {
    y %= s->ih;
    if (y < 0)
      y += s->ih;
  } else if (s->edge_clamp) {
    y = y < s->cy0 ? s->cy0 : y >= s->cy1 ? s->cy1 - 1 : y;
  } else if (y < 0 || y >= s->ih) {
    return 0;
  }
  return s->img + ((unsigned long)y * s->iw + x) * 4;
}

static void sample_image(const struct c2d_src *s, float u, float v, unsigned char out[4])
{
  if (!s->smooth) {
    const unsigned char *t = texel(s, (int)floor(u), (int)floor(v));
    if (t)
      memcpy(out, t, 4);
    else
      memset(out, 0, 4);
    return;
  }
  u -= 0.5f;
  v -= 0.5f;
  int x0 = (int)floor(u), y0 = (int)floor(v);
  float fx = u - (float)x0, fy = v - (float)y0;
  float acc[4] = {0, 0, 0, 0};
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      const unsigned char *t = texel(s, x0 + i, y0 + j);
      if (!t)
        continue;
      float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), a = (float)t[3] * w;
      acc[0] += (float)t[0] * a;
      acc[1] += (float)t[1] * a;
      acc[2] += (float)t[2] * a;
      acc[3] += a;
    }
  if (acc[3] <= 0) {
    memset(out, 0, 4);
    return;
  }
  for (int k = 0; k < 3; k++)
    out[k] = (unsigned char)(acc[k] / acc[3] + 0.5f);
  out[3] = (unsigned char)(acc[3] + 0.5f);
}

static void src_eval(const struct c2d_src *s, int x, int y, unsigned char out[4])
{
  if (s->kind == C2D_COLOR) {
    memcpy(out, s->col, 4);
    return;
  }
  float px, py;
  c2d_mat_apply(&s->inv, (float)x + 0.5f, (float)y + 0.5f, &px, &py);
  if (s->kind == C2D_PATTERN) {
    sample_image(s, px, py, out);
    return;
  }
  float t;
  if (!gradient_t(s->grad, px, py, &t)) {
    memset(out, 0, 4);
    return;
  }
  gradient_color(s->grad, t < 0 ? 0 : t > 1 ? 1 : t, out);
}

/* ---------------------------------------------------------------- compositing */

static float blend_fn(int comp, float cb, float cs)
{
  switch (comp) {
  case C2D_MULTIPLY:
    return cb * cs;
  case C2D_SCREEN:
    return cb + cs - cb * cs;
  case C2D_OVERLAY:
    return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);
  case C2D_HARD_LIGHT:
    return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);
  case C2D_DARKEN:
    return cb < cs ? cb : cs;
  case C2D_LIGHTEN:
    return cb > cs ? cb : cs;
  case C2D_COLOR_DODGE:
    return cb == 0 ? 0 : cs >= 1 ? 1 : (cb / (1 - cs) > 1 ? 1 : cb / (1 - cs));
  case C2D_COLOR_BURN:
    return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - ((1 - cb) / cs > 1 ? 1 : (1 - cb) / cs);
  case C2D_SOFT_LIGHT: {
    if (cs <= 0.5f)
      return cb - (1 - 2 * cs) * cb * (1 - cb);
    float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : (float)sqrt(cb);
    return cb + (2 * cs - 1) * (d - cb);
  }
  case C2D_DIFFERENCE:
    return cb > cs ? cb - cs : cs - cb;
  case C2D_EXCLUSION:
    return cb + cs - 2 * cb * cs;
  }
  return cs;
}

/* Ops whose result outside the source shape differs from the old pixel (everything transparent-source clears). */
static bool comp_unbounded(int comp)
{
  return comp == C2D_SOURCE_IN || comp == C2D_SOURCE_OUT || comp == C2D_DEST_IN || comp == C2D_DEST_ATOP ||
         comp == C2D_COPY;
}

static unsigned char to8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (unsigned char)(v * 255.0f + 0.5f); }

/* d = op(d, rgb with alpha sa) lerped towards the old pixel by (1 - keep) */
static void blend_generic(unsigned char *d, const unsigned char s[3], float sa, float keep, int comp)
{
  float da = (float)d[3] / 255.0f;
  float cs[3] = {(float)s[0] / 255.0f, (float)s[1] / 255.0f, (float)s[2] / 255.0f};
  float cd[3] = {(float)d[0] / 255.0f, (float)d[1] / 255.0f, (float)d[2] / 255.0f};
  float fs = 1, fd = 1 - sa;
  if (comp >= C2D_MULTIPLY) {
    for (int k = 0; k < 3; k++)
      cs[k] = (1 - da) * cs[k] + da * blend_fn(comp, cd[k], cs[k]);
  } else {
    switch (comp) {
    case C2D_SOURCE_IN:
      fs = da, fd = 0;
      break;
    case C2D_SOURCE_OUT:
      fs = 1 - da, fd = 0;
      break;
    case C2D_SOURCE_ATOP:
      fs = da, fd = 1 - sa;
      break;
    case C2D_DEST_OVER:
      fs = 1 - da, fd = 1;
      break;
    case C2D_DEST_IN:
      fs = 0, fd = sa;
      break;
    case C2D_DEST_OUT:
      fs = 0, fd = 1 - sa;
      break;
    case C2D_DEST_ATOP:
      fs = 1 - da, fd = sa;
      break;
    case C2D_LIGHTER:
      fs = 1, fd = 1;
      break;
    case C2D_COPY:
      fs = 1, fd = 0;
      break;
    case C2D_XOR:
      fs = 1 - da, fd = 1 - sa;
      break;
    }
  }
  float ra = sa * fs + da * fd;
  float rp[3];
  for (int k = 0; k < 3; k++)
    rp[k] = cs[k] * sa * fs + cd[k] * da * fd;
  if (ra > 1)
    ra = 1;
  float fa = da * (1 - keep) + ra * keep;
  if (fa <= 0) {
    d[0] = d[1] = d[2] = d[3] = 0;
    return;
  }
  for (int k = 0; k < 3; k++) {
    float fp = cd[k] * da * (1 - keep) + (rp[k] > ra ? ra : rp[k]) * keep;
    d[k] = to8(fp / fa);
  }
  d[3] = to8(fa);
}

/* Source-over with an integer source alpha a (0..255): the common path. */
static void blend_over(unsigned char *d, const unsigned char s[3], unsigned a)
{
  if (a == 0)
    return;
  if (a == 255 || d[3] == 0) {
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
    d[3] = (unsigned char)a;
    return;
  }
  unsigned da = d[3], ra = a + da * (255 - a) / 255;
  unsigned wd = da * (255 - a);
  unsigned den = a * 255 + wd;
  for (int k = 0; k < 3; k++)
    d[k] = (unsigned char)((s[k] * a * 255 + d[k] * wd + den / 2) / den);
  d[3] = (unsigned char)ra;
}

static void blend_box(struct c2d *c, const unsigned char *cov, int bx, int by, int bw, int bh,
                      const struct c2d_src *src, float alpha, int comp)
{
  const struct c2d_mask *clip = c->st[c->depth].clip;
  unsigned alpha8 = (unsigned)(alpha * 255.0f + 0.5f);
  bool solid = src->kind == C2D_COLOR;
  bool unb = comp_unbounded(comp);
  for (int y = by; y < by + bh; y++) {
    unsigned char *drow = c->px + ((unsigned long)y * c->w + bx) * 4;
    for (int x = bx; x < bx + bw; x++, drow += 4) {
      unsigned cv = cov ? cov[(unsigned long)(y - by) * bw + (x - bx)] : 255;
      if (!cv && !unb)
        continue;
      unsigned ck = clip ? clip->px[(unsigned long)y * c->w + x] : 255;
      if (!ck)
        continue;
      unsigned char s[4];
      if (solid)
        memcpy(s, src->col, 4);
      else
        src_eval(src, x, y, s);
      unsigned sa8 = (s[3] * alpha8 + 127) / 255;
      sa8 = (sa8 * cv + 127) / 255;
      if (comp == C2D_SOURCE_OVER && ck == 255) {
        blend_over(drow, s, sa8);
        continue;
      }
      blend_generic(drow, s, (float)sa8 / 255.0f, (float)ck / 255.0f, comp);
    }
  }
}

/* ---------------------------------------------------------------------- shadow */

static void box_blur(unsigned char *buf, int w, int h, int d)
{
  if (d < 2)
    return;
  int r = d / 2;
  unsigned char *tmp = malloc((unsigned)(w > h ? w : h));
  if (!tmp)
    return;
  for (int pass = 0; pass < 2; pass++) {
    int len = pass ? h : w, lines = pass ? w : h;
    int step = pass ? w : 1, lstep = pass ? 1 : w;
    for (int l = 0; l < lines; l++) {
      unsigned char *line = buf + (unsigned long)l * lstep;
      unsigned sum = 0;
      for (int i = 0; i < r && i < len; i++)
        sum += line[(unsigned long)i * step];
      for (int i = 0; i < len; i++) {
        if (i + r < len)
          sum += line[(unsigned long)(i + r) * step];
        if (i - r - 1 >= 0)
          sum -= line[(unsigned long)(i - r - 1) * step];
        tmp[i] = (unsigned char)(sum / (unsigned)(2 * r + 1));
      }
      for (int i = 0; i < len; i++)
        line[(unsigned long)i * step] = tmp[i];
    }
  }
  free(tmp);
}

static void draw_shadow(struct c2d *c, const unsigned char *cov, int bx, int by, int bw, int bh, float alpha, int comp)
{
  const struct c2d_state *st = &c->st[c->depth];
  float sigma = st->shadow_blur / 2;
  int d = sigma > 0 ? (int)(sigma * 2 + 0.5f) : 0;
  if (d > 0 && !(d & 1))
    d++;
  int pad = d > 0 ? d * 3 / 2 + 1 : 0;
  int ox = (int)floor(st->shadow_ox + 0.5f), oy = (int)floor(st->shadow_oy + 0.5f);
  int x0 = bx + ox - pad, y0 = by + oy - pad, x1 = bx + bw + ox + pad, y1 = by + bh + oy + pad;
  x0 = x0 < 0 ? 0 : x0;
  y0 = y0 < 0 ? 0 : y0;
  x1 = x1 > c->w ? c->w : x1;
  y1 = y1 > c->h ? c->h : y1;
  if (x1 <= x0 || y1 <= y0)
    return;
  int sw = x1 - x0, sh = y1 - y0;
  unsigned char *sb = calloc((unsigned)sw, (unsigned)sh);
  if (!sb)
    return;
  for (int y = 0; y < sh; y++) {
    int cy = y0 + y - oy - by;
    if (cy < 0 || cy >= bh)
      continue;
    for (int x = 0; x < sw; x++) {
      int cx = x0 + x - ox - bx;
      if (cx >= 0 && cx < bw)
        sb[(unsigned long)y * sw + x] = cov ? cov[(unsigned long)cy * bw + cx] : 255;
    }
  }
  for (int i = 0; i < 3; i++)
    box_blur(sb, sw, sh, d);
  struct c2d_src ss;
  unsigned char col[4] = {st->shadow_r, st->shadow_g, st->shadow_b, st->shadow_a};
  c2d_src_color(&ss, col);
  blend_box(c, sb, x0, y0, sw, sh, &ss, alpha, comp);
  free(sb);
}

void c2d_draw_cov(struct c2d *c, unsigned char *cov, int bx, int by, int bw, int bh, const struct c2d_src *src,
                  float alpha, int comp, bool shadow)
{
  const struct c2d_state *st = &c->st[c->depth];
  if (bw <= 0 || bh <= 0)
    return;
  if (shadow && st->shadow_a && (st->shadow_blur > 0 || st->shadow_ox != 0 || st->shadow_oy != 0))
    draw_shadow(c, cov, bx, by, bw, bh, alpha, comp);
  if (comp_unbounded(comp) && (bx != 0 || by != 0 || bw != c->w || bh != c->h)) {
    unsigned char *full = calloc((unsigned)c->w, (unsigned)c->h);
    if (!full)
      return;
    for (int y = 0; y < bh; y++)
      for (int x = 0; x < bw; x++)
        full[(unsigned long)(by + y) * c->w + bx + x] = cov ? cov[(unsigned long)y * bw + x] : 255;
    blend_box(c, full, 0, 0, c->w, c->h, src, alpha, comp);
    free(full);
    return;
  }
  blend_box(c, cov, bx, by, bw, bh, src, alpha, comp);
}
