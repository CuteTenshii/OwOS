/* svg_paint.c: gradients and paint sources. */
#include "features/svg/svg_int.h"

/* Attribute looked up along the xlink:href chain of a gradient. */
const char *svg_chain_attr(const struct ctx *cx, const struct dom_node *g, const char *name, bool same_type)
{
  const char *tag = g->tag;
  for (int hop = 0; g && hop < 8; hop++) {
    const char *v = dom_get_attr(g, name);
    if (v && (!same_type || streq(g->tag, tag)))
      return v;
    g = svg_href_target(cx, g);
  }
  return 0;
}

float svg_grad_coord(const char *v, float def, bool obb, float ref)
{
  if (!v)
    return def;
  float out;
  if (obb) {
    const char *q = v;
    if (!svg_parse_num(&q, v + strlen(v), &out))
      return def;
    return *q == '%' ? out / 100.0f : out;
  }
  return svg_parse_len(v, ref, &out) ? out : def;
}

static int collect_stops(const struct ctx *cx, const struct dom_node *g, struct stop *st)
{
  for (int hop = 0; g && hop < 8; hop++, g = svg_href_target(cx, g)) {
    int n = 0;
    float last = 0;
    for (const struct dom_node *c = g->first_child; c && n < MAX_STOPS; c = c->next) {
      if (c->type != DOM_ELEMENT || !streq(c->tag, "stop"))
        continue;
      char b[64];
      const char *ov = dom_get_attr(c, "offset");
      float off = ov ? svg_grad_coord(ov, 0, true, 1) : 0;
      off = off < last ? last : off > 1 ? 1 : off;
      last = off;
      int rgb = 0, alpha = 255, a;
      if (svg_get_prop(cx, c, "stop-color", b, sizeof b)) {
        char cb[64];
        if (starts_ci(b, "currentcolor")) {
          int ca, cc = svg_get_prop(cx, c, "color", cb, sizeof cb) ? color_parse_a(cb, &ca) : -1;
          rgb = cc >= 0 ? cc : 0;
        } else {
          int col = color_parse_a(b, &a);
          if (col >= 0) {
            rgb = col;
            alpha = a;
          }
        }
      }
      if (svg_get_prop(cx, c, "stop-opacity", b, sizeof b))
        alpha = alpha * (int)(svg_parse_unit(b, 1.0f) * 255 + 0.5f) / 255;
      st[n].off = off;
      st[n].rgb = rgb;
      st[n].alpha = alpha;
      n++;
    }
    if (n)
      return n;
  }
  return 0;
}

