/* path.c - canvas2d path construction (flattened into device space), hit testing and bounds. */
#include "c2int.h"
#include "math.h"
#include "owos.h"

const struct c2d_mat C2D_IDENTITY = {1, 0, 0, 1, 0, 0};

struct c2d_mat c2d_mat_mul(struct c2d_mat l, struct c2d_mat r)
{
  struct c2d_mat o;
  o.a = l.a * r.a + l.c * r.b;
  o.b = l.b * r.a + l.d * r.b;
  o.c = l.a * r.c + l.c * r.d;
  o.d = l.b * r.c + l.d * r.d;
  o.e = l.a * r.e + l.c * r.f + l.e;
  o.f = l.b * r.e + l.d * r.f + l.f;
  return o;
}

bool c2d_mat_invert(const struct c2d_mat *m, struct c2d_mat *out)
{
  float det = m->a * m->d - m->b * m->c;
  if (det == 0 || det != det)
    return false;
  float id = 1.0f / det;
  out->a = m->d * id;
  out->b = -m->b * id;
  out->c = -m->c * id;
  out->d = m->a * id;
  out->e = (m->c * m->f - m->d * m->e) * id;
  out->f = (m->b * m->e - m->a * m->f) * id;
  return true;
}

void c2d_mat_apply(const struct c2d_mat *m, float x, float y, float *ox, float *oy)
{
  *ox = m->a * x + m->c * y + m->e;
  *oy = m->b * x + m->d * y + m->f;
}

bool c2d_grow(void **buf, int *cap, int need, unsigned elem)
{
  if (need <= *cap)
    return true;
  int nc = *cap ? *cap * 2 : 32;
  while (nc < need)
    nc *= 2;
  void *nb = realloc(*buf, (unsigned)nc * elem);
  if (!nb)
    return false;
  *buf = nb;
  *cap = nc;
  return true;
}

void c2d_path_free(struct c2d_path *p)
{
  free(p->pt);
  free(p->sub);
  memset(p, 0, sizeof *p);
}

void c2d_path_clear(struct c2d_path *p)
{
  p->npt = 0;
  p->nsub = 0;
  p->ux = p->uy = p->sux = p->suy = 0;
}

bool c2d_path_copy(struct c2d_path *dst, const struct c2d_path *src)
{
  c2d_path_clear(dst);
  dst->ux = src->ux;
  dst->uy = src->uy;
  dst->sux = src->sux;
  dst->suy = src->suy;
  return c2d_path_add(dst, src, 0);
}

static struct c2d_sub *last_sub(struct c2d_path *p) { return p->nsub ? &p->sub[p->nsub - 1] : 0; }

static bool begin_sub(struct c2d_path *p)
{
  if (!c2d_grow((void **)&p->sub, &p->subcap, p->nsub + 1, sizeof *p->sub))
    return false;
  p->sub[p->nsub++] = (struct c2d_sub){p->npt, 0, false};
  return true;
}

static bool push_pt(struct c2d_path *p, float x, float y)
{
  if (!c2d_grow((void **)&p->pt, &p->ptcap, (p->npt + 1) * 2, sizeof *p->pt))
    return false;
  p->pt[p->npt * 2] = x;
  p->pt[p->npt * 2 + 1] = y;
  p->npt++;
  p->sub[p->nsub - 1].n++;
  return true;
}

bool c2d_path_add(struct c2d_path *dst, const struct c2d_path *src, const struct c2d_mat *m)
{
  for (int s = 0; s < src->nsub; s++) {
    const struct c2d_sub *sb = &src->sub[s];
    if (!begin_sub(dst))
      return false;
    for (int i = 0; i < sb->n; i++) {
      float x = src->pt[(sb->start + i) * 2], y = src->pt[(sb->start + i) * 2 + 1];
      if (m)
        c2d_mat_apply(m, x, y, &x, &y);
      if (!push_pt(dst, x, y))
        return false;
    }
    dst->sub[dst->nsub - 1].closed = sb->closed;
  }
  return true;
}

static void to_dev(const struct c2d_mat *m, float x, float y, float *ox, float *oy)
{
  if (m)
    c2d_mat_apply(m, x, y, ox, oy);
  else {
    *ox = x;
    *oy = y;
  }
}

bool c2d_path_has_current(const struct c2d_path *p) { return p->nsub > 0; }

