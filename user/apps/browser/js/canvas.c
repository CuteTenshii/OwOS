/* HTMLCanvasElement, CanvasRenderingContext2D, gradients, patterns, ImageData, TextMetrics, OffscreenCanvas. */

#include "base64.h"
#include "bindings.h"
#include "canvasjs.h"
#include "color.h"
#include "font.h"
#include "math.h"

__thread js_object *g_ctx2d_proto, *g_gradient_proto, *g_pattern_proto, *g_imagedata_proto, *g_offscreen_proto;

/* Who owns the surfaces this thread creates: the page's document, or a worker's context */
static __thread void *canvas_owner;

#define MAX_IMAGEDATA_PIXELS (16 * 1024 * 1024)

static const char *const comp_names[C2D_COMP_COUNT] = {"source-over",
                                                       "source-in",
                                                       "source-out",
                                                       "source-atop",
                                                       "destination-over",
                                                       "destination-in",
                                                       "destination-out",
                                                       "destination-atop",
                                                       "lighter",
                                                       "copy",
                                                       "xor",
                                                       "multiply",
                                                       "screen",
                                                       "overlay",
                                                       "darken",
                                                       "lighten",
                                                       "color-dodge",
                                                       "color-burn",
                                                       "hard-light",
                                                       "soft-light",
                                                       "difference",
                                                       "exclusion"};

/* ------------------------------------------------------------ native objects */

js_value make_host(js_ctx *ctx, js_object *proto, int tag, void *p, int w, int h)
{
  struct chost *ch = malloc(sizeof *ch);
  if (!ch)
    return js_throw_range(ctx, "out of memory");
  ch->tag = tag;
  ch->p = p;
  ch->w = w;
  ch->h = h;
  js_object *o = js_new_object(ctx, proto);
  o->kind = OBJ_HOST;
  o->host = ch;
  return js_obj_value(o);
}

struct chost *host_of(js_value v, int tag)
{
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_HOST || !v.u.obj->host)
    return 0;
  struct chost *ch = v.u.obj->host;
  return ch->tag == tag ? ch : 0;
}

static void host_finalize(js_object *o)
{
  struct chost *ch = o->host;
  switch (ch->tag) {
  case HK_GRADIENT:
    c2d_gradient_unref(ch->p);
    break;
  case HK_PATTERN:
    c2d_pattern_unref(ch->p);
    break;
  case HK_PATH:
    c2d_path_free(ch->p);
    free(ch->p);
    break;
  case HK_BITMAP:
    free(ch->p);
    break;
  }
  free(ch);
}

/* ------------------------------------------------------------------- helpers */

static bool finite(double d) { return d - d == 0; }

/* Read n finite numbers; 0 = ok, 1 = a value was NaN/Infinity (the call is ignored), -1 = threw (too few args). */
int js_nums(js_ctx *ctx, js_value *args, int argc, int n, float *out, const char *iface, const char *what)
{
  if (argc < n) {
    char msg[128];
    int k = 0;
    sapp(msg, &k, sizeof msg, "Failed to execute '");
    sapp(msg, &k, sizeof msg, what);
    sapp(msg, &k, sizeof msg, "' on '");
    sapp(msg, &k, sizeof msg, iface);
    sapp(msg, &k, sizeof msg, "': more arguments required");
    js_throw_type(ctx, msg);
    return -1;
  }
  int bad = 0;
  for (int i = 0; i < n; i++) {
    double d = js_to_number(ctx, args[i]);
    if (!finite(d))
      bad = 1;
    out[i] = (float)d;
  }
  return bad;
}

static struct canvas_rec *rec_of(js_ctx *ctx, js_value t)
{
  struct chost *ch = host_of(t, HK_CTX);
  if (!ch) {
    js_throw_type(ctx, "Illegal invocation");
    return 0;
  }
  return ch->p;
}

#define DRAW                                                                                                           \
  struct canvas_rec *r = rec_of(ctx, t);                                                                               \
  if (!r)                                                                                                              \
    return js_undef();                                                                                                 \
  struct c2d *c = r->c;                                                                                                \
  (void)c
#define ST (&c->st[c->depth])
#define NUMS(n, what)                                                                                                  \
  float v[n];                                                                                                          \
  if (js_nums(ctx, args, argc, n, v, "CanvasRenderingContext2D", what))                                                \
  return js_undef()

static bool parse_css_color(const char *s, unsigned char out[4])
{
  int a = 255, rgb;
  while (*s == ' ' || *s == '\t' || *s == '\n')
    s++;
  rgb = color_parse_a(s, &a);
  if (rgb < 0) {
    if (!streq_ci(s, "transparent"))
      return false;
    memset(out, 0, 4);
    return true;
  }
  out[0] = (unsigned char)(rgb >> 16);
  out[1] = (unsigned char)(rgb >> 8);
  out[2] = (unsigned char)rgb;
  out[3] = (unsigned char)a;
  return true;
}

static js_value color_string(js_ctx *ctx, const unsigned char c[4])
{
  char s[40];
  int k = 0;
  static const char hx[] = "0123456789abcdef";
  if (c[3] == 255) {
    s[k++] = '#';
    for (int i = 0; i < 3; i++) {
      s[k++] = hx[c[i] >> 4];
      s[k++] = hx[c[i] & 15];
    }
    s[k] = '\0';
    return js_mk_cstring(ctx, s);
  }
  sapp(s, &k, sizeof s, "rgba(");
  for (int i = 0; i < 3; i++) {
    uapp(s, &k, sizeof s, c[i]);
    sapp(s, &k, sizeof s, ", ");
  }
  unsigned a100 = ((unsigned)c[3] * 100 + 127) / 255;
  if (a100 >= 100)
    sapp(s, &k, sizeof s, "1");
  else if (a100 == 0)
    sapp(s, &k, sizeof s, "0");
  else {
    sapp(s, &k, sizeof s, "0.");
    uapp(s, &k, sizeof s, a100 / 10);
    if (a100 % 10)
      uapp(s, &k, sizeof s, a100 % 10);
  }
  sapp(s, &k, sizeof s, ")");
  return js_mk_cstring(ctx, s);
}

/* ---------------------------------------------------------- state: transform */

static js_value ctx_save(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  c2d_save(c);
  return js_undef();
}

static js_value ctx_restore(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  c2d_restore(c);
  return js_undef();
}

static js_value ctx_reset(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  c2d_reset_state(c);
  memset(c->px, 0, (unsigned long)c->w * c->h * 4);
  r->pending = 1;
  return js_undef();
}

static void apply(struct c2d *c, struct c2d_mat m) { ST->m = c2d_mat_mul(ST->m, m); }

static js_value ctx_scale(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(2, "scale");
  apply(c, (struct c2d_mat){v[0], 0, 0, v[1], 0, 0});
  return js_undef();
}

static js_value ctx_rotate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(1, "rotate");
  float s = (float)sin(v[0]), co = (float)cos(v[0]);
  apply(c, (struct c2d_mat){co, s, -s, co, 0, 0});
  return js_undef();
}

static js_value ctx_translate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(2, "translate");
  apply(c, (struct c2d_mat){1, 0, 0, 1, v[0], v[1]});
  return js_undef();
}

