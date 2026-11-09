/* Path2D and the SVG path-data parser behind `new Path2D("M 0 0 L 10 10")`. */

#include "bindings.h"
#include "canvasjs.h"
#include "math.h"

__thread js_object *g_path2d_proto;

#define PI_F 3.14159265f

/* ------------------------------------------------------------- SVG path data */

struct svg_cursor {
  const char *s;
};

static void skip_sep(struct svg_cursor *c)
{
  while (*c->s == ' ' || *c->s == ',' || *c->s == '\t' || *c->s == '\n' || *c->s == '\r')
    c->s++;
}

static bool read_num(struct svg_cursor *c, float *out)
{
  skip_sep(c);
  const char *p = c->s;
  double v = 0, scale = 1;
  bool neg = false, digits = false;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  for (; *p >= '0' && *p <= '9'; p++, digits = true)
    v = v * 10 + (*p - '0');
  if (*p == '.')
    for (p++; *p >= '0' && *p <= '9'; p++, digits = true) {
      scale /= 10;
      v += (*p - '0') * scale;
    }
  if (!digits)
    return false;
  if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
    const char *q = p + 1;
    bool eneg = false;
    int e = 0;
    if (*q == '+' || *q == '-')
      eneg = *q++ == '-';
    if (*q >= '0' && *q <= '9') {
      for (; *q >= '0' && *q <= '9'; q++)
        e = e * 10 + (*q - '0');
      v *= pow(10.0, eneg ? -e : e);
      p = q;
    }
  }
  c->s = p;
  *out = (float)(neg ? -v : v);
  return true;
}

static bool read_flag(struct svg_cursor *c, bool *out)
{
  skip_sep(c);
  if (*c->s != '0' && *c->s != '1')
    return false;
  *out = *c->s++ == '1';
  return true;
}

/* SVG elliptical arc from (x0,y0) to (x,y) as a centre-parameterised ellipse, per the SVG implementation notes */
static void svg_arc(struct c2d_path *p, float x0, float y0, float rx, float ry, float phi_deg, bool large, bool sweep,
                    float x, float y)
{
  if (x0 == x && y0 == y)
    return;
  rx = (float)fabs(rx);
  ry = (float)fabs(ry);
  if (rx == 0 || ry == 0) {
    c2d_path_line(p, 0, x, y);
    return;
  }
  float phi = phi_deg * PI_F / 180, cp = (float)cos(phi), sp = (float)sin(phi);
  float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
  float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
  float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lam > 1) {
    float k = (float)sqrt(lam);
    rx *= k;
    ry *= k;
  }
  float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  float co = den == 0 ? 0 : (float)sqrt(num / den > 0 ? num / den : 0);
  if (large == sweep)
    co = -co;
  float cx1 = co * rx * y1 / ry, cy1 = -co * ry * x1 / rx;
  float cx = cp * cx1 - sp * cy1 + (x0 + x) / 2, cy = sp * cx1 + cp * cy1 + (y0 + y) / 2;
  float ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry, vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;
  float a0 = (float)atan2(uy, ux), da = (float)atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  if (!sweep && da > 0)
    da -= 2 * PI_F;
  else if (sweep && da < 0)
    da += 2 * PI_F;
  c2d_path_ellipse(p, 0, cx, cy, rx, ry, phi, a0, a0 + da, da < 0);
  c2d_path_line(p, 0, x, y); /* land exactly on the endpoint */
}