void c2d_path_move(struct c2d_path *p, const struct c2d_mat *m, float x, float y)
{
  float dx, dy;
  if (x - x != 0 || y - y != 0)
    return;
  to_dev(m, x, y, &dx, &dy);
  if (!begin_sub(p) || !push_pt(p, dx, dy))
    return;
  p->ux = p->sux = x;
  p->uy = p->suy = y;
}

/* A subpath to extend: start one at (x,y) when there is none. */
static bool ensure_sub(struct c2d_path *p, const struct c2d_mat *m, float x, float y)
{
  if (p->nsub)
    return true;
  c2d_path_move(p, m, x, y);
  return p->nsub > 0;
}

void c2d_path_line(struct c2d_path *p, const struct c2d_mat *m, float x, float y)
{
  float dx, dy;
  if (x - x != 0 || y - y != 0)
    return;
  if (!p->nsub) {
    c2d_path_move(p, m, x, y);
    return;
  }
  to_dev(m, x, y, &dx, &dy);
  if (push_pt(p, dx, dy)) {
    p->ux = x;
    p->uy = y;
  }
}

static int curve_steps(const struct c2d_path *p, const struct c2d_mat *m, const float *cp, int n)
{
  float len = 0, px, py, qx, qy;
  to_dev(m, p->ux, p->uy, &px, &py);
  for (int i = 0; i < n; i++) {
    to_dev(m, cp[i * 2], cp[i * 2 + 1], &qx, &qy);
    len += (float)hypot(qx - px, qy - py);
    px = qx;
    py = qy;
  }
  int steps = (int)(sqrt(len) * 1.6) + 2;
  return steps > 96 ? 96 : steps;
}

void c2d_path_bezier(struct c2d_path *p, const struct c2d_mat *m, float c1x, float c1y, float c2x, float c2y, float x,
                     float y)
{
  float cp[6] = {c1x, c1y, c2x, c2y, x, y};
  for (int i = 0; i < 6; i++)
    if (cp[i] - cp[i] != 0)
      return;
  if (!ensure_sub(p, m, c1x, c1y))
    return;
  float x0 = p->ux, y0 = p->uy;
  int steps = curve_steps(p, m, cp, 3);
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / (float)steps, u = 1 - t;
    float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    float dx, dy;
    to_dev(m, b0 * x0 + b1 * c1x + b2 * c2x + b3 * x, b0 * y0 + b1 * c1y + b2 * c2y + b3 * y, &dx, &dy);
    if (!push_pt(p, dx, dy))
      return;
  }
  p->ux = x;
  p->uy = y;
}

void c2d_path_quad(struct c2d_path *p, const struct c2d_mat *m, float cx, float cy, float x, float y)
{
  if (cx - cx != 0 || cy - cy != 0 || x - x != 0 || y - y != 0)
    return;
  if (!ensure_sub(p, m, cx, cy))
    return;
  c2d_path_bezier(p, m, p->ux + 2.0f / 3.0f * (cx - p->ux), p->uy + 2.0f / 3.0f * (cy - p->uy),
                  x + 2.0f / 3.0f * (cx - x), y + 2.0f / 3.0f * (cy - y), x, y);
}

void c2d_path_close(struct c2d_path *p)
{
  struct c2d_sub *s = last_sub(p);
  if (!s || s->closed)
    return;
  s->closed = true;
  float dx = p->pt[s->start * 2], dy = p->pt[s->start * 2 + 1];
  /* The next segment starts a fresh subpath at the closed one's start point */
  if (begin_sub(p) && push_pt(p, dx, dy)) {
    p->ux = p->sux;
    p->uy = p->suy;
  }
}

/* Arc about (cx,cy) with radii rx,ry rotated by rot, from angle a0 sweeping `sweep` radians; joins from the current
 * point with a line, like the spec's arc(). */