static js_value ctx_transform(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(6, "transform");
  apply(c, (struct c2d_mat){v[0], v[1], v[2], v[3], v[4], v[5]});
  return js_undef();
}

/* a..f from six numbers or a matrix-like object (a, b, c, d, e, f) */
static int matrix_args(js_ctx *ctx, js_value *args, int argc, struct c2d_mat *m)
{
  if (argc == 1 && args[0].type == JV_OBJ) {
    static const char *const k[6] = {"a", "b", "c", "d", "e", "f"};
    float f[6];
    for (int i = 0; i < 6; i++) {
      double d = js_to_number(ctx, js_get(ctx, args[0], k[i]));
      if (!finite(d))
        return 1;
      f[i] = (float)d;
    }
    *m = (struct c2d_mat){f[0], f[1], f[2], f[3], f[4], f[5]};
    return 0;
  }
  float v[6];
  int rc = js_nums(ctx, args, argc, 6, v, "CanvasRenderingContext2D", "setTransform");
  if (!rc)
    *m = (struct c2d_mat){v[0], v[1], v[2], v[3], v[4], v[5]};
  return rc;
}

static js_value ctx_set_transform(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct c2d_mat m;
  if (matrix_args(ctx, args, argc, &m))
    return js_undef();
  ST->m = m;
  return js_undef();
}

static js_value ctx_reset_transform(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  ST->m = C2D_IDENTITY;
  return js_undef();
}

static js_value ctx_get_transform(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  struct c2d_mat m = ST->m;
  static const char *const abc[6] = {"a", "b", "c", "d", "e", "f"};
  static const char *const mxx[6] = {"m11", "m12", "m21", "m22", "m41", "m42"};
  float f[6] = {m.a, m.b, m.c, m.d, m.e, m.f};
  gc_push_scope();
  js_object *o = gc_keep(js_new_object(ctx, ctx->object_proto));
  for (int i = 0; i < 6; i++) {
    js_def(o, abc[i], js_number(f[i]), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
    js_def(o, mxx[i], js_number(f[i]), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  }
  js_def(o, "is2D", js_bool(1), P_ENUMERABLE);
  js_def(o, "isIdentity", js_bool(m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0), P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(o);
}

/* ------------------------------------------------------------ state: styles */

static void get_color(const struct c2d_paint *p, unsigned char out[4])
{
  out[0] = p->r, out[1] = p->g, out[2] = p->b, out[3] = p->a;
}

static js_value style_get(js_ctx *ctx, js_value t, bool stroke)
{
  DRAW;
  struct c2d_paint *p = stroke ? &ST->stroke : &ST->fill;
  if (p->kind != C2D_COLOR) {
    js_value last = js_get(ctx, t, stroke ? "@stroke" : "@fill");
    if (last.type == JV_OBJ)
      return last;
  }
  unsigned char col[4];
  get_color(p, col);
  return color_string(ctx, col);
}

static js_value style_set(js_ctx *ctx, js_value t, js_value v, bool stroke)
{
  DRAW;
  struct c2d_paint *p = stroke ? &ST->stroke : &ST->fill;
  struct chost *g = host_of(v, HK_GRADIENT), *pat = host_of(v, HK_PATTERN);
  if (g || pat) {
    if (g)
      c2d_set_paint_gradient(p, g->p);
    else
      c2d_set_paint_pattern(p, pat->p);
    js_def(t.u.obj, stroke ? "@stroke" : "@fill", v, P_WRITABLE);
    return js_undef();
  }
  unsigned char col[4];
  if (v.type == JV_OBJ || !parse_css_color(arg_str(ctx, v), col))
    return js_undef();
  c2d_set_paint_color(p, col[0], col[1], col[2], col[3]);
  return js_undef();
}

static js_value ctx_get_fill(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return style_get(ctx, t, false);
}
static js_value ctx_set_fill(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return style_set(ctx, t, A(0), false);
}
static js_value ctx_get_stroke(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return style_get(ctx, t, true);
}
static js_value ctx_set_stroke(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return style_set(ctx, t, A(0), true);
}

/* A number-valued property: getter and a setter that ignores non-finite values and anything `ok` rejects */
#define NUM_PROP(name, field, cond)                                                                                    \
  static js_value get_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                        \
  {                                                                                                                    \
    DRAW;                                                                                                              \
    (void)args, (void)argc;                                                                                            \
    return js_number(ST->field);                                                                                       \
  }                                                                                                                    \
  static js_value set_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                        \
  {                                                                                                                    \
    DRAW;                                                                                                              \
    double x = js_to_number(ctx, A(0));                                                                                \
    if (finite(x) && (cond))                                                                                           \
      ST->field = (float)x;                                                                                            \
    return js_undef();                                                                                                 \
  }
NUM_PROP(line_width, line_width, x > 0)
NUM_PROP(miter_limit, miter_limit, x > 0)
NUM_PROP(dash_offset, dash_off, 1)
NUM_PROP(global_alpha, alpha, x >= 0 && x <= 1)
NUM_PROP(shadow_blur, shadow_blur, x >= 0)
NUM_PROP(shadow_ox, shadow_ox, 1)
NUM_PROP(shadow_oy, shadow_oy, 1)

static int enum_index(const char *s, const char *const *names, int n)
{
  for (int i = 0; i < n; i++)
    if (streq(s, names[i]))
      return i;
  return -1;
}

#define ENUM_PROP(name, field, ...)                                                                                    \
  static const char *const name##_names[] = {__VA_ARGS__};                                                             \
  static js_value get_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                        \
  {                                                                                                                    \
    DRAW;                                                                                                              \
    (void)args, (void)argc;                                                                                            \
    return js_mk_cstring(ctx, name##_names[ST->field]);                                                                \
  }                                                                                                                    \
  static js_value set_##name(js_ctx *ctx, js_value t, js_value *args, int argc)                                        \
  {                                                                                                                    \
    DRAW;                                                                                                              \
    int i = enum_index(arg_str(ctx, A(0)), name##_names, (int)(sizeof name##_names / sizeof *name##_names));           \
    if (i >= 0)                                                                                                        \
      ST->field = i;                                                                                                   \
    return js_undef();                                                                                                 \
  }
ENUM_PROP(cap, cap, "butt", "round", "square")
ENUM_PROP(join, join, "miter", "round", "bevel")
ENUM_PROP(align, text_align, "start", "end", "left", "right", "center")
ENUM_PROP(baseline, text_base, "alphabetic", "top", "hanging", "middle", "ideographic", "bottom")

static js_value get_comp(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  return js_mk_cstring(ctx, comp_names[ST->comp]);
}
static js_value set_comp(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  int i = enum_index(arg_str(ctx, A(0)), comp_names, C2D_COMP_COUNT);
  if (i >= 0)
    ST->comp = i;
  return js_undef();
}

static js_value get_shadow_color(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  unsigned char col[4] = {ST->shadow_r, ST->shadow_g, ST->shadow_b, ST->shadow_a};
  return color_string(ctx, col);
}
static js_value set_shadow_color(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  unsigned char col[4];
  if (parse_css_color(arg_str(ctx, A(0)), col)) {
    ST->shadow_r = col[0], ST->shadow_g = col[1], ST->shadow_b = col[2], ST->shadow_a = col[3];
  }
  return js_undef();
}

static js_value get_smoothing(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  return js_bool(ST->smoothing);
}
static js_value set_smoothing(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  ST->smoothing = js_to_boolean(A(0));
  return js_undef();
}

/* "italic bold 12px/14px 'Helvetica Neue', monospace": the size, weight and whether the family is monospace */
static bool parse_font(const char *s, float *px, bool *mono, bool *bold)
{
  float size = 0;
  bool b = false, m = false, got = false;
  const char *p = s;
  while (*p && !got) {
    while (*p == ' ')
      p++;
    const char *e = p;
    while (*e && *e != ' ')
      e++;
    if (e == p)
      break;
    double n = 0, frac = 0, scale = 0.1;
    const char *q = p;
    for (; q < e && *q >= '0' && *q <= '9'; q++)
      n = n * 10 + (*q - '0');
    if (q < e && *q == '.')
      for (q++; q < e && *q >= '0' && *q <= '9'; q++, scale /= 10)
        frac += (*q - '0') * scale * 10;
    n += frac;
    int ulen = 0;
    while (q + ulen < e && q[ulen] != '/')
      ulen++;
    if (q > p && ulen > 0) {
      if (ulen == 2 && starts_ci(q, "px"))
        size = (float)n;
      else if (ulen == 2 && starts_ci(q, "pt"))
        size = (float)(n * 4 / 3);
      else if (ulen == 2 && starts_ci(q, "em"))
        size = (float)(n * 16);
      else if (ulen == 3 && starts_ci(q, "rem"))
        size = (float)(n * 16);
      else if (ulen == 1 && *q == '%')
        size = (float)(n * 16 / 100);
      got = size > 0;
    } else if (q == e && q > p && n >= 600) {
      b = true;
    } else if (e - p >= 4 && starts_ci(p, "bold")) {
      b = true;
    }
    p = e;
  }
  if (!got)
    return false;
  for (const char *f = p; *f; f++)
    if (starts_ci(f, "mono") || starts_ci(f, "courier") || starts_ci(f, "consolas"))
      m = true;
  *px = size;
  *mono = m;
  *bold = b;
  return true;
}

static js_value get_font(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  js_value f = js_get(ctx, t, "@font");
  return f.type == JV_STR ? f : js_mk_cstring(ctx, "10px sans-serif");
}
static js_value set_font(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  js_value v = js_to_string(ctx, A(0));
  float px;
  bool mono, bold;
  if (v.type == JV_STR && parse_font(v.u.str->data, &px, &mono, &bold)) {
    ST->font_px = px;
    ST->font_mono = mono;
    ST->font_bold = bold;
    js_def(t.u.obj, "@font", v, P_WRITABLE);
  }
  return js_undef();
}

static js_value ctx_set_line_dash(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  js_value arr = A(0);
  if (arr.type != JV_OBJ)
    return js_undef();
  unsigned n = js_to_uint32(ctx, js_get(ctx, arr, "length"));
  if (n > 64)
    return js_undef();
  float d[64];
  float total = 0;
  for (unsigned i = 0; i < n; i++) {
    double x = js_to_number(ctx, js_get_index(ctx, arr, i));
    if (!finite(x) || x < 0)
      return js_undef();
    d[i] = (float)x;
    total += d[i];
  }
  c2d_set_dash(c, d, total > 0 ? (int)n : 0);
  return js_undef();
}

static js_value ctx_get_line_dash(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  gc_push_scope();
  js_object *a = gc_keep(js_new_array(ctx, 0));
  int n = ST->ndash;
  for (int i = 0; i < n; i++)
    js_array_push(ctx, a, js_number(ST->dash[i % ST->ndash]));
  if (n & 1)
    for (int i = 0; i < n; i++)
      js_array_push(ctx, a, js_number(ST->dash[i]));
  gc_pop_scope();
  return js_obj_value(a);
}

/* ---------------------------------------------------------------- the path */

static js_value ctx_begin_path(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  c2d_path_clear(&c->path);
  return js_undef();
}

static js_value ctx_close_path(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  (void)args, (void)argc;
  c2d_path_close(&c->path);
  return js_undef();
}

static js_value ctx_move_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(2, "moveTo");
  c2d_path_move(&c->path, &ST->m, v[0], v[1]);
  return js_undef();
}

static js_value ctx_line_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(2, "lineTo");
  c2d_path_line(&c->path, &ST->m, v[0], v[1]);
  return js_undef();
}

static js_value ctx_quad_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "quadraticCurveTo");
  c2d_path_quad(&c->path, &ST->m, v[0], v[1], v[2], v[3]);
  return js_undef();
}

static js_value ctx_bezier_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(6, "bezierCurveTo");
  c2d_path_bezier(&c->path, &ST->m, v[0], v[1], v[2], v[3], v[4], v[5]);
  return js_undef();
}