bool path_from_svg(struct c2d_path *p, const char *d)
{
  struct svg_cursor cur = {d};
  char cmd = 0;
  float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; /* current, subpath start, last control point */
  char prev = 0;
  for (;;) {
    skip_sep(&cur);
    if (!*cur.s)
      return true;
    if ((*cur.s >= 'A' && *cur.s <= 'Z') || (*cur.s >= 'a' && *cur.s <= 'z'))
      cmd = *cur.s++;
    else if (!cmd || cmd == 'z' || cmd == 'Z')
      return false;
    bool rel = cmd >= 'a';
    char up = rel ? (char)(cmd - 32) : cmd;
    float v[7];
    bool f1, f2;
    float ox = rel ? cx : 0, oy = rel ? cy : 0;
    switch (up) {
    case 'Z':
      c2d_path_close(p);
      cx = sx;
      cy = sy;
      break;
    case 'M':
    case 'L':
    case 'T':
      if (!read_num(&cur, &v[0]) || !read_num(&cur, &v[1]))
        return false;
      v[0] += ox, v[1] += oy;
      if (up == 'M') {
        c2d_path_move(p, 0, v[0], v[1]);
        sx = v[0], sy = v[1];
        cmd = rel ? 'l' : 'L'; /* further pairs are implicit linetos */
      } else if (up == 'L') {
        c2d_path_line(p, 0, v[0], v[1]);
      } else {
        float qx = (prev == 'Q' || prev == 'T') ? 2 * cx - lcx : cx,
              qy = (prev == 'Q' || prev == 'T') ? 2 * cy - lcy : cy;
        c2d_path_quad(p, 0, qx, qy, v[0], v[1]);
        lcx = qx, lcy = qy;
      }
      cx = v[0], cy = v[1];
      break;
    case 'H':
      if (!read_num(&cur, &v[0]))
        return false;
      cx = v[0] + ox;
      c2d_path_line(p, 0, cx, cy);
      break;
    case 'V':
      if (!read_num(&cur, &v[0]))
        return false;
      cy = v[0] + oy;
      c2d_path_line(p, 0, cx, cy);
      break;
    case 'C':
    case 'S':
    case 'Q': {
      int n = up == 'C' ? 6 : 4;
      for (int i = 0; i < n; i++)
        if (!read_num(&cur, &v[i]))
          return false;
      for (int i = 0; i < n; i += 2)
        v[i] += ox, v[i + 1] += oy;
      if (up == 'Q') {
        c2d_path_quad(p, 0, v[0], v[1], v[2], v[3]);
        lcx = v[0], lcy = v[1];
        cx = v[2], cy = v[3];
      } else {
        float c1x = v[0], c1y = v[1];
        float *e = up == 'C' ? &v[2] : &v[0];
        if (up == 'S') {
          bool smooth = prev == 'C' || prev == 'S';
          c1x = smooth ? 2 * cx - lcx : cx;
          c1y = smooth ? 2 * cy - lcy : cy;
        }
        c2d_path_bezier(p, 0, c1x, c1y, e[0], e[1], e[2], e[3]);
        lcx = e[0], lcy = e[1];
        cx = e[2], cy = e[3];
      }
      break;
    }
    case 'A':
      if (!read_num(&cur, &v[0]) || !read_num(&cur, &v[1]) || !read_num(&cur, &v[2]) || !read_flag(&cur, &f1) ||
          !read_flag(&cur, &f2) || !read_num(&cur, &v[3]) || !read_num(&cur, &v[4]))
        return false;
      v[3] += ox, v[4] += oy;
      svg_arc(p, cx, cy, v[0], v[1], v[2], f1, f2, v[3], v[4]);
      cx = v[3], cy = v[4];
      break;
    default:
      return false;
    }
    prev = up == 'S' ? 'S' : up;
  }
}

/* ---------------------------------------------------------------------- Path2D */

static struct c2d_path *path_of(js_ctx *ctx, js_value t)
{
  struct chost *h = host_of(t, HK_PATH);
  if (!h) {
    js_throw_type(ctx, "Illegal invocation");
    return 0;
  }
  return h->p;
}

static js_value path2d_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  struct c2d_path *p = calloc(1, sizeof *p);
  if (!p)
    return js_throw_range(ctx, "out of memory");
  struct chost *src = argc > 0 ? host_of(args[0], HK_PATH) : 0;
  if (src)
    c2d_path_copy(p, src->p);
  else if (argc > 0 && args[0].type != JV_UNDEF)
    path_from_svg(p, arg_str(ctx, args[0])); /* a malformed string keeps whatever parsed before the error */
  js_value v = make_host(ctx, g_path2d_proto, HK_PATH, p, 0, 0);
  if (v.type != JV_OBJ) {
    c2d_path_free(p);
    free(p);
  }
  return v;
}

#define PATH_NUMS(n, what)                                                                                             \
  struct c2d_path *p = path_of(ctx, t);                                                                                \
  float v[n];                                                                                                          \
  if (!p || js_nums(ctx, args, argc, n, v, "Path2D", what))                                                            \
  return js_undef()

static js_value path_move_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(2, "moveTo");
  c2d_path_move(p, 0, v[0], v[1]);
  return js_undef();
}

static js_value path_line_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(2, "lineTo");
  c2d_path_line(p, 0, v[0], v[1]);
  return js_undef();
}

