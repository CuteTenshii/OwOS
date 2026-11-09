/* svg_stroke.c: stroke outline generation (caps, joins, dashes). */
#include "features/svg/svg_int.h"

/* stroking: the outline is a union of same-orientation polygons, filled nonzero in one pass */
static void add_poly(struct path *out, const float *x, const float *y, int n)
{
  float area = 0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    area += x[i] * y[j] - x[j] * y[i];
  }
  for (int i = 0; i < n; i++) {
    int k = area >= 0 ? i : n - 1 - i;
    if (i == 0)
      svg_path_move(out, x[k], y[k]);
    else
      svg_path_pt(out, x[k], y[k]);
  }
}

static void add_disc(struct path *out, float cx, float cy, float r, float scale)
{
  float px[64], py[64];
  int n = 8 + (int)(sqrt((double)(r * scale)) * 4);
  if (n > 64)
    n = 64;
  for (int i = 0; i < n; i++) {
    float a = (float)i / n * 2 * SVG_PI;
    px[i] = cx + r * (float)cos(a);
    py[i] = cy + r * (float)sin(a);
  }
  add_poly(out, px, py, n);
}

static void seg_dir(const float *x, const float *y, int n, int i, float *ux, float *uy)
{
  int j = (i + 1) % n;
  float dx = x[j] - x[i], dy = y[j] - y[i];
  float l = (float)sqrt((double)(dx * dx + dy * dy));
  *ux = l > 0 ? dx / l : 1;
  *uy = l > 0 ? dy / l : 0;
}

static void add_join(struct path *out, float vx, float vy, float u0x, float u0y, float u1x, float u1y, float hw,
                     const struct style *st, float scale)
{
  float cross = u0x * u1y - u0y * u1x, dot = u0x * u1x + u0y * u1y;
  if (ABSF(cross) < 1e-4f && dot > 0)
    return;
  float sg = cross > 0 ? -1.0f : 1.0f; /* outer side of the turn */
  if (st->join == JOIN_ROUND && dot < 0.99f) {
    add_disc(out, vx, vy, hw, scale);
    return;
  }
  float px[4] = {vx, vx - sg * u0y * hw, 0, vx - sg * u1y * hw};
  float py[4] = {vy, vy + sg * u0x * hw, 0, vy + sg * u1x * hw};
  int n = 3;
  if (st->join == JOIN_MITER && dot > -0.9999f) {
    float ratio = 1.0f / (float)sqrt((double)((1 + dot) * 0.5f));
    if (ratio <= st->miter) {
      px[2] = vx - sg * hw * (u0y + u1y) / (1 + dot);
      py[2] = vy + sg * hw * (u0x + u1x) / (1 + dot);
      n = 4;
    }
  }
  if (n == 3) {
    px[2] = px[3];
    py[2] = py[3];
  }
  add_poly(out, px, py, n);
}

static void add_cap(struct path *out, float x, float y, float ux, float uy, float hw, int cap, float scale)
{
  if (cap == CAP_ROUND) {
    add_disc(out, x, y, hw, scale);
  } else if (cap == CAP_SQUARE) {
    float nx = -uy * hw, ny = ux * hw, ex = ux * hw, ey = uy * hw;
    float px[4] = {x + nx, x + nx + ex, x - nx + ex, x - nx};
    float py[4] = {y + ny, y + ny + ey, y - ny + ey, y - ny};
    add_poly(out, px, py, 4);
  }
}

static void emit_polyline(struct path *out, const float *x, const float *y, int n, bool closed, const struct style *st,
                          float scale)
{
  float hw = st->sw * 0.5f;
  if (n == 1) {
    if (st->cap == CAP_ROUND)
      add_disc(out, x[0], y[0], hw, scale);
    else if (st->cap == CAP_SQUARE)
      add_cap(out, x[0], y[0], 1, 0, hw, CAP_SQUARE, scale), add_cap(out, x[0], y[0], -1, 0, hw, CAP_SQUARE, scale);
    return;
  }
  int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    float ux, uy;
    seg_dir(x, y, n, i, &ux, &uy);
    int j = (i + 1) % n;
    float nx = -uy * hw, ny = ux * hw;
    float px[4] = {x[i] + nx, x[j] + nx, x[j] - nx, x[i] - nx};
    float py[4] = {y[i] + ny, y[j] + ny, y[j] - ny, y[i] - ny};
    add_poly(out, px, py, 4);
  }
  for (int v = closed ? 0 : 1; v < (closed ? n : n - 1); v++) {
    float u0x, u0y, u1x, u1y;
    seg_dir(x, y, n, (v + segs - 1) % segs, &u0x, &u0y);
    seg_dir(x, y, n, v, &u1x, &u1y);
    add_join(out, x[v], y[v], u0x, u0y, u1x, u1y, hw, st, scale);
  }
  if (!closed) {
    float ux, uy;
    seg_dir(x, y, n, 0, &ux, &uy);
    add_cap(out, x[0], y[0], -ux, -uy, hw, st->cap, scale);
    seg_dir(x, y, n, n - 2, &ux, &uy);
    add_cap(out, x[n - 1], y[n - 1], ux, uy, hw, st->cap, scale);
  }
}