static js_value index_size_error(js_ctx *ctx, const char *msg)
{
  return js_throw_error(ctx, ctx->error_proto, "IndexSizeError", msg);
}

static js_value ctx_arc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(5, "arc");
  if (!c2d_path_arc(&c->path, &ST->m, v[0], v[1], v[2], v[3], v[4], argc > 5 && js_to_boolean(args[5])))
    return index_size_error(ctx, "The radius provided is negative.");
  return js_undef();
}

static js_value ctx_arc_to(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(5, "arcTo");
  if (!c2d_path_arc_to(&c->path, &ST->m, v[0], v[1], v[2], v[3], v[4]))
    return index_size_error(ctx, "The radius provided is negative.");
  return js_undef();
}

static js_value ctx_ellipse(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(7, "ellipse");
  if (!c2d_path_ellipse(&c->path, &ST->m, v[0], v[1], v[2], v[3], v[4], v[5], v[6], argc > 7 && js_to_boolean(args[7])))
    return index_size_error(ctx, "The radius provided is negative.");
  return js_undef();
}

static js_value ctx_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "rect");
  c2d_path_rect(&c->path, &ST->m, v[0], v[1], v[2], v[3]);
  return js_undef();
}

/* tl, tr, br, bl from the CSS-style 1-4 value radii argument; false if it holds a bad value */
static bool corner_radii(js_ctx *ctx, js_value rv, float out[4])
{
  float list[4];
  int n = 0;
  if (rv.type == JV_OBJ) {
    unsigned len = js_to_uint32(ctx, js_get(ctx, rv, "length"));
    if (len < 1 || len > 4)
      return false;
    for (unsigned i = 0; i < len; i++)
      list[n++] = (float)js_to_number(ctx, js_get_index(ctx, rv, i));
  } else {
    list[n++] = (float)js_to_number(ctx, rv);
  }
  for (int i = 0; i < n; i++)
    if (!finite(list[i]) || list[i] < 0)
      return false;
  static const int map[5][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
  for (int i = 0; i < 4; i++)
    out[i] = list[map[n][i]];
  return true;
}

static js_value ctx_round_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "roundRect");
  float rad[4] = {0, 0, 0, 0};
  if (argc > 4 && args[4].type != JV_UNDEF && !corner_radii(ctx, args[4], rad))
    return js_throw_range(ctx, "Radii must be non-negative numbers");
  c2d_path_round_rect(&c->path, &ST->m, v[0], v[1], v[2], v[3], rad);
  return js_undef();
}