static unsigned stop_color_at(const struct stop *st, int n, float t)
{
  if (t <= st[0].off)
    return ((unsigned)st[0].alpha << 24) | (unsigned)st[0].rgb;
  for (int i = 1; i < n; i++) {
    if (t <= st[i].off) {
      float span = st[i].off - st[i - 1].off;
      float f = span > 0 ? (t - st[i - 1].off) / span : 1;
      int r0 = (st[i - 1].rgb >> 16) & 255, g0 = (st[i - 1].rgb >> 8) & 255, b0 = st[i - 1].rgb & 255;
      int r1 = (st[i].rgb >> 16) & 255, g1 = (st[i].rgb >> 8) & 255, b1 = st[i].rgb & 255;
      unsigned a = (unsigned)(st[i - 1].alpha + (st[i].alpha - st[i - 1].alpha) * f + 0.5f);
      unsigned r = (unsigned)(r0 + (r1 - r0) * f + 0.5f), g = (unsigned)(g0 + (g1 - g0) * f + 0.5f),
               b = (unsigned)(b0 + (b1 - b0) * f + 0.5f);
      return (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  return ((unsigned)st[n - 1].alpha << 24) | (unsigned)st[n - 1].rgb;
}

static bool grad_setup(const struct ctx *cx, const struct dom_node *g, const struct bb *bb, struct mat m, float opacity,
                       struct src *s)
{
  struct stop st[MAX_STOPS];
  int n = collect_stops(cx, g, st);
  if (!n)
    return false;
  if (n == 1) {
    s->grad = false;
    s->rgb = (unsigned)st[0].rgb;
    s->alpha = (int)(st[0].alpha * opacity + 0.5f);
    return true;
  }
  bool radial = streq(g->tag, "radialgradient");
  const char *u = svg_chain_attr(cx, g, "gradientunits", false);
  bool obb = !(u && starts_ci(u, "userSpaceOnUse"));
  struct mat total = m;
  if (obb) {
    if (!bb->ok || bb->x1 <= bb->x0 || bb->y1 <= bb->y0)
      return false;
    struct mat bm = {bb->x1 - bb->x0, 0, 0, bb->y1 - bb->y0, bb->x0, bb->y0};
    total = svg_mat_mul(total, bm);
  }
  total = svg_mat_mul(total, svg_parse_transform(svg_chain_attr(cx, g, "gradienttransform", false)));
  if (!svg_mat_inv(total, &s->inv))
    return false;
  float vw = obb ? 1 : cx->vw, vh = obb ? 1 : cx->vh, vd = obb ? 1 : (float)sqrt((double)((vw * vw + vh * vh) * 0.5f));

  const char *sp = svg_chain_attr(cx, g, "spreadmethod", false);
  s->spread = sp && starts_ci(sp, "reflect") ? 1 : sp && starts_ci(sp, "repeat") ? 2 : 0;
  s->grad = true;
  s->radial = radial;
  s->alpha = (int)(opacity * 255 + 0.5f);
  if (!radial) {
    s->x1 = svg_grad_coord(svg_chain_attr(cx, g, "x1", true), 0, obb, vw);
    s->y1 = svg_grad_coord(svg_chain_attr(cx, g, "y1", true), 0, obb, vh);
    float x2 = svg_grad_coord(svg_chain_attr(cx, g, "x2", true), vw, obb, vw);
    float y2 = svg_grad_coord(svg_chain_attr(cx, g, "y2", true), 0, obb, vh);
    s->vx = x2 - s->x1;
    s->vy = y2 - s->y1;
    float l2 = s->vx * s->vx + s->vy * s->vy;
    s->ilen2 = l2 > 0 ? 1.0f / l2 : 0;
  } else {
    float cxv = svg_grad_coord(svg_chain_attr(cx, g, "cx", true), vw * 0.5f, obb, vw);
    float cyv = svg_grad_coord(svg_chain_attr(cx, g, "cy", true), vh * 0.5f, obb, vh);
    float r = svg_grad_coord(svg_chain_attr(cx, g, "r", true), vd * 0.5f, obb, vd);
    s->fx = svg_grad_coord(svg_chain_attr(cx, g, "fx", true), cxv, obb, vw);
    s->fy = svg_grad_coord(svg_chain_attr(cx, g, "fy", true), cyv, obb, vh);
    s->cfx = cxv - s->fx;
    s->cfy = cyv - s->fy;
    float dist = (float)sqrt((double)(s->cfx * s->cfx + s->cfy * s->cfy));
    if (dist > r * 0.99f && dist > 0) { /* keep the focus inside the circle */
      float k = r * 0.99f / dist;
      s->fx = cxv - s->cfx * k;
      s->fy = cyv - s->cfy * k;
      s->cfx *= k;
      s->cfy *= k;
    }
    s->ga = s->cfx * s->cfx + s->cfy * s->cfy - r * r;
    if (!(r > 0))
      s->ga = 0;
  }
  for (int i = 0; i < 256; i++)
    s->lut[i] = stop_color_at(st, n, (float)i / 255.0f);
  return true;
}

unsigned svg_grad_px(const struct src *s, float px, float py)
{
  float gx, gy, t;
  svg_mat_apply(s->inv, px, py, &gx, &gy);
  if (!s->radial) {
    t = s->ilen2 > 0 ? ((gx - s->x1) * s->vx + (gy - s->y1) * s->vy) * s->ilen2 : 1;
  } else if (s->ga < 0) {
    float dx = gx - s->fx, dy = gy - s->fy;
    float b = dx * s->cfx + dy * s->cfy, c0 = dx * dx + dy * dy;
    float disc = b * b - s->ga * c0;
    t = (b - (float)sqrt((double)(disc < 0 ? 0 : disc))) / s->ga;
  } else {
    t = 1;
  }
  if (s->spread == 0) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
  } else if (s->spread == 2) {
    t -= (float)floor((double)t);
  } else {
    t = (float)fmod((double)ABSF(t), 2.0);
    if (t > 1)
      t = 2 - t;
  }
  return s->lut[(int)(t * 255.0f + 0.5f)];
}

bool svg_make_src(struct ctx *cx, const struct paint *p, float opacity, int color, const struct bb *bb, struct mat m,
                  struct src *s)
{
  s->grad = s->tex = false;
  s->pix = 0;
  if (p->kind == PAINT_COLOR || p->kind == PAINT_CURRENT) {
    s->rgb = (unsigned)(p->kind == PAINT_COLOR ? p->rgb : color);
    s->alpha = (int)((p->kind == PAINT_COLOR ? p->alpha : 255) * opacity + 0.5f);
    return s->alpha > 0;
  }
  if (p->kind != PAINT_URL)
    return false;
  if (streq(p->ref->tag, "pattern"))
    return svg_pattern_src(cx, p->ref, bb, m, opacity, color, s);
  return grad_setup(cx, p->ref, bb, m, opacity, s);
}

void svg_src_free(struct src *s)
{
  free(s->pix);
  s->pix = 0;
}

/* Bilinear texel lookup at a device pixel centre; straight ARGB. */
unsigned svg_tex_px(const struct src *s, float px, float py)
{
  float u, v;
  svg_mat_apply(s->inv, px, py, &u, &v);
  u -= 0.5f;
  v -= 0.5f;
  float fu = (float)floor((double)u), fv = (float)floor((double)v);
  float tx = u - fu, ty = v - fv;
  int x0 = (int)fu, y0 = (int)fv;
  float acc[4] = {0, 0, 0, 0};
  for (int k = 0; k < 4; k++) {
    int x = x0 + (k & 1), y = y0 + (k >> 1);
    float w = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
    if (s->repeat) {
      x %= s->tw;
      y %= s->th;
      x += x < 0 ? s->tw : 0;
      y += y < 0 ? s->th : 0;
    } else {
      x = x < 0 ? 0 : x >= s->tw ? s->tw - 1 : x;
      y = y < 0 ? 0 : y >= s->th ? s->th - 1 : y;
    }
    unsigned t = s->pix[y * s->tw + x];
    acc[0] += w * (float)(t >> 24);
    acc[1] += w * (float)((t >> 16) & 255);
    acc[2] += w * (float)((t >> 8) & 255);
    acc[3] += w * (float)(t & 255);
  }
  if (acc[0] < 0.5f)
    return 0;
  float inv = 255.0f / acc[0];
  unsigned a = (unsigned)(acc[0] + 0.5f);
  unsigned r = (unsigned)(acc[1] * inv + 0.5f), g = (unsigned)(acc[2] * inv + 0.5f),
           b = (unsigned)(acc[3] * inv + 0.5f);
  return (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}