static js_value path_quad_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(4, "quadraticCurveTo");
  c2d_path_quad(p, 0, v[0], v[1], v[2], v[3]);
  return js_undef();
}

static js_value path_bezier_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(6, "bezierCurveTo");
  c2d_path_bezier(p, 0, v[0], v[1], v[2], v[3], v[4], v[5]);
  return js_undef();
}

static js_value negative_radius(js_ctx *ctx)
{
  return dom_throw(ctx, "IndexSizeError", "The radius provided is negative.");
}

static js_value path_arc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(5, "arc");
  if (!c2d_path_arc(p, 0, v[0], v[1], v[2], v[3], v[4], argc > 5 && js_to_boolean(args[5])))
    return negative_radius(ctx);
  return js_undef();
}

static js_value path_arc_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(5, "arcTo");
  if (!c2d_path_arc_to(p, 0, v[0], v[1], v[2], v[3], v[4]))
    return negative_radius(ctx);
  return js_undef();
}

static js_value path_ellipse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(7, "ellipse");
  if (!c2d_path_ellipse(p, 0, v[0], v[1], v[2], v[3], v[4], v[5], v[6], argc > 7 && js_to_boolean(args[7])))
    return negative_radius(ctx);
  return js_undef();
}

static js_value path_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(4, "rect");
  c2d_path_rect(p, 0, v[0], v[1], v[2], v[3]);
  return js_undef();
}

static js_value path_round_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  PATH_NUMS(4, "roundRect");
  float rad[4] = {0, 0, 0, 0};
  if (argc > 4 && args[4].type != JV_UNDEF) {
    double d = js_to_number(ctx, args[4].type == JV_OBJ ? js_get_index(ctx, args[4], 0) : args[4]);
    if (d - d != 0 || d < 0)
      return js_throw_range(ctx, "Radii must be non-negative numbers");
    rad[0] = rad[1] = rad[2] = rad[3] = (float)d;
  }
  c2d_path_round_rect(p, 0, v[0], v[1], v[2], v[3], rad);
  return js_undef();
}

static js_value path_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct c2d_path *p = path_of(ctx, t);
  (void)args, (void)argc;
  if (p)
    c2d_path_close(p);
  return js_undef();
}

static js_value path_add_path(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct c2d_path *p = path_of(ctx, t);
  struct chost *other = host_of(A(0), HK_PATH);
  struct c2d_mat m = C2D_IDENTITY;
  if (!p)
    return js_undef();
  if (!other)
    return js_throw_type(ctx, "Failed to execute 'addPath' on 'Path2D': parameter 1 is not of type 'Path2D'");
  if (argc > 1 && A(1).type == JV_OBJ) {
    static const char *const k[6] = {"a", "b", "c", "d", "e", "f"};
    float f[6] = {1, 0, 0, 1, 0, 0};
    for (int i = 0; i < 6; i++) {
      js_value x = js_get(ctx, A(1), k[i]);
      if (x.type != JV_UNDEF)
        f[i] = (float)js_to_number(ctx, x);
    }
    m = (struct c2d_mat){f[0], f[1], f[2], f[3], f[4], f[5]};
  }
  c2d_path_add(p, other->p, &m);
  return js_undef();
}

void canvas_path_setup(js_ctx *ctx, js_object *win)
{
  g_path2d_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_path2d_proto);
  static const struct {
    const char *name;
    js_native fn;
    int arity;
  } methods[] = {
      {"moveTo", path_move_to, 2},          {"lineTo", path_line_to, 2},   {"quadraticCurveTo", path_quad_to, 4},
      {"bezierCurveTo", path_bezier_to, 6}, {"arc", path_arc, 5},          {"arcTo", path_arc_to, 5},
      {"ellipse", path_ellipse, 7},         {"rect", path_rect, 4},        {"roundRect", path_round_rect, 4},
      {"closePath", path_close, 0},         {"addPath", path_add_path, 1},
  };
  for (unsigned i = 0; i < sizeof methods / sizeof *methods; i++)
    js_def_native(ctx, g_path2d_proto, methods[i].name, methods[i].fn, methods[i].arity);
  js_object *ctor = js_new_native(ctx, path2d_ctor, "Path2D", 1);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(g_path2d_proto), 0);
  js_def(g_path2d_proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "Path2D", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}