/* [path,] [rule]: the path to use (Path2D, moved into device space) and the fill rule */
static bool path_arg(js_ctx *ctx, struct c2d *c, js_value *args, int argc, int *next, struct c2d_path *tmp,
                     const struct c2d_path **use)
{
  struct chost *ph = argc > 0 ? host_of(args[0], HK_PATH) : 0;
  *next = 0;
  *use = &c->path;
  if (ph) {
    *next = 1;
    memset(tmp, 0, sizeof *tmp);
    if (!c2d_path_add(tmp, ph->p, &ST->m)) {
      c2d_path_free(tmp);
      js_throw_range(ctx, "out of memory");
      return false;
    }
    *use = tmp;
  }
  return true;
}

static bool evenodd_arg(js_ctx *ctx, js_value *args, int argc, int i)
{
  return i < argc && args[i].type == JV_STR && streq(arg_str(ctx, args[i]), "evenodd");
}

static js_value ctx_fill(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct c2d_path tmp;
  const struct c2d_path *p;
  int next;
  if (!path_arg(ctx, c, args, argc, &next, &tmp, &p))
    return js_undef();
  c2d_fill(c, p, evenodd_arg(ctx, args, argc, next));
  if (p == &tmp)
    c2d_path_free(&tmp);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_stroke(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct c2d_path tmp;
  const struct c2d_path *p;
  int next;
  if (!path_arg(ctx, c, args, argc, &next, &tmp, &p))
    return js_undef();
  c2d_stroke(c, p);
  if (p == &tmp)
    c2d_path_free(&tmp);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_clip(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct c2d_path tmp;
  const struct c2d_path *p;
  int next;
  if (!path_arg(ctx, c, args, argc, &next, &tmp, &p))
    return js_undef();
  c2d_clip(c, p, evenodd_arg(ctx, args, argc, next));
  if (p == &tmp)
    c2d_path_free(&tmp);
  return js_undef();
}

static js_value point_query(js_ctx *ctx, js_value t, js_value *args, int argc, bool stroke)
{
  DRAW;
  struct c2d_path tmp;
  const struct c2d_path *p;
  int next;
  if (!path_arg(ctx, c, args, argc, &next, &tmp, &p))
    return js_undef();
  float xy[2];
  js_value *rest = args + next;
  int restc = argc - next;
  bool hit = false;
  if (js_nums(ctx, rest, restc, 2, xy, "CanvasRenderingContext2D", stroke ? "isPointInStroke" : "isPointInPath") == 0)
    hit = stroke ? c2d_path_stroke_contains(p, xy[0], xy[1],
                                            ST->line_width * (float)sqrt(fabs(ST->m.a * ST->m.d - ST->m.b * ST->m.c)))
                 : c2d_path_contains(p, xy[0], xy[1], evenodd_arg(ctx, args, argc, next + 2));
  if (p == &tmp)
    c2d_path_free(&tmp);
  return js_bool(hit);
}

static js_value ctx_in_path(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return point_query(ctx, t, args, argc, false);
}

static js_value ctx_in_stroke(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return point_query(ctx, t, args, argc, true);
}

/* -------------------------------------------------------------- rects, text */

static js_value ctx_fill_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "fillRect");
  c2d_fill_rect(c, v[0], v[1], v[2], v[3]);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_stroke_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "strokeRect");
  c2d_stroke_rect(c, v[0], v[1], v[2], v[3]);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_clear_rect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "clearRect");
  c2d_clear_rect(c, v[0], v[1], v[2], v[3]);
  r->pending = 1;
  return js_undef();
}

static js_value text_draw(js_ctx *ctx, js_value t, js_value *args, int argc, bool stroke)
{
  DRAW;
  if (argc < 3)
    return js_throw_type(ctx, "Failed to execute 'fillText' on 'CanvasRenderingContext2D': 3 arguments required");
  double x = js_to_number(ctx, args[1]), y = js_to_number(ctx, args[2]);
  js_value s = js_to_string(ctx, args[0]);
  if (!finite(x) || !finite(y) || s.type != JV_STR)
    return js_undef();
  c2d_fill_text(c, s.u.str->data, (float)x, (float)y, stroke);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_fill_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return text_draw(ctx, t, args, argc, false);
}

static js_value ctx_stroke_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return text_draw(ctx, t, args, argc, true);
}

static js_value ctx_measure_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  js_value s = js_to_string(ctx, A(0));
  float w = s.type == JV_STR ? c2d_measure_text(c, s.u.str->data) : 0;
  int asc = 0, desc = 0, gap;
  const struct font *f = ST->font_mono && c->mono ? c->mono : c->sans;
  if (f)
    font_vmetrics(f, ST->font_px, &asc, &desc, &gap);
  float left = (ST->text_align == C2D_ALIGN_END || ST->text_align == C2D_ALIGN_RIGHT) ? w
               : ST->text_align == C2D_ALIGN_CENTER                                   ? w / 2
                                                                                      : 0;
  gc_push_scope();
  js_object *o = gc_keep(js_new_object(ctx, ctx->object_proto));
  const unsigned fl = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  js_def(o, "width", js_number(w), fl);
  js_def(o, "actualBoundingBoxLeft", js_number(left), fl);
  js_def(o, "actualBoundingBoxRight", js_number(w - left), fl);
  js_def(o, "actualBoundingBoxAscent", js_number(asc), fl);
  js_def(o, "actualBoundingBoxDescent", js_number(-desc), fl);
  js_def(o, "fontBoundingBoxAscent", js_number(asc), fl);
  js_def(o, "fontBoundingBoxDescent", js_number(-desc), fl);
  gc_pop_scope();
  return js_obj_value(o);
}

/* ------------------------------------------------------ gradients, patterns */

static js_value make_gradient(js_ctx *ctx, int kind, const float *v)
{
  struct c2d_gradient *g =
      c2d_gradient_new(kind, v[0], v[1], kind == C2D_RADIAL ? v[2] : 0, kind == C2D_RADIAL ? v[3] : v[2],
                       kind == C2D_RADIAL ? v[4] : v[3], kind == C2D_RADIAL ? v[5] : 0);
  if (!g)
    return js_throw_range(ctx, "out of memory");
  return make_host(ctx, g_gradient_proto, HK_GRADIENT, g, 0, 0);
}

static js_value ctx_linear_gradient(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "createLinearGradient");
  return make_gradient(ctx, C2D_LINEAR, v);
}

static js_value ctx_radial_gradient(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(6, "createRadialGradient");
  if (v[2] < 0 || v[5] < 0)
    return index_size_error(ctx, "The radius provided is negative.");
  return make_gradient(ctx, C2D_RADIAL, v);
}

