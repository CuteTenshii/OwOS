/* svg_filter.c: <filter> primitives over a rendered layer (blur, offset, flood, blend, composite, colour effects). */
#include "features/svg/svg_int.h"

#define MAX_RESULTS 16
#define FNAME 40

struct fenv {
  struct ctx *cx;
  const struct dom_node *filter;
  struct mat m;
  struct bb bbox;
  bool pobb, linear;
  int x0, y0, x1, y1; /* filter region in device pixels */
  unsigned *source;
  unsigned *alpha; /* SourceAlpha, built on demand */
  unsigned *blank;
  unsigned *res[MAX_RESULTS];
  char name[MAX_RESULTS][FNAME];
  int nres;
  unsigned *last;
};

static unsigned char to_lin[256], to_srgb[256];

static void init_luts(void)
{
  if (to_lin[255])
    return;
  for (int i = 0; i < 256; i++) {
    double c = i / 255.0;
    double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    to_lin[i] = (unsigned char)(l * 255.0 + 0.5);
    double s = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    to_srgb[i] = (unsigned char)(s * 255.0 + 0.5);
  }
}

static unsigned pack(float r, float g, float b, float a)
{
  a = a < 0 ? 0 : a > 1 ? 1 : a;
  r = r < 0 ? 0 : r > 1 ? 1 : r;
  g = g < 0 ? 0 : g > 1 ? 1 : g;
  b = b < 0 ? 0 : b > 1 ? 1 : b;
  return ((unsigned)(a * 255 + 0.5f) << 24) | ((unsigned)(r * a * 255 + 0.5f) << 16) |
         ((unsigned)(g * a * 255 + 0.5f) << 8) | (unsigned)(b * a * 255 + 0.5f);
}

/* Premultiplied pixel to straight floats. */
static void unpack(unsigned p, float *r, float *g, float *b, float *a)
{
  float al = (float)(p >> 24) / 255.0f;
  float inv = al > 0 ? 1.0f / (al * 255.0f) : 0;
  *a = al;
  *r = (float)((p >> 16) & 255) * inv;
  *g = (float)((p >> 8) & 255) * inv;
  *b = (float)(p & 255) * inv;
}