static void arc_sweep(struct c2d_path *p, const struct c2d_mat *m, float cx, float cy, float rx, float ry, float rot,
                      float a0, float sweep)
{
  float cr = (float)cos(rot), sr = (float)sin(rot);
  float scale = m ? (float)sqrt(fabs((double)m->a * m->d - (double)m->b * m->c)) : 1;
  float rdev = (rx > ry ? rx : ry) * (scale > 0 ? scale : 1);
  float step = rdev > 0.5f ? 2 * (float)acos(1.0 - 0.1 / rdev) : C2D_PI / 4;
  if (step < 0.02f)
    step = 0.02f;
  int n = (int)(fabs(sweep) / step) + 1;
  if (n > 720)
    n = 720;
  for (int i = 0; i <= n; i++) {
    float a = a0 + sweep * (float)i / (float)n;
    float ex = rx * (float)cos(a), ey = ry * (float)sin(a);
    float x = cx + ex * cr - ey * sr, y = cy + ex * sr + ey * cr;
    if (i == 0 && !p->nsub)
      c2d_path_move(p, m, x, y);
    else
      c2d_path_line(p, m, x, y);
  }
}

static float norm_sweep(float a0, float a1, bool ccw)
{
  const float tau = 2 * C2D_PI;
  if (!ccw && a1 - a0 >= tau)
    return tau;
  if (ccw && a0 - a1 >= tau)
    return -tau;
  float d = (float)fmod((double)(a1 - a0), (double)tau);
  if (!ccw) {
    if (d < 0)
      d += tau;
    return d;
  }
  if (d > 0)
    d -= tau;
  return d;
}

bool c2d_path_ellipse(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float rx, float ry, float rot,
                      float a0, float a1, bool ccw)
{
  if (rx < 0 || ry < 0)
    return false;
  if (x - x != 0 || y - y != 0 || a0 - a0 != 0 || a1 - a1 != 0)
    return true;
  arc_sweep(p, m, x, y, rx, ry, rot, a0, norm_sweep(a0, a1, ccw));
  return true;
}

bool c2d_path_arc(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float r, float a0, float a1, bool ccw)
{
  return c2d_path_ellipse(p, m, x, y, r, r, 0, a0, a1, ccw);
}

bool c2d_path_arc_to(struct c2d_path *p, const struct c2d_mat *m, float x1, float y1, float x2, float y2, float r)
{
  if (r < 0)
    return false;
  if (!ensure_sub(p, m, x1, y1))
    return true;
  float x0 = p->ux, y0 = p->uy;
  float v0x = x0 - x1, v0y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
  float l0 = (float)hypot(v0x, v0y), l2 = (float)hypot(v2x, v2y);
  if (l0 == 0 || l2 == 0 || r == 0) {
    c2d_path_line(p, m, x1, y1);
    return true;
  }
  v0x /= l0;
  v0y /= l0;
  v2x /= l2;
  v2y /= l2;
  float cross = v0x * v2y - v0y * v2x, dot = v0x * v2x + v0y * v2y;
  if (fabs(cross) < 1e-6f) {
    c2d_path_line(p, m, x1, y1);
    return true;
  }
  float theta = (float)atan2(fabs(cross), dot);
  float d = r / (float)tan(theta / 2);
  float bx = v0x + v2x, by = v0y + v2y, bl = (float)hypot(bx, by);
  float cdist = r / (float)sin(theta / 2);
  float cx = x1 + bx / bl * cdist, cy = y1 + by / bl * cdist;
  float t0x = x1 + v0x * d, t0y = y1 + v0y * d;
  float a0a = (float)atan2(t0y - cy, t0x - cx);
  float a1a = (float)atan2((y1 + v2y * d) - cy, (x1 + v2x * d) - cx);
  float sweep = a1a - a0a;
  while (sweep > C2D_PI)
    sweep -= 2 * C2D_PI;
  while (sweep < -C2D_PI)
    sweep += 2 * C2D_PI;
  c2d_path_line(p, m, t0x, t0y);
  arc_sweep(p, m, cx, cy, r, r, 0, a0a, sweep);
  return true;
}

void c2d_path_rect(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float w, float h)
{
  c2d_path_move(p, m, x, y);
  c2d_path_line(p, m, x + w, y);
  c2d_path_line(p, m, x + w, y + h);
  c2d_path_line(p, m, x, y + h);
  c2d_path_close(p);
}