static js_value gradient_add_stop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct chost *g = host_of(t, HK_GRADIENT);
  double off = js_to_number(ctx, A(0));
  unsigned char col[4];
  if (!g)
    return js_throw_type(ctx, "Illegal invocation");
  if (!finite(off) || off < 0 || off > 1)
    return index_size_error(ctx, "The provided value is outside the range (0.0 - 1.0)");
  if (!parse_css_color(arg_str(ctx, A(1)), col))
    return js_throw_error(ctx, ctx->error_proto, "SyntaxError", "The value provided could not be parsed as a color");
  c2d_gradient_add_stop(g->p, (float)off, col[0], col[1], col[2], col[3]);
  return js_undef();
}

static js_value ctx_create_pattern(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct pixsrc src;
  const char *rep = A(1).type == JV_NULL || A(1).type == JV_UNDEF ? "repeat" : arg_str(ctx, A(1));
  bool rx = streq(rep, "repeat") || streq(rep, "repeat-x"), ry = streq(rep, "repeat") || streq(rep, "repeat-y");
  if (!rx && !ry && !streq(rep, "no-repeat") && !streq(rep, ""))
    return js_throw_error(ctx, ctx->error_proto, "SyntaxError", "The provided type is not a valid repetition");
  int rc = source_pixels(ctx, A(0), &src);
  if (rc == SRC_BAD_TYPE)
    return js_throw_type(ctx, "The image argument is not a drawable image type");
  if (rc == SRC_NOT_READY)
    return js_null();
  struct c2d_pattern *p = c2d_pattern_new(src.rgba, src.w, src.h, rx, ry);
  if (!p)
    return js_throw_range(ctx, "out of memory");
  return make_host(ctx, g_pattern_proto, HK_PATTERN, p, 0, 0);
}

static js_value pattern_set_transform(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx, (void)t, (void)args, (void)argc;
  return js_undef();
}

/* -------------------------------------------------------------- image data */

static js_value make_imagedata(js_ctx *ctx, int w, int h)
{
  gc_push_scope();
  js_object *o = gc_keep(js_new_object(ctx, g_imagedata_proto));
  js_value data = js_new_typedarray(ctx, TA_U8C, w * h * 4);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  js_def(o, "width", js_number(w), P_ENUMERABLE);
  js_def(o, "height", js_number(h), P_ENUMERABLE);
  js_def(o, "data", data, P_ENUMERABLE);
  js_def(o, "colorSpace", js_mk_cstring(ctx, "srgb"), P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(o);
}

/* The RGBA bytes behind an ImageData's .data, or NULL */
static unsigned char *imagedata_bytes(js_ctx *ctx, js_value img, int *w, int *h)
{
  if (img.type != JV_OBJ || img.u.obj->proto != g_imagedata_proto)
    return 0;
  js_value d = js_get(ctx, img, "data");
  *w = js_to_int_sat(ctx, js_get(ctx, img, "width"));
  *h = js_to_int_sat(ctx, js_get(ctx, img, "height"));
  if (d.type != JV_OBJ || d.u.obj->kind != OBJ_TYPEDARRAY || !d.u.obj->host)
    return 0;
  js_typedarray *ta = d.u.obj->host;
  js_arraybuffer *ab = ta->buffer->host;
  if (!ab || *w <= 0 || *h <= 0 || (long long)*w * *h * 4 > ab->byteLength - ta->offset)
    return 0;
  return ab->data + ta->offset;
}

static js_value imagedata_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  int w, h;
  if (argc >= 2 && args[0].type == JV_OBJ && args[0].u.obj->kind == OBJ_TYPEDARRAY) {
    js_typedarray *ta = args[0].u.obj->host;
    js_arraybuffer *ab = ta->buffer->host;
    int len = ta_length(ta);
    w = js_to_int_sat(ctx, args[1]);
    if (w <= 0 || len % (w * 4))
      return index_size_error(ctx, "The input data length is not a multiple of (4 * width).");
    h = len / (w * 4);
    if (argc >= 3 && js_to_int_sat(ctx, args[2]) != h)
      return index_size_error(ctx, "The input data length is not equal to (4 * width * height).");
    js_value out = make_imagedata(ctx, w, h);
    js_typedarray *ot = js_get(ctx, out, "data").u.obj->host;
    memcpy(((js_arraybuffer *)ot->buffer->host)->data, ab->data + ta->offset, (unsigned)len);
    return out;
  }
  w = js_to_int_sat(ctx, A(0));
  h = js_to_int_sat(ctx, A(1));
  if (w <= 0 || h <= 0 || (long long)w * h > MAX_IMAGEDATA_PIXELS)
    return index_size_error(ctx, "The source width or height is zero or too large.");
  return make_imagedata(ctx, w, h);
}

static js_value ctx_create_image_data(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  int w, h;
  if (argc == 1 && args[0].type == JV_OBJ) {
    w = js_to_int_sat(ctx, js_get(ctx, args[0], "width"));
    h = js_to_int_sat(ctx, js_get(ctx, args[0], "height"));
  } else {
    NUMS(2, "createImageData");
    w = (int)v[0];
    h = (int)v[1];
  }
  if (w < 0)
    w = -w;
  if (h < 0)
    h = -h;
  if (w == 0 || h == 0 || (long long)w * h > MAX_IMAGEDATA_PIXELS)
    return index_size_error(ctx, "The source width or height is zero or too large.");
  return make_imagedata(ctx, w, h);
}

static js_value ctx_get_image_data(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  NUMS(4, "getImageData");
  int w = (int)v[2], h = (int)v[3], x = (int)floor(v[0]), y = (int)floor(v[1]);
  if (w < 0) {
    x += w;
    w = -w;
  }
  if (h < 0) {
    y += h;
    h = -h;
  }
  if (w == 0 || h == 0 || (long long)w * h > MAX_IMAGEDATA_PIXELS)
    return index_size_error(ctx, "The source width or height is zero or too large.");
  js_value out = make_imagedata(ctx, w, h);
  if (out.type != JV_OBJ)
    return out;
  int iw, ih;
  unsigned char *dst = imagedata_bytes(ctx, out, &iw, &ih);
  if (dst)
    c2d_get_image_data(c, x, y, w, h, dst);
  return out;
}

static js_value ctx_put_image_data(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  int w, h;
  unsigned char *src = imagedata_bytes(ctx, A(0), &w, &h);
  if (!src)
    return js_throw_type(ctx, "The provided value is not of type 'ImageData'");
  float v[6] = {0, 0, 0, 0, (float)w, (float)h};
  if (js_nums(ctx, args + 1, argc - 1, 2, v, "CanvasRenderingContext2D", "putImageData"))
    return js_undef();
  int sx = 0, sy = 0, sw = w, sh = h;
  if (argc >= 7) {
    float d[4];
    if (js_nums(ctx, args + 3, argc - 3, 4, d, "CanvasRenderingContext2D", "putImageData"))
      return js_undef();
    sx = (int)d[0], sy = (int)d[1], sw = (int)d[2], sh = (int)d[3];
    if (sw < 0) {
      sx += sw;
      sw = -sw;
    }
    if (sh < 0) {
      sy += sh;
      sh = -sh;
    }
  }
  c2d_put_image_data(c, src, w, h, (int)floor(v[0]), (int)floor(v[1]), sx, sy, sw, sh);
  r->pending = 1;
  return js_undef();
}

