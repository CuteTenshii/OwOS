/* svg_geom.c: matrices, number parsing, path data and transforms. */
#include "features/svg/svg_int.h"

struct mat svg_mat_id(void)
{
  struct mat m = {1, 0, 0, 1, 0, 0};
  return m;
}

struct mat svg_mat_mul(struct mat A, struct mat B) /* apply B, then A */
{
  struct mat r;
  r.a = A.a * B.a + A.c * B.b;
  r.b = A.b * B.a + A.d * B.b;
  r.c = A.a * B.c + A.c * B.d;
  r.d = A.b * B.c + A.d * B.d;
  r.e = A.a * B.e + A.c * B.f + A.e;
  r.f = A.b * B.e + A.d * B.f + A.f;
  return r;
}

void svg_mat_apply(struct mat m, float x, float y, float *ox, float *oy)
{
  *ox = m.a * x + m.c * y + m.e;
  *oy = m.b * x + m.d * y + m.f;
}

bool svg_mat_inv(struct mat m, struct mat *out)
{
  float det = m.a * m.d - m.b * m.c;
  if (!(ABSF(det) > 1e-12f))
    return false;
  float id = 1.0f / det;
  out->a = m.d * id;
  out->b = -m.b * id;
  out->c = -m.c * id;
  out->d = m.a * id;
  out->e = (m.c * m.f - m.d * m.e) * id;
  out->f = (m.b * m.e - m.a * m.f) * id;
  return true;
}

/* Average device scale of a matrix (curve flattening, dot sizes). */
float svg_mat_scale(struct mat m)
{
  float sx = m.a * m.a + m.b * m.b, sy = m.c * m.c + m.d * m.d;
  return (float)sqrt((double)((sx + sy) * 0.5f));
}

struct mat svg_mat_translate(float x, float y)
{
  struct mat m = svg_mat_id();
  m.e = x;
  m.f = y;
  return m;
}

/* number and length parsing */
bool svg_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ','; }

