/* stroke.c - turn a flattened path into stroke outline polygons (to be filled nonzero). */
#include "c2int.h"
#include "math.h"
#include "owos.h"

struct pts {
  float *v;
  int n, cap;
};

static bool pts_add(struct pts *p, float x, float y)
{
  if (p->n > 0 && fabs(p->v[(p->n - 1) * 2] - x) < 1e-4 && fabs(p->v[(p->n - 1) * 2 + 1] - y) < 1e-4)
    return true;
  if (!c2d_grow((void **)&p->v, &p->cap, (p->n + 1) * 2, sizeof *p->v))
    return false;
  p->v[p->n * 2] = x;
  p->v[p->n * 2 + 1] = y;
  p->n++;
  return true;
}

struct stroker {
  struct c2d_path *out;
  float hw;
  int cap, join;
  float miter;
  bool ok;
};

static void emit(struct stroker *s, const float *xy, int n)
{
  float area = 0;
  for (int i = 0; i < n; i++) {
    int j = i + 1 == n ? 0 : i + 1;
    area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
  }
  bool flip = area < 0;
  for (int i = 0; i < n; i++) {
    int k = flip ? n - 1 - i : i;
    if (i == 0)
      c2d_path_move(s->out, 0, xy[k * 2], xy[k * 2 + 1]);
    else
      c2d_path_line(s->out, 0, xy[k * 2], xy[k * 2 + 1]);
  }
  s->out->sub[s->out->nsub - 1].closed = true;
}

static void emit_circle(struct stroker *s, float cx, float cy)
{
  int n = s->hw > 0.5f ? (int)(C2D_PI / acos(1.0 - 0.1 / s->hw)) + 1 : 8;
  n = n < 8 ? 8 : n > 64 ? 64 : n;
  float xy[128];
  for (int i = 0; i < n; i++) {
    float a = 2 * C2D_PI * (float)i / (float)n;
    xy[i * 2] = cx + s->hw * (float)cos(a);
    xy[i * 2 + 1] = cy + s->hw * (float)sin(a);
  }
  emit(s, xy, n);
}

static void emit_join(struct stroker *s, float vx, float vy, float d0x, float d0y, float d1x, float d1y)
{
  float cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
  if (fabs(cross) < 1e-5f && dot > 0)
    return;
  if (s->join == C2D_ROUND_JOIN) {
    emit_circle(s, vx, vy);
    return;
  }
  float sg = cross > 0 ? -1.0f : 1.0f;
  float o0x = -d0y * s->hw * sg, o0y = d0x * s->hw * sg;
  float o1x = -d1y * s->hw * sg, o1y = d1x * s->hw * sg;
  float cos_half = (float)sqrt((1 + dot) / 2);
  if (s->join == C2D_MITER && cos_half > 1e-4f && 1.0f / cos_half <= s->miter) {
    float k = 1.0f / (1 + dot);
    float xy[8] = {vx, vy, vx + o0x, vy + o0y, vx + (o0x + o1x) * k, vy + (o0y + o1y) * k, vx + o1x, vy + o1y};
    emit(s, xy, 4);
    return;
  }
  float xy[6] = {vx, vy, vx + o0x, vy + o0y, vx + o1x, vy + o1y};
  emit(s, xy, 3);
}

/* One polyline (deduped points) -> quads, joins and caps. `dot_ok`: a zero-length subpath still draws its caps. */
static void stroke_poly(struct stroker *s, const float *v, int n, bool closed, bool dot_ok)
{
  if (n < 2) {
    if (n == 1 && dot_ok) {
      if (s->cap == C2D_ROUND)
        emit_circle(s, v[0], v[1]);
      else if (s->cap == C2D_SQUARE) {
        float xy[8] = {v[0] - s->hw, v[1] - s->hw, v[0] + s->hw, v[1] - s->hw,
                       v[0] + s->hw, v[1] + s->hw, v[0] - s->hw, v[1] + s->hw};
        emit(s, xy, 4);
      }
    }
    return;
  }
  int nseg = closed ? n : n - 1;
  float pdx = 0, pdy = 0, fdx = 0, fdy = 0;
  for (int i = 0; i < nseg; i++) {
    int j = i + 1 == n ? 0 : i + 1;
    float ax = v[i * 2], ay = v[i * 2 + 1], bx = v[j * 2], by = v[j * 2 + 1];
    float dx = bx - ax, dy = by - ay, l = (float)hypot(dx, dy);
    dx /= l;
    dy /= l;
    float qax = ax, qay = ay, qbx = bx, qby = by;
    if (!closed && s->cap == C2D_SQUARE) {
      if (i == 0) {
        qax -= dx * s->hw;
        qay -= dy * s->hw;
      }
      if (i == nseg - 1) {
        qbx += dx * s->hw;
        qby += dy * s->hw;
      }
    }
    float nx = -dy * s->hw, ny = dx * s->hw;
    float xy[8] = {qax + nx, qay + ny, qbx + nx, qby + ny, qbx - nx, qby - ny, qax - nx, qay - ny};
    emit(s, xy, 4);
    if (i == 0) {
      fdx = dx;
      fdy = dy;
    } else {
      emit_join(s, ax, ay, pdx, pdy, dx, dy);
    }
    pdx = dx;
    pdy = dy;
  }
  if (closed)
    emit_join(s, v[0], v[1], pdx, pdy, fdx, fdy);
  else if (s->cap == C2D_ROUND) {
    emit_circle(s, v[0], v[1]);
    emit_circle(s, v[(n - 1) * 2], v[(n - 1) * 2 + 1]);
  }
}