/* Drop consecutive points closer than eps2 (squared); returns the new count. */
static int dedupe(float *x, float *y, int n, float eps2)
{
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (k) {
      float dx = x[i] - x[k - 1], dy = y[i] - y[k - 1];
      if (dx * dx + dy * dy < eps2)
        continue;
    }
    x[k] = x[i];
    y[k] = y[i];
    k++;
  }
  return k;
}

static void flush_dash(struct ctx *cx, struct path *out, const struct style *st, float scale, float eps2)
{
  struct path *d = &cx->aux[1];
  int n = dedupe(d->x, d->y, d->n, eps2);
  if (n >= 1)
    emit_polyline(out, d->x, d->y, n, false, st, scale);
  svg_path_reset(d);
}

static void dash_polyline(struct ctx *cx, struct path *out, const float *x, const float *y, int n, bool closed,
                          const struct style *st, float scale, float eps2)
{
  float period = 0, total = 0;
  for (int i = 0; i < st->ndash; i++)
    period += st->dash[i];
  int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    total += (float)sqrt((double)((x[j] - x[i]) * (x[j] - x[i]) + (y[j] - y[i]) * (y[j] - y[i])));
  }
  if (total / period * (float)st->ndash > 20000) {
    emit_polyline(out, x, y, n, closed, st, scale);
    return;
  }
  float off = (float)fmod((double)st->dash_off, (double)period);
  if (off < 0)
    off += period;
  int idx = 0;
  while (off > st->dash[idx] || (off == st->dash[idx] && off > 0)) {
    off -= st->dash[idx];
    idx = (idx + 1) % st->ndash;
  }
  float remain = st->dash[idx] - off;
  bool on = (idx & 1) == 0;
  struct path *d = &cx->aux[1];
  svg_path_reset(d);
  if (on)
    svg_path_pt(d, x[0], y[0]);
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    float len = (float)sqrt((double)((x[j] - x[i]) * (x[j] - x[i]) + (y[j] - y[i]) * (y[j] - y[i])));
    float pos = 0;
    while (pos < len) {
      float step = remain < len - pos ? remain : len - pos;
      pos += step;
      remain -= step;
      float qx = x[i] + (x[j] - x[i]) * (pos / len), qy = y[i] + (y[j] - y[i]) * (pos / len);
      if (on)
        svg_path_pt(d, qx, qy);
      if (remain <= 0) {
        if (on)
          flush_dash(cx, out, st, scale, eps2);
        idx = (idx + 1) % st->ndash;
        remain = st->dash[idx];
        on = !on;
        if (on)
          svg_path_pt(d, qx, qy);
      }
    }
  }
  if (on)
    flush_dash(cx, out, st, scale, eps2);
}

/* Stroke outline of `src` (user space) into `out`. */
void svg_stroke_build(struct ctx *cx, struct path *out, const struct path *src, const struct style *st, float scale)
{
  svg_path_reset(out);
  float sc = scale > 1e-3f ? scale : 1e-3f;
  float eps2 = (0.01f / sc) * (0.01f / sc);
  struct path *pts = &cx->aux[0];
  for (int s = 0; s < src->ns; s++) {
    svg_path_reset(pts);
    int e = svg_sub_end(src, s);
    for (int i = src->sub[s]; i < e; i++)
      svg_path_pt(pts, src->x[i], src->y[i]);
    int n = dedupe(pts->x, pts->y, pts->n, eps2);
    bool closed = src->closed[s];
    if (closed && n > 1) {
      float dx = pts->x[n - 1] - pts->x[0], dy = pts->y[n - 1] - pts->y[0];
      if (dx * dx + dy * dy < eps2)
        n--;
    }
    if (n < 1 || (closed && n < 3 && n > 1))
      closed = false;
    if (n < 1)
      continue;
    if (st->ndash && n >= 2)
      dash_polyline(cx, out, pts->x, pts->y, n, closed, st, scale, eps2);
    else
      emit_polyline(out, pts->x, pts->y, n, closed, st, scale);
  }
}