bool svg_parse_num(const char **pp, const char *end, float *out)
{
  const char *p = *pp;
  while (p < end && svg_is_ws(*p))
    p++;
  if (p >= end)
    return false;
  char c = *p; /* numeric lead only, so path letters are not read as Infinity/NaN */
  if (!(c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')))
    return false;
  char *e2;
  double v = strtod(p, &e2);
  if ((const char *)e2 == p)
    return false;
  *out = (float)v;
  *pp = (const char *)e2;
  return true;
}

/* Length with unit to user units; % resolves against `ref`. */
bool svg_parse_len(const char *p, float ref, float *out)
{
  const char *e = p + strlen(p);
  float v;
  if (!svg_parse_num(&p, e, &v))
    return false;
  if (*p == '%')
    v = v * ref / 100.0f;
  else if (starts_ci(p, "em"))
    v *= 16.0f;
  else if (starts_ci(p, "ex"))
    v *= 8.0f;
  else if (starts_ci(p, "pt"))
    v *= 96.0f / 72.0f;
  else if (starts_ci(p, "pc"))
    v *= 16.0f;
  else if (starts_ci(p, "mm"))
    v *= 96.0f / 25.4f;
  else if (starts_ci(p, "cm"))
    v *= 96.0f / 2.54f;
  else if (starts_ci(p, "in"))
    v *= 96.0f;
  *out = v;
  return true;
}

float svg_attr_len(const struct dom_node *n, const char *name, float def, float ref)
{
  const char *v = dom_get_attr(n, name);
  float out;
  return (v && svg_parse_len(v, ref, &out)) ? out : def;
}

/* Number or percentage clamped to 0..1 (opacity values). */
float svg_parse_unit(const char *v, float def)
{
  const char *e = v + strlen(v);
  float f;
  if (!svg_parse_num(&v, e, &f))
    return def;
  if (*v == '%')
    f /= 100.0f;
  return f < 0 ? 0 : f > 1 ? 1 : f;
}

bool svg_parse_viewbox(const char *vb, float out[4])
{
  if (!vb)
    return false;
  const char *e = vb + strlen(vb);
  for (int i = 0; i < 4; i++)
    if (!svg_parse_num(&vb, e, &out[i]))
      return false;
  return out[2] > 0 && out[3] > 0;
}

void svg_path_reset(struct path *p) { p->n = p->ns = 0; }

void svg_path_free(struct path *p)
{
  free(p->x);
  free(p->y);
  free(p->sub);
  free(p->closed);
  memset(p, 0, sizeof *p);
}

bool svg_path_pt(struct path *p, float x, float y)
{
  if (p->n >= p->cap) {
    int nc = p->cap ? p->cap * 2 : 64;
    float *nx = realloc(p->x, (unsigned)nc * sizeof(float));
    if (nx)
      p->x = nx;
    float *ny = realloc(p->y, (unsigned)nc * sizeof(float));
    if (ny)
      p->y = ny;
    if (!nx || !ny)
      return false;
    p->cap = nc;
  }
  p->x[p->n] = x;
  p->y[p->n] = y;
  p->n++;
  return true;
}

void svg_path_move(struct path *p, float x, float y)
{
  if (p->ns >= p->scap) {
    int nc = p->scap ? p->scap * 2 : 16;
    int *ns = realloc(p->sub, (unsigned)nc * sizeof(int));
    if (ns)
      p->sub = ns;
    unsigned char *nz = realloc(p->closed, (unsigned)nc);
    if (nz)
      p->closed = nz;
    if (!ns || !nz)
      return;
    p->scap = nc;
  }
  p->sub[p->ns] = p->n;
  p->closed[p->ns] = 0;
  p->ns++;
  svg_path_pt(p, x, y);
}

void svg_path_line(struct path *p, float x, float y)
{
  if (!p->ns)
    return;
  if (p->closed[p->ns - 1]) { /* drawing after Z starts a new subpath at the old start */
    int s = p->sub[p->ns - 1];
    svg_path_move(p, p->x[s], p->y[s]);
  }
  svg_path_pt(p, x, y);
}

void svg_path_close(struct path *p)
{
  if (p->ns)
    p->closed[p->ns - 1] = 1;
}

int svg_sub_end(const struct path *p, int s) { return s + 1 < p->ns ? p->sub[s + 1] : p->n; }

void svg_path_xform(struct path *dst, const struct path *src, struct mat m)
{
  svg_path_reset(dst);
  for (int s = 0; s < src->ns; s++) {
    int e = svg_sub_end(src, s);
    for (int i = src->sub[s]; i < e; i++) {
      float x, y;
      svg_mat_apply(m, src->x[i], src->y[i], &x, &y);
      if (i == src->sub[s])
        svg_path_move(dst, x, y);
      else
        svg_path_pt(dst, x, y);
    }
    if (src->closed[s])
      svg_path_close(dst);
  }
}

void svg_bb_add(struct bb *b, float x, float y)
{
  if (!b->ok) {
    b->x0 = b->x1 = x;
    b->y0 = b->y1 = y;
    b->ok = true;
    return;
  }
  if (x < b->x0)
    b->x0 = x;
  if (x > b->x1)
    b->x1 = x;
  if (y < b->y0)
    b->y0 = y;
  if (y > b->y1)
    b->y1 = y;
}

struct bb svg_path_bbox(const struct path *p, struct mat m)
{
  struct bb b = {0, 0, 0, 0, false};
  for (int i = 0; i < p->n; i++) {
    float x, y;
    svg_mat_apply(m, p->x[i], p->y[i], &x, &y);
    svg_bb_add(&b, x, y);
  }
  return b;
}

/* Cubic Bezier flattened by estimated device length. */
static void path_cubic(struct path *p, float scale, float x0, float y0, float x1, float y1, float x2, float y2,
                       float x3, float y3)
{
  float l = (float)(sqrt((double)((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0))) +
                    sqrt((double)((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1))) +
                    sqrt((double)((x3 - x2) * (x3 - x2) + (y3 - y2) * (y3 - y2)))) *
            scale;
  int n = (int)sqrt((double)(l * 3.0f)) + 3;
  if (n > 100)
    n = 100;
  for (int i = 1; i <= n; i++) {
    float t = (float)i / n, u = 1 - t;
    svg_path_line(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                  u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
  }
}

static void path_quad(struct path *p, float scale, float x0, float y0, float x1, float y1, float x2, float y2)
{
  path_cubic(p, scale, x0, y0, x0 + (x1 - x0) * (2.0f / 3), y0 + (y1 - y0) * (2.0f / 3), x2 + (x1 - x2) * (2.0f / 3),
             y2 + (y1 - y2) * (2.0f / 3), x2, y2);
}

/* SVG elliptical arc (endpoint parameterization) flattened into lines. */
void svg_path_arc(struct path *p, float scale, float x0, float y0, float rx, float ry, float phi_deg, bool large,
                  bool sweep, float x, float y)
{
  if (x0 == x && y0 == y)
    return;
  rx = ABSF(rx);
  ry = ABSF(ry);
  if (rx == 0 || ry == 0) {
    svg_path_line(p, x, y);
    return;
  }
  float phi = phi_deg * SVG_PI / 180.0f;
  float cosp = (float)cos(phi), sinp = (float)sin(phi);
  float dx2 = (x0 - x) * 0.5f, dy2 = (y0 - y) * 0.5f;
  float x1p = cosp * dx2 + sinp * dy2;
  float y1p = -sinp * dx2 + cosp * dy2;
  float rx2 = rx * rx, ry2 = ry * ry, x1p2 = x1p * x1p, y1p2 = y1p * y1p;
  float lam = x1p2 / rx2 + y1p2 / ry2;
  if (lam > 1) {
    float s = (float)sqrt(lam);
    rx *= s;
    ry *= s;
    rx2 = rx * rx;
    ry2 = ry * ry;
  }
  float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
  float den = rx2 * y1p2 + ry2 * x1p2;
  float co = (num > 0 && den > 0) ? (float)sqrt(num / den) : 0;
  if (large == sweep)
    co = -co;
  float cxp = co * (rx * y1p / ry);
  float cyp = -co * (ry * x1p / rx);
  float cx = cosp * cxp - sinp * cyp + (x0 + x) * 0.5f;
  float cy = sinp * cxp + cosp * cyp + (y0 + y) * 0.5f;
  float th1 = (float)atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
  float dth = (float)atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx) - th1;
  if (!sweep && dth > 0)
    dth -= 2 * SVG_PI;
  else if (sweep && dth < 0)
    dth += 2 * SVG_PI;
  float r = (rx > ry ? rx : ry) * scale;
  float step = 2.0f * (float)sqrt(0.1 / (r > 0.1f ? r : 0.1f));
  int n = (int)(ABSF(dth) / step) + 1;
  if (n > 256)
    n = 256;
  for (int i = 1; i <= n; i++) {
    if (i == n) {
      svg_path_line(p, x, y);
      break;
    }
    float t = th1 + dth * (float)i / n;
    float ct = (float)cos(t), st = (float)sin(t);
    svg_path_line(p, cx + rx * ct * cosp - ry * st * sinp, cy + rx * ct * sinp + ry * st * cosp);
  }
}

static bool nums(const char **s, const char *e, float *a, int k)
{
  for (int i = 0; i < k; i++)
    if (!svg_parse_num(s, e, &a[i]))
      return false;
  return true;
}

/* Arc flags are single digits that may run into the next number. */
static bool arc_flag(const char **s, const char *e, bool *out)
{
  const char *p = *s;
  while (p < e && svg_is_ws(*p))
    p++;
  if (p >= e || (*p != '0' && *p != '1'))
    return false;
  *out = *p == '1';
  *s = p + 1;
  return true;
}

/* Build a path "d" in user space; stops at the first error like browsers do. */
void svg_parse_path(struct path *p, float scale, const char *d)
{
  const char *s = d, *e = d + strlen(d);
  float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0; /* current point, subpath start, last control */
  char prev = 0, last = 0;
  float a[6];
  for (;;) {
    while (s < e && svg_is_ws(*s))
      s++;
    if (s >= e)
      return;
    char cmd = *s;
    if ((cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z'))
      s++;
    else if (prev)
      cmd = prev;
    else
      return;
    bool rel = cmd >= 'a';
    char c = rel ? (char)(cmd - 32) : cmd;
    float ox = rel ? cx : 0, oy = rel ? cy : 0;
    if (!p->ns && c != 'M')
      return;
    switch (c) {
    case 'M':
      if (!nums(&s, e, a, 2))
        return;
      cx = sx = ox + a[0];
      cy = sy = oy + a[1];
      svg_path_move(p, cx, cy);
      prev = rel ? 'l' : 'L';
      break;
    case 'L':
      if (!nums(&s, e, a, 2))
        return;
      cx = ox + a[0];
      cy = oy + a[1];
      svg_path_line(p, cx, cy);
      prev = cmd;
      break;
    case 'H':
      if (!nums(&s, e, a, 1))
        return;
      cx = ox + a[0];
      svg_path_line(p, cx, cy);
      prev = cmd;
      break;
    case 'V':
      if (!nums(&s, e, a, 1))
        return;
      cy = oy + a[0];
      svg_path_line(p, cx, cy);
      prev = cmd;
      break;
    case 'C':
    case 'S': {
      float x1, y1;
      if (c == 'S') {
        x1 = (last == 'C' || last == 'S') ? 2 * cx - qx : cx;
        y1 = (last == 'C' || last == 'S') ? 2 * cy - qy : cy;
        if (!nums(&s, e, a, 4))
          return;
        a[4] = a[2];
        a[5] = a[3];
        a[2] = a[0];
        a[3] = a[1];
      } else {
        if (!nums(&s, e, a, 6))
          return;
        x1 = ox + a[0];
        y1 = oy + a[1];
        a[0] = a[2];
        a[1] = a[3];
        a[2] = a[4];
        a[3] = a[5];
        a[4] = a[2];
        a[5] = a[3];
        a[2] = a[0];
        a[3] = a[1];
      }
      /* a[0..1] = control 2, a[4..5] = end (both still relative for S) */
      float x2 = ox + (c == 'S' ? a[2] : a[0]), y2 = oy + (c == 'S' ? a[3] : a[1]);
      float x3 = ox + a[4], y3 = oy + a[5];
      path_cubic(p, scale, cx, cy, x1, y1, x2, y2, x3, y3);
      qx = x2;
      qy = y2;
      cx = x3;
      cy = y3;
      prev = cmd;
      break;
    }
    case 'Q':
    case 'T': {
      float x1, y1;
      if (c == 'T') {
        x1 = (last == 'Q' || last == 'T') ? 2 * cx - qx : cx;
        y1 = (last == 'Q' || last == 'T') ? 2 * cy - qy : cy;
        if (!nums(&s, e, a, 2))
          return;
        a[2] = a[0];
        a[3] = a[1];
      } else {
        if (!nums(&s, e, a, 4))
          return;
        x1 = ox + a[0];
        y1 = oy + a[1];
      }
      float x2 = ox + a[2], y2 = oy + a[3];
      path_quad(p, scale, cx, cy, x1, y1, x2, y2);
      qx = x1;
      qy = y1;
      cx = x2;
      cy = y2;
      prev = cmd;
      break;
    }
    case 'A': {
      float r[3], xy[2];
      bool large, sweep;
      if (!nums(&s, e, r, 3) || !arc_flag(&s, e, &large) || !arc_flag(&s, e, &sweep) || !nums(&s, e, xy, 2))
        return;
      float ex = ox + xy[0], ey = oy + xy[1];
      svg_path_arc(p, scale, cx, cy, r[0], r[1], r[2], large, sweep, ex, ey);
      cx = ex;
      cy = ey;
      prev = cmd;
      break;
    }
    case 'Z':
      svg_path_close(p);
      cx = sx;
      cy = sy;
      prev = 0;
      break;
    default:
      return;
    }
    last = c;
  }
}

/* transform attribute */
struct mat svg_parse_transform(const char *s)
{
  struct mat m = svg_mat_id();
  if (!s)
    return m;
  const char *p = s, *end = s + strlen(s);
  while (p < end) {
    while (p < end && svg_is_ws(*p))
      p++;
    const char *name = p;
    while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
      p++;
    int nl = (int)(p - name);
    while (p < end && *p == ' ')
      p++;
    if (!nl || p >= end || *p != '(')
      break;
    p++;
    float arg[6] = {0};
    int na = 0;
    while (na < 6 && svg_parse_num(&p, end, &arg[na]))
      na++;
    while (p < end && *p != ')')
      p++;
    if (p < end)
      p++;

    struct mat t = svg_mat_id();
    if (nl == 9 && starts_ci(name, "translate")) {
      t.e = arg[0];
      t.f = (na > 1) ? arg[1] : 0;
    } else if (nl == 5 && starts_ci(name, "scale")) {
      t.a = arg[0];
      t.d = (na > 1) ? arg[1] : arg[0];
    } else if (nl == 6 && starts_ci(name, "rotate")) {
      float r = arg[0] * SVG_PI / 180.0f;
      float cs = (float)cos(r), sn = (float)sin(r);
      struct mat rot = {cs, sn, -sn, cs, 0, 0};
      t = (na >= 3)
              ? svg_mat_mul(svg_mat_mul(svg_mat_translate(arg[1], arg[2]), rot), svg_mat_translate(-arg[1], -arg[2]))
              : rot;
    } else if (nl == 5 && starts_ci(name, "skewX")) {
      float r = arg[0] * SVG_PI / 180.0f;
      t.c = (float)(sin(r) / cos(r));
    } else if (nl == 5 && starts_ci(name, "skewY")) {
      float r = arg[0] * SVG_PI / 180.0f;
      t.b = (float)(sin(r) / cos(r));
    } else if (nl == 6 && starts_ci(name, "matrix") && na >= 6) {
      t.a = arg[0];
      t.b = arg[1];
      t.c = arg[2];
      t.d = arg[3];
      t.e = arg[4];
      t.f = arg[5];
    }
    m = svg_mat_mul(m, t);
  }
  return m;
}

struct par svg_parse_par(const char *v)
{
  struct par r = {1, 1, false, false};
  if (!v)
    return r;
  while (*v == ' ')
    v++;
  if (starts_ci(v, "defer"))
    v += 5;
  while (*v == ' ')
    v++;
  if (starts_ci(v, "none")) {
    r.none = true;
    return r;
  }
  if (v[0] && v[1] && v[2] && v[3] && v[4] && v[5] && v[6] && v[7]) {
    r.ax = starts_ci(v + 1, "min") ? 0 : starts_ci(v + 1, "max") ? 2 : 1;
    r.ay = starts_ci(v + 5, "min") ? 0 : starts_ci(v + 5, "max") ? 2 : 1;
  }
  r.slice = contains_ci(v, "slice");
  return r;
}

/* Map the viewBox into the viewport rectangle (x, y, w, h). */
struct mat svg_viewbox_mat(const float vb[4], struct par par, float x, float y, float w, float h)
{
  struct mat m = svg_mat_id();
  float sx = w / vb[2], sy = h / vb[3];
  if (par.none) {
    m.a = sx;
    m.d = sy;
    m.e = x - vb[0] * sx;
    m.f = y - vb[1] * sy;
    return m;
  }
  float s = (sx < sy) != par.slice ? sx : sy;
  m.a = m.d = s;
  m.e = x + (w - vb[2] * s) * (float)par.ax * 0.5f - vb[0] * s;
  m.f = y + (h - vb[3] * s) * (float)par.ay * 0.5f - vb[1] * s;
  return m;
}