static void convert(unsigned *px, int n, const unsigned char *lut)
{
  for (int i = 0; i < n; i++) {
    unsigned p = px[i], a = p >> 24;
    if (!a)
      continue;
    unsigned r = (((p >> 16) & 255) * 255 + a / 2) / a, g = (((p >> 8) & 255) * 255 + a / 2) / a,
             b = ((p & 255) * 255 + a / 2) / a;
    r = lut[r > 255 ? 255 : r] * a / 255;
    g = lut[g > 255 ? 255 : g] * a / 255;
    b = lut[b > 255 ? 255 : b] * a / 255;
    px[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
}

static unsigned *fnew(const struct fenv *e)
{
  return calloc((unsigned)e->cx->w * (unsigned)e->cx->h, sizeof(unsigned));
}

static unsigned *fdup(const struct fenv *e, const unsigned *src)
{
  unsigned *d = fnew(e);
  if (d)
    memcpy(d, src, (unsigned)e->cx->w * (unsigned)e->cx->h * sizeof(unsigned));
  return d;
}

/* Input named by in="..."; falls back to the previous result (or the source for the first primitive). */
static const unsigned *input(struct fenv *e, const char *name)
{
  if (name && *name) {
    if (streq(name, "SourceGraphic"))
      return e->source;
    if (streq(name, "SourceAlpha")) {
      if (!e->alpha) {
        e->alpha = fdup(e, e->source);
        for (int i = 0; e->alpha && i < e->cx->w * e->cx->h; i++)
          e->alpha[i] &= 0xFF000000u;
      }
      return e->alpha ? e->alpha : e->blank;
    }
    if (starts_ci(name, "Background") || starts_ci(name, "Fill") || starts_ci(name, "Stroke"))
      return e->blank;
    for (int i = e->nres - 1; i >= 0; i--)
      if (streq(e->name[i], name))
        return e->res[i];
  }
  return e->last ? e->last : e->source;
}

static float prim_num(const struct dom_node *n, const char *name, float def)
{
  const char *v = dom_get_attr(n, name);
  float f;
  return v && svg_parse_num(&v, v + strlen(v), &f) ? f : def;
}

/* Two-number attribute ("sx [sy]"). */
static void prim_pair(const struct dom_node *n, const char *name, float def, float *a, float *b)
{
  const char *v = dom_get_attr(n, name);
  *a = *b = def;
  if (!v)
    return;
  const char *e = v + strlen(v);
  float x;
  if (svg_parse_num(&v, e, &x)) {
    *a = *b = x;
    if (svg_parse_num(&v, e, &x))
      *b = x;
  }
}

/* One box-blur pass along rows (horiz) or columns: window [i - lo, i - lo + size). */
static void box_pass(const unsigned *src, unsigned *dst, int w, int h, bool horiz, int size, int lo)
{
  int len = horiz ? w : h, lines = horiz ? h : w;
  for (int l = 0; l < lines; l++) {
    unsigned s[4] = {0, 0, 0, 0};
    int step = horiz ? 1 : w, base = horiz ? l * w : l;
    for (int i = -lo; i < size - lo; i++)
      if (i >= 0 && i < len) {
        unsigned p = src[base + i * step];
        s[0] += p >> 24;
        s[1] += (p >> 16) & 255;
        s[2] += (p >> 8) & 255;
        s[3] += p & 255;
      }
    for (int i = 0; i < len; i++) {
      dst[base + i * step] = ((s[0] / (unsigned)size) << 24) | ((s[1] / (unsigned)size) << 16) |
                             ((s[2] / (unsigned)size) << 8) | (s[3] / (unsigned)size);
      int out = i - lo, in = i - lo + size;
      if (out >= 0 && out < len) {
        unsigned p = src[base + out * step];
        s[0] -= p >> 24;
        s[1] -= (p >> 16) & 255;
        s[2] -= (p >> 8) & 255;
        s[3] -= p & 255;
      }
      if (in >= 0 && in < len) {
        unsigned p = src[base + in * step];
        s[0] += p >> 24;
        s[1] += (p >> 16) & 255;
        s[2] += (p >> 8) & 255;
        s[3] += p & 255;
      }
    }
  }
}

/* Gaussian approximated by three box blurs, as the filter spec describes. */
static void blur_axis(unsigned *buf, unsigned *tmp, int w, int h, bool horiz, float sigma)
{
  if (!(sigma > 0))
    return;
  int d = (int)(sigma * 3.0f * 2.5066283f / 4.0f + 0.5f);
  if (d < 1)
    return;
  if (d > 4096)
    d = 4096;
  if (d & 1) {
    box_pass(buf, tmp, w, h, horiz, d, d / 2);
    box_pass(tmp, buf, w, h, horiz, d, d / 2);
    box_pass(buf, tmp, w, h, horiz, d, d / 2);
  } else {
    box_pass(buf, tmp, w, h, horiz, d, d / 2);
    box_pass(tmp, buf, w, h, horiz, d, d / 2 - 1);
    box_pass(buf, tmp, w, h, horiz, d + 1, d / 2);
  }
  memcpy(buf, tmp, (unsigned)w * (unsigned)h * sizeof(unsigned));
}

/* Device-space standard deviation along x and y for user-space values. */
static void device_sigma(const struct fenv *e, float sx, float sy, float *dx, float *dy)
{
  if (e->pobb) {
    sx *= e->bbox.x1 - e->bbox.x0;
    sy *= e->bbox.y1 - e->bbox.y0;
  }
  *dx = sx * (float)sqrt((double)(e->m.a * e->m.a + e->m.b * e->m.b));
  *dy = sy * (float)sqrt((double)(e->m.c * e->m.c + e->m.d * e->m.d));
}

static unsigned *blur(struct fenv *e, const unsigned *in, float sx, float sy)
{
  unsigned *out = fdup(e, in), *tmp = fnew(e);
  if (out && tmp) {
    float dx, dy;
    device_sigma(e, sx, sy, &dx, &dy);
    blur_axis(out, tmp, e->cx->w, e->cx->h, true, dx);
    blur_axis(out, tmp, e->cx->w, e->cx->h, false, dy);
  }
  free(tmp);
  return out;
}

static unsigned *offset(struct fenv *e, const unsigned *in, float dx, float dy)
{
  unsigned *out = fnew(e);
  if (!out)
    return 0;
  if (e->pobb) {
    dx *= e->bbox.x1 - e->bbox.x0;
    dy *= e->bbox.y1 - e->bbox.y0;
  }
  int ox = (int)floor((double)(e->m.a * dx + e->m.c * dy) + 0.5),
      oy = (int)floor((double)(e->m.b * dx + e->m.d * dy) + 0.5);
  int w = e->cx->w, h = e->cx->h;
  for (int y = 0; y < h; y++) {
    int sy = y - oy;
    if (sy < 0 || sy >= h)
      continue;
    for (int x = 0; x < w; x++) {
      int sx = x - ox;
      if (sx >= 0 && sx < w)
        out[y * w + x] = in[sy * w + sx];
    }
  }
  return out;
}

static unsigned over(unsigned top, unsigned bot)
{
  unsigned ia = 255 - (top >> 24);
  unsigned a = (top >> 24) + ((bot >> 24) * ia + 127) / 255,
           r = ((top >> 16) & 255) + (((bot >> 16) & 255) * ia + 127) / 255,
           g = ((top >> 8) & 255) + (((bot >> 8) & 255) * ia + 127) / 255,
           b = (top & 255) + ((bot & 255) * ia + 127) / 255;
  a = a > 255 ? 255 : a;
  r = r > a ? a : r;
  g = g > a ? a : g;
  b = b > a ? a : b;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Flood colour in the working colour space; false if the primitive paints nothing. */
static unsigned flood_pixel(struct fenv *e, const struct dom_node *p, const char *cname, const char *oname)
{
  char b[64];
  int rgb = 0, al = 255, a;
  if (svg_get_prop(e->cx, p, cname, b, sizeof b)) {
    if (starts_ci(b, "currentcolor")) {
      int cc = svg_get_prop(e->cx, p, "color", b, sizeof b) ? color_parse_a(b, &a) : -1;
      rgb = cc >= 0 ? cc : 0;
    } else {
      int c = color_parse_a(b, &a);
      if (c >= 0) {
        rgb = c;
        al = a;
      }
    }
  }
  float op = svg_get_prop(e->cx, p, oname, b, sizeof b) ? svg_parse_unit(b, 1.0f) : 1.0f;
  float r = (float)((rgb >> 16) & 255) / 255, g = (float)((rgb >> 8) & 255) / 255, bl = (float)(rgb & 255) / 255;
  if (e->linear) {
    r = (float)to_lin[(rgb >> 16) & 255] / 255;
    g = (float)to_lin[(rgb >> 8) & 255] / 255;
    bl = (float)to_lin[rgb & 255] / 255;
  }
  return pack(r, g, bl, op * (float)al / 255.0f);
}

static void blend_px(const char *mode, unsigned top, unsigned bot, unsigned *out)
{
  float ar, ag, ab, aa, br, bg, bb, ba;
  unpack(top, &ar, &ag, &ab, &aa);
  unpack(bot, &br, &bg, &bb, &ba);
  ar *= aa;
  ag *= aa;
  ab *= aa;
  br *= ba;
  bg *= ba;
  bb *= ba;
  float c[3], qa = aa, qb = ba, a[3] = {ar, ag, ab}, b[3] = {br, bg, bb};
  for (int i = 0; i < 3; i++) {
    if (starts_ci(mode, "multiply"))
      c[i] = (1 - qa) * b[i] + (1 - qb) * a[i] + a[i] * b[i];
    else if (starts_ci(mode, "screen"))
      c[i] = b[i] + a[i] - a[i] * b[i];
    else if (starts_ci(mode, "darken")) {
      float x = (1 - qa) * b[i] + a[i], y = (1 - qb) * a[i] + b[i];
      c[i] = x < y ? x : y;
    } else if (starts_ci(mode, "lighten")) {
      float x = (1 - qa) * b[i] + a[i], y = (1 - qb) * a[i] + b[i];
      c[i] = x > y ? x : y;
    } else
      c[i] = (1 - qa) * b[i] + a[i];
  }
  float ra = 1 - (1 - qa) * (1 - qb);
  float pr = ra > 0 ? c[0] / ra : 0, pg = ra > 0 ? c[1] / ra : 0, pb = ra > 0 ? c[2] / ra : 0;
  *out = pack(pr, pg, pb, ra);
}

static unsigned *composite(struct fenv *e, const struct dom_node *p, const unsigned *a, const unsigned *b)
{
  unsigned *out = fnew(e);
  if (!out)
    return 0;
  const char *op = dom_get_attr(p, "operator");
  float k1 = prim_num(p, "k1", 0), k2 = prim_num(p, "k2", 0), k3 = prim_num(p, "k3", 0), k4 = prim_num(p, "k4", 0);
  int n = e->cx->w * e->cx->h;
  for (int i = 0; i < n; i++) {
    unsigned pa = a[i], pb = b[i];
    float fa = (float)(pa >> 24) / 255, fb = (float)(pb >> 24) / 255;
    if (op && starts_ci(op, "arithmetic")) {
      float ch[4];
      for (int k = 0; k < 4; k++) {
        int sh = k == 0 ? 24 : 16 - 8 * (k - 1);
        float x = (float)((pa >> sh) & 255) / 255, y = (float)((pb >> sh) & 255) / 255;
        float v = k1 * x * y + k2 * x + k3 * y + k4;
        ch[k] = v < 0 ? 0 : v > 1 ? 1 : v;
      }
      float al = ch[0];
      for (int k = 1; k < 4; k++)
        if (ch[k] > al)
          ch[k] = al;
      out[i] = ((unsigned)(al * 255 + 0.5f) << 24) | ((unsigned)(ch[1] * 255 + 0.5f) << 16) |
               ((unsigned)(ch[2] * 255 + 0.5f) << 8) | (unsigned)(ch[3] * 255 + 0.5f);
      continue;
    }
    float fa_k, fb_k; /* weights of A and B */
    if (op && starts_ci(op, "in")) {
      fa_k = fb;
      fb_k = 0;
    } else if (op && starts_ci(op, "out")) {
      fa_k = 1 - fb;
      fb_k = 0;
    } else if (op && starts_ci(op, "atop")) {
      fa_k = fb;
      fb_k = 1 - fa;
    } else if (op && starts_ci(op, "xor")) {
      fa_k = 1 - fb;
      fb_k = 1 - fa;
    } else {
      fa_k = 1;
      fb_k = 1 - fa;
    }
    unsigned v = 0;
    for (int sh = 24; sh >= 0; sh -= 8) {
      float x = (float)((pa >> sh) & 255) * fa_k + (float)((pb >> sh) & 255) * fb_k;
      v |= (unsigned)(x > 255 ? 255 : x + 0.5f) << sh;
    }
    out[i] = v;
  }
  return out;
}

static unsigned *color_matrix(struct fenv *e, const struct dom_node *p, const unsigned *in)
{
  unsigned *out = fnew(e);
  if (!out)
    return 0;
  const char *type = dom_get_attr(p, "type"), *vals = dom_get_attr(p, "values");
  float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
  float v[20];
  int nv = 0;
  if (vals) {
    const char *q = vals, *qe = vals + strlen(vals);
    while (nv < 20 && svg_parse_num(&q, qe, &v[nv]))
      nv++;
  }
  if (type && starts_ci(type, "saturate")) {
    float s = nv ? v[0] : 1;
    float t[20] = {0.213f + 0.787f * s,
                   0.715f - 0.715f * s,
                   0.072f - 0.072f * s,
                   0,
                   0,
                   0.213f - 0.213f * s,
                   0.715f + 0.285f * s,
                   0.072f - 0.072f * s,
                   0,
                   0,
                   0.213f - 0.213f * s,
                   0.715f - 0.715f * s,
                   0.072f + 0.928f * s,
                   0,
                   0,
                   0,
                   0,
                   0,
                   1,
                   0};
    memcpy(m, t, sizeof m);
  } else if (type && starts_ci(type, "hueRotate")) {
    float rad = (nv ? v[0] : 0) * SVG_PI / 180.0f, c = (float)cos((double)rad), s = (float)sin((double)rad);
    float t[20] = {0.213f + c * 0.787f - s * 0.213f,
                   0.715f - c * 0.715f - s * 0.715f,
                   0.072f - c * 0.072f + s * 0.928f,
                   0,
                   0,
                   0.213f - c * 0.213f + s * 0.143f,
                   0.715f + c * 0.285f + s * 0.140f,
                   0.072f - c * 0.072f - s * 0.283f,
                   0,
                   0,
                   0.213f - c * 0.213f - s * 0.787f,
                   0.715f - c * 0.715f + s * 0.715f,
                   0.072f + c * 0.928f + s * 0.072f,
                   0,
                   0,
                   0,
                   0,
                   0,
                   1,
                   0};
    memcpy(m, t, sizeof m);
  } else if (type && starts_ci(type, "luminanceToAlpha")) {
    float t[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0};
    memcpy(m, t, sizeof m);
  } else if (nv == 20) {
    memcpy(m, v, sizeof m);
  }
  int n = e->cx->w * e->cx->h;
  for (int i = 0; i < n; i++) {
    float r, g, b, a;
    unpack(in[i], &r, &g, &b, &a);
    out[i] = pack(m[0] * r + m[1] * g + m[2] * b + m[3] * a + m[4], m[5] * r + m[6] * g + m[7] * b + m[8] * a + m[9],
                  m[10] * r + m[11] * g + m[12] * b + m[13] * a + m[14],
                  m[15] * r + m[16] * g + m[17] * b + m[18] * a + m[19]);
  }
  return out;
}

static float transfer(const struct dom_node *fn, float c)
{
  if (!fn)
    return c;
  const char *type = dom_get_attr(fn, "type");
  if (!type)
    return c;
  if (starts_ci(type, "linear"))
    return prim_num(fn, "slope", 1) * c + prim_num(fn, "intercept", 0);
  if (starts_ci(type, "gamma"))
    return prim_num(fn, "amplitude", 1) * (float)pow((double)c, (double)prim_num(fn, "exponent", 1)) +
           prim_num(fn, "offset", 0);
  if (starts_ci(type, "table") || starts_ci(type, "discrete")) {
    const char *tv = dom_get_attr(fn, "tablevalues");
    float t[32];
    int nt = 0;
    if (tv) {
      const char *tvend = tv + strlen(tv);
      while (nt < 32 && svg_parse_num(&tv, tvend, &t[nt]))
        nt++;
    }
    if (!nt)
      return c;
    if (starts_ci(type, "discrete")) {
      int k = (int)(c * (float)nt);
      return t[k >= nt ? nt - 1 : k];
    }
    if (nt == 1)
      return t[0];
    float pos = c * (float)(nt - 1);
    int k = (int)pos;
    if (k >= nt - 1)
      return t[nt - 1];
    return t[k] + (pos - (float)k) * (t[k + 1] - t[k]);
  }
  return c;
}

static unsigned *component_transfer(struct fenv *e, const struct dom_node *p, const unsigned *in)
{
  unsigned *out = fnew(e);
  if (!out)
    return 0;
  const struct dom_node *fn[4] = {0, 0, 0, 0};
  for (const struct dom_node *c = p->first_child; c; c = c->next)
    if (c->type == DOM_ELEMENT && starts_ci(c->tag, "fefunc") && c->tag[6] && !c->tag[7]) {
      char ch = (char)(c->tag[6] | 32);
      fn[ch == 'r' ? 0 : ch == 'g' ? 1 : ch == 'b' ? 2 : 3] = c;
    }
  int n = e->cx->w * e->cx->h;
  for (int i = 0; i < n; i++) {
    float r, g, b, a;
    unpack(in[i], &r, &g, &b, &a);
    out[i] = pack(transfer(fn[0], r), transfer(fn[1], g), transfer(fn[2], b), transfer(fn[3], a));
  }
  return out;
}

static unsigned *morphology(struct fenv *e, const struct dom_node *p, const unsigned *in)
{
  unsigned *out = fdup(e, in), *tmp = fnew(e);
  if (!out || !tmp) {
    free(out);
    free(tmp);
    return 0;
  }
  const char *op = dom_get_attr(p, "operator");
  bool dilate = op && starts_ci(op, "dilate");
  float rx, ry;
  prim_pair(p, "radius", 0, &rx, &ry);
  int w = e->cx->w, h = e->cx->h;
  float drx, dry;
  device_sigma(e, rx, ry, &drx, &dry);
  int r[2] = {(int)(drx + 0.5f), (int)(dry + 0.5f)};
  for (int axis = 0; axis < 2; axis++) {
    int rad = r[axis] > 64 ? 64 : r[axis];
    if (rad <= 0)
      continue;
    int len = axis ? h : w, lines = axis ? w : h, step = axis ? w : 1;
    for (int l = 0; l < lines; l++) {
      int base = axis ? l : l * w;
      for (int i = 0; i < len; i++) {
        unsigned best[4] = {dilate ? 0u : 255u, dilate ? 0u : 255u, dilate ? 0u : 255u, dilate ? 0u : 255u};
        for (int k = i - rad; k <= i + rad; k++) {
          unsigned px = k >= 0 && k < len ? out[base + k * step] : 0;
          for (int c = 0; c < 4; c++) {
            unsigned v = (px >> (24 - 8 * c)) & 255;
            if (dilate ? v > best[c] : v < best[c])
              best[c] = v;
          }
        }
        tmp[base + i * step] = (best[0] << 24) | (best[1] << 16) | (best[2] << 8) | best[3];
      }
    }
    memcpy(out, tmp, (unsigned)w * (unsigned)h * sizeof(unsigned));
  }
  free(tmp);
  return out;
}

/* feTurbulence: Perlin noise as in the filter specification's reference code. */
#define BSIZE 0x100
#define BM 0xff
#define PN 0x1000
struct turb {
  int lattice[BSIZE + BSIZE + 2];
  double grad[4][BSIZE + BSIZE + 2][2];
};

static long turb_random(long seed)
{
  long result = 16807 * (seed % 127773) - 2836 * (seed / 127773);
  return result <= 0 ? result + 2147483647 : result;
}

static void turb_init(struct turb *t, long seed)
{
  if (seed <= 0)
    seed = -(seed % 2147483646) + 1;
  if (seed > 2146483646)
    seed = 2147483646;
  for (int k = 0; k < 4; k++)
    for (int i = 0; i < BSIZE; i++) {
      t->lattice[i] = i;
      for (int j = 0; j < 2; j++) {
        seed = turb_random(seed);
        t->grad[k][i][j] = (double)((seed % (BSIZE + BSIZE)) - BSIZE) / BSIZE;
      }
      double s = sqrt(t->grad[k][i][0] * t->grad[k][i][0] + t->grad[k][i][1] * t->grad[k][i][1]);
      if (s > 0) {
        t->grad[k][i][0] /= s;
        t->grad[k][i][1] /= s;
      }
    }
  for (int i = BSIZE - 1; i > 0; i--) {
    int k = t->lattice[i];
    seed = turb_random(seed);
    int j = (int)(seed % BSIZE);
    t->lattice[i] = t->lattice[j];
    t->lattice[j] = k;
  }
  for (int i = 0; i < BSIZE + 2; i++) {
    t->lattice[BSIZE + i] = t->lattice[i];
    for (int k = 0; k < 4; k++)
      for (int j = 0; j < 2; j++)
        t->grad[k][BSIZE + i][j] = t->grad[k][i][j];
  }
}

static double turb_noise(const struct turb *t, int ch, double vx, double vy)
{
  double tt = vx + PN;
  int bx0 = ((int)tt) & BM, bx1 = (bx0 + 1) & BM;
  double rx0 = tt - (int)tt, rx1 = rx0 - 1.0;
  tt = vy + PN;
  int by0 = ((int)tt) & BM, by1 = (by0 + 1) & BM;
  double ry0 = tt - (int)tt, ry1 = ry0 - 1.0;
  int i = t->lattice[bx0], j = t->lattice[bx1];
  int b00 = t->lattice[i + by0], b10 = t->lattice[j + by0], b01 = t->lattice[i + by1], b11 = t->lattice[j + by1];
  double sx = rx0 * rx0 * (3. - 2. * rx0), sy = ry0 * ry0 * (3. - 2. * ry0);
  const double (*g)[2] = t->grad[ch];
  double u = rx0 * g[b00][0] + ry0 * g[b00][1], v = rx1 * g[b10][0] + ry0 * g[b10][1];
  double a = u + sx * (v - u);
  u = rx0 * g[b01][0] + ry1 * g[b01][1];
  v = rx1 * g[b11][0] + ry1 * g[b11][1];
  double b = u + sx * (v - u);
  return a + sy * (b - a);
}

static unsigned *turbulence(struct fenv *e, const struct dom_node *p)
{
  unsigned *out = fnew(e);
  struct turb *t = malloc(sizeof *t);
  struct mat inv;
  if (!out || !t || !svg_mat_inv(e->m, &inv)) {
    free(t);
    return out;
  }
  float fx, fy;
  prim_pair(p, "basefrequency", 0, &fx, &fy);
  int octaves = (int)prim_num(p, "numoctaves", 1);
  octaves = octaves < 0 ? 0 : octaves > 8 ? 8 : octaves;
  const char *type = dom_get_attr(p, "type");
  bool fractal = type && starts_ci(type, "fractalNoise");
  turb_init(t, (long)prim_num(p, "seed", 0));
  int w = e->cx->w;
  for (int y = e->y0; y < e->y1; y++)
    for (int x = e->x0; x < e->x1; x++) {
      float ux, uy;
      svg_mat_apply(inv, (float)x, (float)y, &ux, &uy);
      float ch[4];
      for (int c = 0; c < 4; c++) {
        double sum = 0, ratio = 1;
        double px = ux * fx, py = uy * fy;
        for (int o = 0; o < octaves; o++) {
          double n = turb_noise(t, c, px, py);
          sum += (fractal ? n : (n < 0 ? -n : n)) / ratio;
          px *= 2;
          py *= 2;
          ratio *= 2;
        }
        ch[c] = (float)(fractal ? (sum + 1) / 2 : sum);
      }
      out[y * w + x] = pack(ch[0], ch[1], ch[2], ch[3]);
    }
  free(t);
  return out;
}

/* Primitive subregion in device pixels, defaulting to the filter region. */
static void subregion(const struct fenv *e, const struct dom_node *p, int r[4])
{
  r[0] = e->x0;
  r[1] = e->y0;
  r[2] = e->x1;
  r[3] = e->y1;
  if (!dom_get_attr(p, "x") && !dom_get_attr(p, "y") && !dom_get_attr(p, "width") && !dom_get_attr(p, "height"))
    return;
  struct ctx *cx = e->cx;
  float bw = e->bbox.x1 - e->bbox.x0, bh = e->bbox.y1 - e->bbox.y0;
  struct bb fr = {0, 0, 0, 0, false};
  float c[4][2] = {{(float)e->x0, (float)e->y0},
                   {(float)e->x1, (float)e->y0},
                   {(float)e->x1, (float)e->y1},
                   {(float)e->x0, (float)e->y1}};
  struct mat inv;
  if (!svg_mat_inv(e->m, &inv))
    return;
  for (int i = 0; i < 4; i++) {
    float ux, uy;
    svg_mat_apply(inv, c[i][0], c[i][1], &ux, &uy);
    svg_bb_add(&fr, ux, uy);
  }
  float x = fr.x0, y = fr.y0, w = fr.x1 - fr.x0, h = fr.y1 - fr.y0;
  if (dom_get_attr(p, "x"))
    x = svg_grad_coord(dom_get_attr(p, "x"), x, e->pobb, cx->vw) * (e->pobb ? bw : 1) + (e->pobb ? e->bbox.x0 : 0);
  if (dom_get_attr(p, "y"))
    y = svg_grad_coord(dom_get_attr(p, "y"), y, e->pobb, cx->vh) * (e->pobb ? bh : 1) + (e->pobb ? e->bbox.y0 : 0);
  if (dom_get_attr(p, "width"))
    w = svg_grad_coord(dom_get_attr(p, "width"), w, e->pobb, cx->vw) * (e->pobb ? bw : 1);
  if (dom_get_attr(p, "height"))
    h = svg_grad_coord(dom_get_attr(p, "height"), h, e->pobb, cx->vh) * (e->pobb ? bh : 1);
  struct bb d = {0, 0, 0, 0, false};
  float q[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
  for (int i = 0; i < 4; i++) {
    float dx, dy;
    svg_mat_apply(e->m, q[i][0], q[i][1], &dx, &dy);
    svg_bb_add(&d, dx, dy);
  }
  int nx0 = (int)floor((double)d.x0 + 0.5), ny0 = (int)floor((double)d.y0 + 0.5);
  int nx1 = (int)floor((double)d.x1 + 0.5), ny1 = (int)floor((double)d.y1 + 0.5);
  r[0] = nx0 > r[0] ? nx0 : r[0];
  r[1] = ny0 > r[1] ? ny0 : r[1];
  r[2] = nx1 < r[2] ? nx1 : r[2];
  r[3] = ny1 < r[3] ? ny1 : r[3];
}

static void clear_outside(const struct fenv *e, unsigned *buf, const int r[4])
{
  int w = e->cx->w, h = e->cx->h;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (x < r[0] || x >= r[2] || y < r[1] || y >= r[3])
        buf[y * w + x] = 0;
}

static unsigned *run_primitive(struct fenv *e, const struct dom_node *p)
{
  const char *t = p->tag;
  const unsigned *a = input(e, dom_get_attr(p, "in"));
  int n = e->cx->w * e->cx->h;
  if (streq(t, "fegaussianblur")) {
    float sx, sy;
    prim_pair(p, "stddeviation", 0, &sx, &sy);
    return blur(e, a, sx, sy);
  }
  if (streq(t, "feoffset"))
    return offset(e, a, prim_num(p, "dx", 0), prim_num(p, "dy", 0));
  if (streq(t, "feflood")) {
    unsigned *out = fnew(e);
    unsigned px = flood_pixel(e, p, "flood-color", "flood-opacity");
    for (int i = 0; out && i < n; i++)
      out[i] = px;
    return out;
  }
  if (streq(t, "feblend")) {
    unsigned *out = fnew(e);
    const unsigned *b = input(e, dom_get_attr(p, "in2"));
    const char *mode = dom_get_attr(p, "mode");
    for (int i = 0; out && i < n; i++)
      blend_px(mode ? mode : "normal", a[i], b[i], &out[i]);
    return out;
  }
  if (streq(t, "fecomposite"))
    return composite(e, p, a, input(e, dom_get_attr(p, "in2")));
  if (streq(t, "fecolormatrix"))
    return color_matrix(e, p, a);
  if (streq(t, "fecomponenttransfer"))
    return component_transfer(e, p, a);
  if (streq(t, "femorphology"))
    return morphology(e, p, a);
  if (streq(t, "feturbulence"))
    return turbulence(e, p);
  if (streq(t, "femerge")) {
    unsigned *out = fnew(e);
    for (const struct dom_node *c = p->first_child; out && c; c = c->next)
      if (c->type == DOM_ELEMENT && streq(c->tag, "femergenode")) {
        const unsigned *src = input(e, dom_get_attr(c, "in"));
        for (int i = 0; i < n; i++)
          out[i] = over(src[i], out[i]);
      }
    return out;
  }
  if (streq(t, "fedropshadow")) {
    float sx, sy;
    prim_pair(p, "stddeviation", 2, &sx, &sy);
    unsigned *al = fdup(e, a);
    for (int i = 0; al && i < n; i++)
      al[i] &= 0xFF000000u;
    unsigned *sh = al ? blur(e, al, sx, sy) : 0;
    free(al);
    unsigned *off = sh ? offset(e, sh, prim_num(p, "dx", 2), prim_num(p, "dy", 2)) : 0;
    free(sh);
    unsigned px = flood_pixel(e, p, "flood-color", "flood-opacity");
    for (int i = 0; off && i < n; i++) {
      unsigned k = off[i] >> 24;
      float pa = (float)(px >> 24) / 255 * (float)k / 255;
      float cr = (float)((px >> 16) & 255), cg = (float)((px >> 8) & 255), cb = (float)(px & 255);
      float inv = (px >> 24) ? 255.0f / (float)(px >> 24) : 0;
      off[i] = pack(cr * inv / 255, cg * inv / 255, cb * inv / 255, pa);
      off[i] = over(a[i], off[i]);
    }
    return off;
  }
  if (streq(t, "feimage"))
    return fnew(e);
  return fdup(e, a); /* feTile, lighting, convolution, displacement: input passes through */
}

/* Run the filter chain over `layer` (premultiplied sRGB, full canvas) in place. */
bool svg_apply_filter(struct ctx *cx, const struct dom_node *f, unsigned *layer, struct mat m,
                      const struct dom_node *target)
{
  init_luts();
  struct fenv e;
  memset(&e, 0, sizeof e);
  e.cx = cx;
  e.filter = f;
  e.m = m;
  const char *fu = dom_get_attr(f, "filterunits"), *pu = dom_get_attr(f, "primitiveunits");
  bool obb = !(fu && starts_ci(fu, "userSpaceOnUse"));
  e.pobb = pu && starts_ci(pu, "objectBoundingBox");
  struct bb b = {0, 0, 0, 0, false};
  int n = cx->w * cx->h;
  if (obb || e.pobb) {
    svg_node_bbox(cx, target, svg_mat_id(), &b, 0);
    if (!b.ok || b.x1 <= b.x0 || b.y1 <= b.y0) {
      memset(layer, 0, (unsigned)n * sizeof(unsigned));
      return true;
    }
  }
  e.bbox = b;
  float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
  float x = svg_grad_coord(dom_get_attr(f, "x"), obb ? -0.1f : -0.1f * cx->vw, obb, cx->vw);
  float y = svg_grad_coord(dom_get_attr(f, "y"), obb ? -0.1f : -0.1f * cx->vh, obb, cx->vh);
  float w = svg_grad_coord(dom_get_attr(f, "width"), obb ? 1.2f : 1.2f * cx->vw, obb, cx->vw);
  float h = svg_grad_coord(dom_get_attr(f, "height"), obb ? 1.2f : 1.2f * cx->vh, obb, cx->vh);
  if (obb) {
    x = b.x0 + x * bw;
    y = b.y0 + y * bh;
    w *= bw;
    h *= bh;
  }
  struct bb d = {0, 0, 0, 0, false};
  float q[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
  for (int i = 0; i < 4; i++) {
    float dx, dy;
    svg_mat_apply(m, q[i][0], q[i][1], &dx, &dy);
    svg_bb_add(&d, dx, dy);
  }
  e.x0 = d.x0 < 0 ? 0 : (int)floor((double)d.x0 + 0.5);
  e.y0 = d.y0 < 0 ? 0 : (int)floor((double)d.y0 + 0.5);
  e.x1 = d.x1 > (float)cx->w ? cx->w : (int)floor((double)d.x1 + 0.5);
  e.y1 = d.y1 > (float)cx->h ? cx->h : (int)floor((double)d.y1 + 0.5);
  if (!(w > 0 && h > 0) || e.x1 <= e.x0 || e.y1 <= e.y0) {
    memset(layer, 0, (unsigned)n * sizeof(unsigned));
    return true;
  }

  char cs[24];
  e.linear = !(svg_get_prop(cx, f, "color-interpolation-filters", cs, sizeof cs) && starts_ci(cs, "srgb"));
  e.source = fdup(&e, layer);
  e.blank = fnew(&e);
  if (!e.source || !e.blank) {
    free(e.source);
    free(e.blank);
    return false;
  }
  int region[4] = {e.x0, e.y0, e.x1, e.y1};
  clear_outside(&e, e.source, region);
  if (e.linear)
    convert(e.source, n, to_lin);

  for (const struct dom_node *p = f->first_child; p && e.nres < MAX_RESULTS; p = p->next) {
    if (p->type != DOM_ELEMENT || !starts_ci(p->tag, "fe"))
      continue;
    unsigned *out = run_primitive(&e, p);
    if (!out)
      break;
    int sr[4];
    subregion(&e, p, sr);
    clear_outside(&e, out, sr);
    const char *rn = dom_get_attr(p, "result");
    e.res[e.nres] = out;
    strlcpy(e.name[e.nres], rn ? rn : "", FNAME);
    e.nres++;
    e.last = out;
  }

  if (e.last) {
    if (e.linear)
      convert(e.last, n, to_srgb);
    memcpy(layer, e.last, (unsigned)n * sizeof(unsigned));
  } else {
    memcpy(layer, e.source, (unsigned)n * sizeof(unsigned));
    if (e.linear)
      convert(layer, n, to_srgb);
  }
  clear_outside(&e, layer, region);
  for (int i = 0; i < e.nres; i++)
    free(e.res[i]);
  free(e.source);
  free(e.alpha);
  free(e.blank);
  return true;
}