void c2d_path_round_rect(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float w, float h,
                         const float *radii)
{
  float r[4] = {radii[0], radii[1], radii[2], radii[3]};
  if (w < 0) {
    x += w;
    w = -w;
  }
  if (h < 0) {
    y += h;
    h = -h;
  }
  float k = 1; /* scale radii down until neighbours fit */
  if (r[0] + r[1] > w && r[0] + r[1] > 0)
    k = w / (r[0] + r[1]);
  if (r[3] + r[2] > w && w / (r[3] + r[2]) < k)
    k = w / (r[3] + r[2]);
  if (r[0] + r[3] > h && h / (r[0] + r[3]) < k)
    k = h / (r[0] + r[3]);
  if (r[1] + r[2] > h && h / (r[1] + r[2]) < k)
    k = h / (r[1] + r[2]);
  for (int i = 0; i < 4; i++)
    r[i] *= k;
  const float q = C2D_PI / 2;
  c2d_path_move(p, m, x + r[0], y);
  c2d_path_line(p, m, x + w - r[1], y);
  if (r[1] > 0)
    arc_sweep(p, m, x + w - r[1], y + r[1], r[1], r[1], 0, -q, q);
  c2d_path_line(p, m, x + w, y + h - r[2]);
  if (r[2] > 0)
    arc_sweep(p, m, x + w - r[2], y + h - r[2], r[2], r[2], 0, 0, q);
  c2d_path_line(p, m, x + r[3], y + h);
  if (r[3] > 0)
    arc_sweep(p, m, x + r[3], y + h - r[3], r[3], r[3], 0, q, q);
  c2d_path_line(p, m, x, y + r[0]);
  if (r[0] > 0)
    arc_sweep(p, m, x + r[0], y + r[0], r[0], r[0], 0, 2 * q, q);
  c2d_path_close(p);
}

bool c2d_path_contains(const struct c2d_path *p, float x, float y, bool evenodd)
{
  int wind = 0, crossings = 0;
  for (int s = 0; s < p->nsub; s++) {
    const struct c2d_sub *sb = &p->sub[s];
    if (sb->n < 2)
      continue;
    for (int i = 0; i < sb->n; i++) {
      int j = i + 1 == sb->n ? 0 : i + 1;
      float ax = p->pt[(sb->start + i) * 2], ay = p->pt[(sb->start + i) * 2 + 1];
      float bx = p->pt[(sb->start + j) * 2], by = p->pt[(sb->start + j) * 2 + 1];
      if ((ay <= y) == (by <= y))
        continue;
      float xi = ax + (y - ay) * (bx - ax) / (by - ay);
      if (xi > x) {
        wind += by > ay ? 1 : -1;
        crossings++;
      }
    }
  }
  return evenodd ? (crossings & 1) != 0 : wind != 0;
}

bool c2d_path_stroke_contains(const struct c2d_path *p, float x, float y, float width)
{
  float hw = width / 2;
  for (int s = 0; s < p->nsub; s++) {
    const struct c2d_sub *sb = &p->sub[s];
    int segs = sb->closed ? sb->n : sb->n - 1;
    for (int i = 0; i < segs; i++) {
      int j = i + 1 == sb->n ? 0 : i + 1;
      float ax = p->pt[(sb->start + i) * 2], ay = p->pt[(sb->start + i) * 2 + 1];
      float bx = p->pt[(sb->start + j) * 2], by = p->pt[(sb->start + j) * 2 + 1];
      float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
      float t = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      if (hypot(x - (ax + t * dx), y - (ay + t * dy)) <= hw)
        return true;
    }
  }
  return false;
}

bool c2d_path_bounds(const struct c2d_path *p, float pad, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
  if (!p->npt)
    return false;
  float minx = p->pt[0], maxx = p->pt[0], miny = p->pt[1], maxy = p->pt[1];
  for (int i = 1; i < p->npt; i++) {
    float x = p->pt[i * 2], y = p->pt[i * 2 + 1];
    minx = x < minx ? x : minx;
    maxx = x > maxx ? x : maxx;
    miny = y < miny ? y : miny;
    maxy = y > maxy ? y : maxy;
  }
  minx = (float)floor(minx - pad);
  miny = (float)floor(miny - pad);
  maxx = (float)ceil(maxx + pad);
  maxy = (float)ceil(maxy + pad);
  if (maxx <= 0 || maxy <= 0 || minx >= (float)w || miny >= (float)h)
    return false;
  *x0 = minx < 0 ? 0 : (int)minx;
  *y0 = miny < 0 ? 0 : (int)miny;
  *x1 = maxx > (float)w ? w : (int)maxx;
  *y1 = maxy > (float)h ? h : (int)maxy;
  return *x1 > *x0 && *y1 > *y0;
}