/* ---------------------------------------------------------------- drawImage */

static js_value ctx_draw_image(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  DRAW;
  struct pixsrc src;
  if (argc < 3)
    return js_throw_type(ctx, "Failed to execute 'drawImage' on 'CanvasRenderingContext2D': 3 arguments required");
  int rc = source_pixels(ctx, args[0], &src);
  if (rc == SRC_BAD_TYPE)
    return js_throw_type(ctx,
                         "The provided value is not of type '(HTMLImageElement or HTMLCanvasElement or ImageBitmap)'");
  if (rc == SRC_NOT_READY)
    return js_undef();
  float v[8];
  float sx = 0, sy = 0, sw = (float)src.w, sh = (float)src.h, dx, dy, dw = (float)src.w, dh = (float)src.h;
  if (argc >= 9) {
    if (js_nums(ctx, args + 1, argc - 1, 8, v, "CanvasRenderingContext2D", "drawImage"))
      return js_undef();
    sx = v[0], sy = v[1], sw = v[2], sh = v[3], dx = v[4], dy = v[5], dw = v[6], dh = v[7];
  } else if (argc >= 5) {
    if (js_nums(ctx, args + 1, argc - 1, 4, v, "CanvasRenderingContext2D", "drawImage"))
      return js_undef();
    dx = v[0], dy = v[1], dw = v[2], dh = v[3];
  } else {
    if (js_nums(ctx, args + 1, argc - 1, 2, v, "CanvasRenderingContext2D", "drawImage"))
      return js_undef();
    dx = v[0], dy = v[1];
  }
  unsigned char *copy = 0;
  if (src.rgba == c->px) { /* drawing a canvas onto itself: read from a snapshot */
    copy = malloc((unsigned long)src.w * src.h * 4);
    if (!copy)
      return js_throw_range(ctx, "out of memory");
    memcpy(copy, c->px, (unsigned long)src.w * src.h * 4);
    src.rgba = copy;
  }
  c2d_draw_image(c, src.rgba, src.w, src.h, sx, sy, sw, sh, dx, dy, dw, dh);
  free(copy);
  r->pending = 1;
  return js_undef();
}

static js_value ctx_is_lost(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx, (void)t, (void)args, (void)argc;
  return js_bool(0);
}

static js_value ctx_get_attributes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t, (void)args, (void)argc;
  gc_push_scope();
  js_object *o = gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "alpha", js_bool(1), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "desynchronized", js_bool(0), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(o, "colorSpace", js_mk_cstring(ctx, "srgb"), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return js_obj_value(o);
}

/* ------------------------------------------- canvas element and OffscreenCanvas */

/* The canvas record behind an HTMLCanvasElement wrapper or an OffscreenCanvas */
static struct canvas_rec *canvas_rec_of(js_ctx *ctx, js_value t, bool create)
{
  struct chost *oc = host_of(t, HK_OFFSCREEN);
  if (oc)
    return oc->p;
  struct dom_node *n = node_of(t);
  if (!n || !streq(n->tag, "canvas")) {
    js_throw_type(ctx, "Illegal invocation");
    return 0;
  }
  struct canvas_rec *r = canvas_find(n);
  if (!r && create) {
    r = canvas_open(n, canvas_owner);
    if (!r)
      js_throw_range(ctx, "out of memory");
  }
  return r;
}

static js_value canvas_get_context(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  if (!streq(arg_str(ctx, A(0)), "2d"))
    return js_null();
  if (js_get(ctx, t, "@transferred").type != JV_UNDEF)
    return js_throw_error(ctx, ctx->error_proto, "InvalidStateError",
                          "Cannot get context from a canvas that has transferred its control to offscreen");
  struct canvas_rec *r = canvas_rec_of(ctx, t, true);
  if (!r)
    return js_undef();
  js_value have = js_get(ctx, t, "@ctx2d");
  if (have.type == JV_OBJ)
    return have;
  gc_push_scope();
  js_value cv = make_host(ctx, g_ctx2d_proto, HK_CTX, r, 0, 0);
  if (cv.type == JV_OBJ) {
    gc_keep(cv.u.obj);
    js_def(cv.u.obj, "canvas", t, P_ENUMERABLE);
    if (t.type == JV_OBJ)
      js_def(t.u.obj, "@ctx2d", cv, P_WRITABLE);
  }
  gc_pop_scope();
  return cv;
}

static js_value canvas_get_dim(js_ctx *ctx, js_value t, bool height)
{
  struct chost *oc = host_of(t, HK_OFFSCREEN);
  if (oc) {
    struct canvas_rec *r = oc->p;
    return js_number(height ? r->c->h : r->c->w);
  }
  struct dom_node *n = node_of(t);
  if (!n || !streq(n->tag, "canvas"))
    return js_throw_type(ctx, "Illegal invocation");
  int w, h;
  struct canvas_rec *r = canvas_find(n);
  if (r)
    w = r->c->w, h = r->c->h;
  else
    canvas_attr_size(n, &w, &h);
  return js_number(height ? h : w);
}

static js_value canvas_set_dim(js_ctx *ctx, js_value t, js_value v, bool height)
{
  double d = js_to_number(ctx, v);
  struct chost *oc = host_of(t, HK_OFFSCREEN);
  struct dom_node *n = oc ? ((struct canvas_rec *)oc->p)->el : node_of(t);
  if (!oc && (!n || !streq(n->tag, "canvas")))
    return js_throw_type(ctx, "Illegal invocation");
  if (!oc && js_get(ctx, t, "@transferred").type != JV_UNDEF)
    return js_throw_error(ctx, ctx->error_proto, "InvalidStateError",
                          "Cannot resize canvas after call to transferControlToOffscreen()");
  long dim = finite(d) && d >= 0 ? (long)d : 0;
  if (dim > 16384)
    dim = 16384;
  struct canvas_rec *r = oc ? oc->p : canvas_find(n);
  if (n) {
    char num[16];
    uint_to_str(num, (unsigned)dim);
    dom_set_attr(n, height ? "height" : "width", num);
  }
  if (r) {
    int w = height ? r->c->w : (int)dim, h = height ? (int)dim : r->c->h;
    if (w > 0 && h > 0)
      canvas_resize(r, w, h);
  }
  return js_undef();
}

static js_value canvas_get_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return canvas_get_dim(ctx, t, false);
}
static js_value canvas_get_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  return canvas_get_dim(ctx, t, true);
}
static js_value canvas_set_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return canvas_set_dim(ctx, t, A(0), false);
}
static js_value canvas_set_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return canvas_set_dim(ctx, t, A(0), true);
}