/* Cut the polyline into dash pieces and stroke each as an open path. */
static void stroke_dashed(struct stroker *s, const float *v, int n, bool closed, const float *dash, int nd, float off)
{
  float total = 0;
  for (int i = 0; i < nd; i++)
    total += dash[i];
  off = (float)fmod((double)off, (double)total);
  if (off < 0)
    off += total;
  int di = 0;
  while (off >= dash[di] && (off > 0 || dash[di] == 0)) {
    off -= dash[di];
    di = (di + 1) % nd;
  }
  float left = dash[di] - off;
  bool on = (di & 1) == 0;
  struct pts piece = {0, 0, 0};
  int nseg = closed ? n : n - 1;
  if (on)
    pts_add(&piece, v[0], v[1]);
  int guard = 0;
  for (int i = 0; i < nseg && s->ok; i++) {
    int j = i + 1 == n ? 0 : i + 1;
    float ax = v[i * 2], ay = v[i * 2 + 1], bx = v[j * 2], by = v[j * 2 + 1];
    float len = (float)hypot(bx - ax, by - ay), at = 0;
    while (len - at > left && guard++ < 200000) {
      at += left;
      float t = at / len, x = ax + (bx - ax) * t, y = ay + (by - ay) * t;
      if (on) {
        pts_add(&piece, x, y);
        stroke_poly(s, piece.v, piece.n, false, true);
        piece.n = 0;
      } else {
        piece.n = 0;
        pts_add(&piece, x, y);
      }
      on = !on;
      di = (di + 1) % nd;
      left = dash[di];
    }
    left -= len - at;
    if (on)
      pts_add(&piece, bx, by);
  }
  if (on && piece.n > 0)
    stroke_poly(s, piece.v, piece.n, false, piece.n > 1);
  free(piece.v);
}

bool c2d_stroke_to_path(const struct c2d_path *p, float width, int cap, int join, float miter_limit, const float *dash,
                        int ndash, float dash_off, struct c2d_path *out)
{
  struct stroker s = {out, width / 2, cap, join, miter_limit, true};
  float *dd = 0;
  int nd = ndash;
  if (ndash > 0) {
    float total = 0;
    nd = (ndash & 1) ? ndash * 2 : ndash;
    dd = malloc((unsigned)nd * sizeof *dd);
    if (!dd)
      return false;
    for (int i = 0; i < nd; i++) {
      dd[i] = dash[i % ndash];
      total += dd[i];
    }
    if (total < 0.5f) { /* too fine to resolve: solid */
      free(dd);
      dd = 0;
    }
  }
  struct pts pp = {0, 0, 0};
  for (int sidx = 0; sidx < p->nsub && s.ok; sidx++) {
    const struct c2d_sub *sb = &p->sub[sidx];
    pp.n = 0;
    for (int i = 0; i < sb->n; i++)
      pts_add(&pp, p->pt[(sb->start + i) * 2], p->pt[(sb->start + i) * 2 + 1]);
    bool closed = sb->closed;
    if (closed && pp.n > 1 && fabs(pp.v[0] - pp.v[(pp.n - 1) * 2]) < 1e-4 &&
        fabs(pp.v[1] - pp.v[(pp.n - 1) * 2 + 1]) < 1e-4)
      pp.n--;
    if (sb->n < 2 && !closed)
      continue;
    if (closed && pp.n == 1)
      stroke_poly(&s, pp.v, 1, false, true);
    else if (dd && pp.n > 1)
      stroke_dashed(&s, pp.v, pp.n, closed, dd, nd, dash_off);
    else
      stroke_poly(&s, pp.v, pp.n, closed && pp.n > 2, sb->n >= 2 || closed);
  }
  free(pp.v);
  free(dd);
  return s.ok;
}