/* "data:image/png;base64,..." of the bitmap (always PNG; other types fall back to it per spec) */
static js_value canvas_to_data_url(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  struct canvas_rec *r = canvas_rec_of(ctx, t, true);
  if (!r)
    return js_undef();
  int len = 0;
  unsigned char *png = c2d_encode_png(r->c, &len);
  if (!png)
    return js_throw_range(ctx, "out of memory");
  static const char prefix[] = "data:image/png;base64,";
  char *out = malloc(sizeof prefix + (unsigned)base64_encoded_len(len));
  if (!out) {
    free(png);
    return js_throw_range(ctx, "out of memory");
  }
  memcpy(out, prefix, sizeof prefix - 1);
  base64_encode(png, len, out + sizeof prefix - 1);
  js_value s = js_mk_cstring(ctx, out);
  free(out);
  free(png);
  return s;
}

static js_value png_blob(js_ctx *ctx, struct canvas_rec *r)
{
  int len = 0;
  unsigned char *png = c2d_encode_png(r->c, &len);
  if (!png)
    return js_null();
  js_value b = ctx->clone_read_ext ? ctx->clone_read_ext(ctx, "blob", png, len, "image/png") : js_null();
  free(png);
  return b;
}

static js_value deliver_blob(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value pair = ctx->native_data, cb = js_get_index(ctx, pair, 0), blob = js_get_index(ctx, pair, 1);
  (void)t, (void)args, (void)argc;
  if (js_is_callable(cb))
    js_call(ctx, cb, js_undef(), &blob, 1);
  return js_undef();
}

static js_value canvas_to_blob(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct canvas_rec *r = canvas_rec_of(ctx, t, true);
  if (!r)
    return js_undef();
  if (!js_is_callable(A(0)))
    return js_throw_type(ctx, "Failed to execute 'toBlob': parameter 1 is not of type 'Function'");
  gc_push_scope();
  js_object *pair = gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, pair, args[0]);
  js_array_push(ctx, pair, png_blob(ctx, r));
  queue_native(ctx, deliver_blob, js_obj_value(pair));
  gc_pop_scope();
  return js_undef();
}

static js_value offscreen_convert_to_blob(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  struct canvas_rec *r = canvas_rec_of(ctx, t, true);
  if (!r)
    return js_undef();
  return js_promise_resolved(ctx, png_blob(ctx, r));
}

static js_value offscreen_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  int w = js_to_int_sat(ctx, A(0)), h = js_to_int_sat(ctx, A(1));
  if (argc < 2 || w <= 0 || h <= 0)
    return js_throw_type(ctx, "Failed to construct 'OffscreenCanvas': width and height must be positive");
  struct canvas_rec *r = canvas_open_offscreen(w, h, canvas_owner);
  if (!r)
    return js_throw_range(ctx, "OffscreenCanvas allocation failed");
  return make_host(ctx, g_offscreen_proto, HK_OFFSCREEN, r, 0, 0);
}

static js_value offscreen_transfer_to_bitmap(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  struct chost *oc = host_of(t, HK_OFFSCREEN);
  if (!oc)
    return js_throw_type(ctx, "Illegal invocation");
  struct canvas_rec *r = oc->p;
  if (r->el)
    return js_throw_error(ctx, ctx->error_proto, "InvalidStateError",
                          "Cannot transfer an ImageBitmap from a placeholder OffscreenCanvas");
  unsigned long bytes = (unsigned long)r->c->w * r->c->h * 4;
  unsigned char *px = malloc(bytes);
  if (!px)
    return js_throw_range(ctx, "out of memory");
  memcpy(px, r->c->px, bytes);
  memset(r->c->px, 0, bytes);
  r->pending = 1;
  return make_bitmap(ctx, px, r->c->w, r->c->h);
}

static js_value canvas_transfer_control(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args, (void)argc;
  if (js_get(ctx, t, "@transferred").type != JV_UNDEF || js_get(ctx, t, "@ctx2d").type == JV_OBJ)
    return js_throw_error(ctx, ctx->error_proto, "InvalidStateError",
                          "Cannot transfer control from a canvas for which a rendering context has been created");
  struct canvas_rec *r = canvas_rec_of(ctx, t, true);
  if (!r)
    return js_undef();
  gc_push_scope();
  js_value off = make_host(ctx, g_offscreen_proto, HK_OFFSCREEN, r, 0, 0);
  if (off.type == JV_OBJ && t.type == JV_OBJ) {
    gc_keep(off.u.obj);
    js_def(t.u.obj, "@transferred", js_bool(1), P_WRITABLE);
  }
  gc_pop_scope();
  return off;
}

/* ------------------------------------------------------------ pixel sources */

int source_pixels(js_ctx *ctx, js_value v, struct pixsrc *out)
{
  struct chost *bm = host_of(v, HK_BITMAP), *oc = host_of(v, HK_OFFSCREEN);
  if (bm) {
    out->rgba = bm->p;
    out->w = bm->w;
    out->h = bm->h;
    return out->rgba ? SRC_OK : SRC_NOT_READY;
  }
  if (oc) {
    struct canvas_rec *r = oc->p;
    out->rgba = r->c->px;
    out->w = r->c->w;
    out->h = r->c->h;
    return SRC_OK;
  }
  int w, h;
  unsigned char *id = imagedata_bytes(ctx, v, &w, &h);
  if (id) {
    out->rgba = id;
    out->w = w;
    out->h = h;
    return SRC_OK;
  }
  struct dom_node *n = node_of(v);
  if (!n)
    return SRC_BAD_TYPE;
  if (streq(n->tag, "canvas")) {
    struct canvas_rec *r = canvas_open(n, canvas_owner);
    if (!r)
      return SRC_NOT_READY;
    out->rgba = r->c->px;
    out->w = r->c->w;
    out->h = r->c->h;
    return SRC_OK;
  }
  if (streq(n->tag, "img")) {
    out->rgba = js_image_pixels(n, &out->w, &out->h);
    return out->rgba ? SRC_OK : SRC_NOT_READY;
  }
  return streq(n->tag, "video") ? SRC_NOT_READY : SRC_BAD_TYPE;
}

/* --------------------------------------------------------------------- setup */

static js_object *root_proto(js_ctx *ctx, js_object **slot)
{
  *slot = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)slot);
  return *slot;
}

static void define_ctor(js_ctx *ctx, js_object *win, const char *name, js_native fn, int arity, js_object *proto)
{
  js_object *ctor = js_new_native(ctx, fn, name, arity);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, name, js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

js_value js_illegal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv, (void)args, (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

static js_object *canvas_element_proto(void)
{
  for (unsigned i = 0; i < g_nhtml_ifaces; i++)
    if (streq(g_html_ifaces[i].name, "HTMLCanvasElement"))
      return g_html_ifaces[i].proto;
  return 0;
}

static void canvas_setup_core(js_ctx *ctx, js_object *win)
{
  js_object *cp = root_proto(ctx, &g_ctx2d_proto);
  root_proto(ctx, &g_gradient_proto);
  root_proto(ctx, &g_pattern_proto);
  root_proto(ctx, &g_imagedata_proto);
  root_proto(ctx, &g_offscreen_proto);

  static const struct {
    const char *name;
    js_native fn;
    int arity;
  } methods[] = {
      {"save", ctx_save, 0},
      {"restore", ctx_restore, 0},
      {"reset", ctx_reset, 0},
      {"scale", ctx_scale, 2},
      {"rotate", ctx_rotate, 1},
      {"translate", ctx_translate, 2},
      {"transform", ctx_transform, 6},
      {"setTransform", ctx_set_transform, 6},
      {"resetTransform", ctx_reset_transform, 0},
      {"getTransform", ctx_get_transform, 0},
      {"setLineDash", ctx_set_line_dash, 1},
      {"getLineDash", ctx_get_line_dash, 0},
      {"beginPath", ctx_begin_path, 0},
      {"closePath", ctx_close_path, 0},
      {"moveTo", ctx_move_to, 2},
      {"lineTo", ctx_line_to, 2},
      {"quadraticCurveTo", ctx_quad_to, 4},
      {"bezierCurveTo", ctx_bezier_to, 6},
      {"arc", ctx_arc, 5},
      {"arcTo", ctx_arc_to, 5},
      {"ellipse", ctx_ellipse, 7},
      {"rect", ctx_rect, 4},
      {"roundRect", ctx_round_rect, 4},
      {"fill", ctx_fill, 0},
      {"stroke", ctx_stroke, 0},
      {"clip", ctx_clip, 0},
      {"isPointInPath", ctx_in_path, 2},
      {"isPointInStroke", ctx_in_stroke, 2},
      {"fillRect", ctx_fill_rect, 4},
      {"strokeRect", ctx_stroke_rect, 4},
      {"clearRect", ctx_clear_rect, 4},
      {"fillText", ctx_fill_text, 3},
      {"strokeText", ctx_stroke_text, 3},
      {"measureText", ctx_measure_text, 1},
      {"drawImage", ctx_draw_image, 3},
      {"createLinearGradient", ctx_linear_gradient, 4},
      {"createRadialGradient", ctx_radial_gradient, 6},
      {"createPattern", ctx_create_pattern, 2},
      {"createImageData", ctx_create_image_data, 1},
      {"getImageData", ctx_get_image_data, 4},
      {"putImageData", ctx_put_image_data, 3},
      {"isContextLost", ctx_is_lost, 0},
      {"getContextAttributes", ctx_get_attributes, 0},
  };
  for (unsigned i = 0; i < sizeof methods / sizeof *methods; i++)
    def_method(cp, methods[i].name, methods[i].fn, methods[i].arity);

  static const struct {
    const char *name;
    js_native get, set;
  } props[] = {
      {"fillStyle", ctx_get_fill, ctx_set_fill},
      {"strokeStyle", ctx_get_stroke, ctx_set_stroke},
      {"lineWidth", get_line_width, set_line_width},
      {"lineCap", get_cap, set_cap},
      {"lineJoin", get_join, set_join},
      {"miterLimit", get_miter_limit, set_miter_limit},
      {"lineDashOffset", get_dash_offset, set_dash_offset},
      {"globalAlpha", get_global_alpha, set_global_alpha},
      {"globalCompositeOperation", get_comp, set_comp},
      {"shadowColor", get_shadow_color, set_shadow_color},
      {"shadowBlur", get_shadow_blur, set_shadow_blur},
      {"shadowOffsetX", get_shadow_ox, set_shadow_ox},
      {"shadowOffsetY", get_shadow_oy, set_shadow_oy},
      {"font", get_font, set_font},
      {"textAlign", get_align, set_align},
      {"textBaseline", get_baseline, set_baseline},
      {"imageSmoothingEnabled", get_smoothing, set_smoothing},
  };
  for (unsigned i = 0; i < sizeof props / sizeof *props; i++)
    def_acc(cp, props[i].name, props[i].get, props[i].set);

  def_method(g_gradient_proto, "addColorStop", gradient_add_stop, 2);
  def_method(g_pattern_proto, "setTransform", pattern_set_transform, 1);

  define_ctor(ctx, win, "CanvasRenderingContext2D", js_illegal_ctor, 0, cp);
  define_ctor(ctx, win, "CanvasGradient", js_illegal_ctor, 0, g_gradient_proto);
  define_ctor(ctx, win, "CanvasPattern", js_illegal_ctor, 0, g_pattern_proto);
  define_ctor(ctx, win, "ImageData", imagedata_ctor, 2, g_imagedata_proto);
  define_ctor(ctx, win, "OffscreenCanvas", offscreen_ctor, 2, g_offscreen_proto);

  def_method(g_offscreen_proto, "getContext", canvas_get_context, 1);
  def_method(g_offscreen_proto, "convertToBlob", offscreen_convert_to_blob, 0);
  def_method(g_offscreen_proto, "transferToImageBitmap", offscreen_transfer_to_bitmap, 0);
  def_acc(g_offscreen_proto, "width", canvas_get_width, canvas_set_width);
  def_acc(g_offscreen_proto, "height", canvas_get_height, canvas_set_height);
}

void canvas_js_setup(js_ctx *ctx, js_object *win)
{
  js_host_finalizer = host_finalize;
  canvas_owner = g_doc_root;
  canvas_setup_core(ctx, win);
  js_object *ep = canvas_element_proto();
  if (ep) {
    def_method(ep, "getContext", canvas_get_context, 2);
    def_method(ep, "toDataURL", canvas_to_data_url, 1);
    def_method(ep, "toBlob", canvas_to_blob, 1);
    def_method(ep, "transferControlToOffscreen", canvas_transfer_control, 0);
    def_acc(ep, "width", canvas_get_width, canvas_set_width);
    def_acc(ep, "height", canvas_get_height, canvas_set_height);
  }
  canvas_path_setup(ctx, win);
  canvas_img_setup(ctx, win);
}

void canvas_js_setup_worker(js_ctx *ctx, js_object *win)
{
  canvas_owner = ctx;
  canvas_setup_core(ctx, win);
  canvas_path_setup(ctx, win);
}

int canvas_clone_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta)
{
  struct chost *oc = host_of(v, HK_OFFSCREEN);
  struct canvas_rec *r;
  (void)ctx;
  (void)meta;
  if (!oc)
    return 0;
  r = oc->p;
  *bytes = malloc(sizeof r->id);
  if (!*bytes)
    return 0;
  memcpy(*bytes, &r->id, sizeof r->id);
  *len = (int)sizeof r->id;
  strcpy(kind, "ocanvas");
  return 1;
}

js_value canvas_clone_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len)
{
  int id = 0;
  struct canvas_rec *r;
  if (!streq(kind, "ocanvas") || len != (int)sizeof id)
    return js_undef();
  memcpy(&id, bytes, sizeof id);
  r = canvas_find_id(id);
  return r ? make_host(ctx, g_offscreen_proto, HK_OFFSCREEN, r, 0, 0) : js_undef();
}

void canvas_js_teardown(void)
{
  gc_remove_root((void **)&g_ctx2d_proto);
  gc_remove_root((void **)&g_gradient_proto);
  gc_remove_root((void **)&g_pattern_proto);
  gc_remove_root((void **)&g_imagedata_proto);
  gc_remove_root((void **)&g_offscreen_proto);
  gc_remove_root((void **)&g_path2d_proto);
  g_path2d_proto = 0;
  g_ctx2d_proto = g_gradient_proto = g_pattern_proto = g_imagedata_proto = g_offscreen_proto = 0;
  canvas_img_teardown();
}
