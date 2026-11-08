/* builtins.c — the core standard library: globals and prototype objects.
 *
 * Scope is a useful core, not the whole web platform: console, Math, Object,
 * Array, String, Number, Boolean, JSON and the Error family. RegExp matching,
 * Date, Map/Set, Promise, Symbol and typed arrays are deferred (see js.h). The
 * natives follow interp.c's GC discipline: anything held across an allocation
 * or a callback is pinned with gc_push_scope()/gc_keep(). */
#include "base64.h"
#include "fmt.h"
#include "gc.h"
#include "interp.h"
#include "math.h"
#include "owos.h"
#include "value.h"

#define ARG(i) ((i) < n ? a[i] : js_undef())

static js_object *as_array(js_ctx *ctx, js_value t);                                   /* fwd */
static js_value iter_self(js_ctx *ctx, js_value t, js_value *a, int n);                /* fwd */
static int define_one(js_ctx *ctx, js_object *target, const char *key, js_value desc); /* fwd */

static int memcmp_(const void *a, const void *b, int n)
{
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  int i;
  for (i = 0; i < n; i++)
    if (x[i] != y[i])
      return (int)x[i] - (int)y[i];
  return 0;
}

static void *refp(js_value v) { return v.type == JV_OBJ ? (void *)v.u.obj : v.type == JV_STR ? (void *)v.u.str : NULL; }
#define KEEP(v)                                                                                                        \
  do {                                                                                                                 \
    void *_p = refp(v);                                                                                                \
    if (_p)                                                                                                            \
      gc_keep(_p);                                                                                                     \
  } while (0)
static int nullish(js_value v) { return v.type == JV_UNDEF || v.type == JV_NULL; }
static js_value symbol_tostring(js_ctx *ctx, js_value t, js_value *a, int n);

/* ------------------------------------------------------- string buffer ---- */

typedef struct {
  char *b;
  int n, cap;
  int failed;
} sbuf;
static int sb_need(sbuf *s, int extra)
{
  if (extra < 0 || s->n > 0x3fffffff - extra - 1) {
    s->failed = 1;
    return 0;
  }
  if (s->n + extra + 1 > s->cap) {
    int c = s->cap ? s->cap * 2 : 64;
    while (c < s->n + extra + 1)
      c *= 2;
    char *b = (char *)realloc(s->b, (unsigned)c);
    if (!b) {
      s->failed = 1;
      return 0;
    }
    s->b = b;
    s->cap = c;
  }
  return 1;
}
static void sb_putc(sbuf *s, char c)
{
  if (!sb_need(s, 1))
    return;
  s->b[s->n++] = c;
}
static void sb_put(sbuf *s, const char *t, int n)
{
  if (!sb_need(s, n))
    return;
  memcpy(s->b + s->n, t, (unsigned)n);
  s->n += n;
}
static void sb_puts(sbuf *s, const char *t) { sb_put(s, t, strlen(t)); }

/* ------------------------------------------------------- helpers ---------- */

static js_value str_of(js_ctx *ctx, const char *s, int n) { return js_mk_string(ctx, s, n); }

static int find_sub(const char *h, int hn, const char *needle, int nn, int from)
{
  int i;
  if (nn == 0)
    return from <= hn ? from : hn;
  for (i = from; i + nn <= hn; i++)
    if (memcmp_(h + i, needle, nn) == 0)
      return i;
  return -1;
}

/* normalize a slice index (negative counts from the end), clamped to [0,len] */
static int clamp_index(double d, int len)
{
  int i;
  if (d != d)
    return 0;
  if (d < 0) {
    d += len;
    if (d < 0)
      d = 0;
  }
  if (d > len)
    d = len;
  i = (int)d;
  return i;
}

/* ------------------------------------------------------- console ---------- */

static void fmtv(js_ctx *ctx, sbuf *sb, js_value v, int depth, int quote)
{
  char tmp[DTOA_MAX];
  switch (v.type) {
  case JV_UNDEF:
    sb_puts(sb, "undefined");
    return;
  case JV_NULL:
    sb_puts(sb, "null");
    return;
  case JV_BOOL:
    sb_puts(sb, v.u.boolean ? "true" : "false");
    return;
  case JV_NUM:
    dtoa(v.u.num, tmp);
    sb_puts(sb, tmp);
    return;
  case JV_STR:
    if (!quote) {
      sb_put(sb, v.u.str->data, v.u.str->len);
      return;
    }
    sb_putc(sb, '\'');
    sb_put(sb, v.u.str->data, v.u.str->len);
    sb_putc(sb, '\'');
    return;
  case JV_SYMBOL: {
    js_value s = symbol_tostring(ctx, v, 0, 0);
    sb_put(sb, s.u.str->data, s.u.str->len);
    return;
  }
  case JV_BIGINT: {
    char *s = bi_to_str(js_bigint_of(v), 10);
    if (s) {
      sb_puts(sb, s);
      free(s);
    }
    sb_putc(sb, 'n');
    return;
  }
  default:
    break;
  }
  {
    js_object *o = v.u.obj;
    if (o->kind == OBJ_FUNCTION) {
      js_value nm = js_get(ctx, v, "name");
      sb_puts(sb, "[Function");
      if (nm.type == JV_STR && nm.u.str->len) {
        sb_puts(sb, ": ");
        sb_put(sb, nm.u.str->data, nm.u.str->len);
      }
      sb_putc(sb, ']');
      return;
    }
    if (o->kind == OBJ_ERROR) {
      js_value nm = js_get(ctx, v, "name"), msg = js_get(ctx, v, "message");
      js_value ns = js_to_string(ctx, nm), ms = js_to_string(ctx, msg);
      sb_put(sb, ns.u.str->data, ns.u.str->len);
      if (ms.u.str->len) {
        sb_puts(sb, ": ");
        sb_put(sb, ms.u.str->data, ms.u.str->len);
      }
      return;
    }
    if (depth > 4) {
      sb_puts(sb, o->kind == OBJ_ARRAY ? "[Array]" : "[Object]");
      return;
    }
    if (o->kind == OBJ_ARRAY) {
      unsigned i;
      if (o->elen == 0) {
        sb_puts(sb, "[]");
        return;
      }
      sb_puts(sb, "[ ");
      for (i = 0; i < o->elen; i++) {
        if (i)
          sb_puts(sb, ", ");
        fmtv(ctx, sb, o->elems[i], depth + 1, 1);
      }
      sb_puts(sb, " ]");
      return;
    }
    {
      int i, first = 1;
      sb_puts(sb, "{");
      for (i = 0; i < o->props.n; i++) {
        pentry *pe = &o->props.e[i];
        if (!(pe->flags & P_ENUMERABLE))
          continue;
        sb_puts(sb, first ? " " : ", ");
        first = 0;
        sb_puts(sb, pe->key);
        sb_puts(sb, ": ");
        if (pe->flags & P_ACCESSOR)
          sb_puts(sb, "[Getter/Setter]");
        else
          fmtv(ctx, sb, pe->value, depth + 1, 1);
      }
      sb_puts(sb, first ? "}" : " }");
    }
  }
}

/* Emit `s` as a `%c` style directive: JS_STYLE_ON, the CSS text, JS_STYLE_OFF.
 * Newlines and the marker bytes are dropped from the payload so the directive
 * never spans a line and can't be confused for another directive. */
static void console_style(sbuf *sb, js_value s)
{
  const char *css = s.type == JV_STR && s.u.str ? s.u.str->data : "";
  int len = s.type == JV_STR && s.u.str ? s.u.str->len : 0, i;
  sb_putc(sb, JS_STYLE_ON);
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)css[i];
    if (c == '\n' || c == '\r' || c == JS_STYLE_ON || c == JS_STYLE_OFF)
      sb_putc(sb, ' ');
    else
      sb_putc(sb, (char)c);
  }
  sb_putc(sb, JS_STYLE_OFF);
}

/* Append the base-10 integer part of `d` (per console's %d/%i), or "NaN". */
static void sb_put_int(sbuf *sb, double d)
{
  char t[24];
  int k = 0;
  long long v;
  if (d != d) {
    sb_puts(sb, "NaN");
    return;
  }
  v = (long long)d; /* truncates toward zero */
  if (v < 0)
    sb_putc(sb, '-');
  {
    unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    char rev[24];
    int r = 0;
    do {
      rev[r++] = (char)('0' + (int)(u % 10));
      u /= 10;
    } while (u);
    while (r)
      t[k++] = rev[--r];
  }
  sb_put(sb, t, k);
}

/* WHATWG console formatter: when the first argument is a string, apply its
 * format directives, consuming later args in order. `%s` string, `%d`/`%i`
 * integer, `%f` float, `%o`/`%O` object inspection, `%c` CSS styling (see
 * console_style), `%%` a literal percent. Returns the index of the first
 * un-consumed argument; those are appended space-joined by the caller. */
static int console_format(js_ctx *ctx, sbuf *sb, js_value *args, int argc)
{
  const char *f = args[0].u.str->data;
  int flen = args[0].u.str->len, i = 0, next = 1;
  for (; i < flen; i++) {
    char c = f[i];
    if (c != '%' || i + 1 >= flen) {
      sb_putc(sb, c);
      continue;
    }
    char d = f[i + 1];
    if (d == '%') {
      sb_putc(sb, '%');
      i++;
      continue;
    }
    if (d != 's' && d != 'd' && d != 'i' && d != 'f' && d != 'o' && d != 'O' && d != 'c') {
      sb_putc(sb, c);
      continue; /* unknown directive: emit the % literally */
    }
    if (d == 'c') { /* %c consumes a CSS arg (undefined past the end) */
      console_style(sb, next < argc ? args[next] : js_undef());
      if (next < argc)
        next++;
      i++;
      continue;
    }
    if (next >= argc) {
      sb_putc(sb, c);
      continue;
    } /* no arg left: leave the directive */
    js_value a = args[next++];
    if (d == 's') {
      js_value s = js_to_string(ctx, a);
      if (s.type == JV_STR)
        sb_put(sb, s.u.str->data, s.u.str->len);
    } else if (d == 'f') {
      char t[DTOA_MAX];
      dtoa(js_to_number(ctx, a), t);
      sb_puts(sb, t);
    } else if (d == 'o' || d == 'O')
      fmtv(ctx, sb, a, 0, 0);
    else
      sb_put_int(sb, js_to_number(ctx, a)); /* %d / %i */
    i++;
  }
  return next;
}

/* Format the args and hand them to the print hook with the calling method's
 * level. A leading string argument is a format string (console_format); any
 * remaining args are space-joined after it, as a browser console does. */
static js_value console_emit(js_ctx *ctx, int level, js_value *args, int argc)
{
  sbuf sb;
  int i = 0;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  if (argc > 0 && args[0].type == JV_STR)
    i = console_format(ctx, &sb, args, argc);
  for (; i < argc; i++) {
    if (sb.n)
      sb_putc(&sb, ' ');
    fmtv(ctx, &sb, args[i], 0, 0);
  }
  sb_putc(&sb, '\n');
  if (ctx->print)
    ctx->print(ctx, level, sb.b ? sb.b : "\n", sb.n);
  if (sb.b)
    free(sb.b);
  return js_undef();
}
#define CONSOLE_METHOD(NAME, LEVEL)                                                                                    \
  static js_value NAME(js_ctx *ctx, js_value thisv, js_value *args, int argc)                                          \
  {                                                                                                                    \
    (void)thisv;                                                                                                       \
    return console_emit(ctx, LEVEL, args, argc);                                                                       \
  }
CONSOLE_METHOD(native_console_log, JS_LOG_LOG)
CONSOLE_METHOD(native_console_info, JS_LOG_INFO)
CONSOLE_METHOD(native_console_warn, JS_LOG_WARN)
CONSOLE_METHOD(native_console_error, JS_LOG_ERROR)
CONSOLE_METHOD(native_console_debug, JS_LOG_DEBUG)

/* ------------------------------------------------------- Math ------------- */

#define MATH1(NAME, FN)                                                                                                \
  static js_value NAME(js_ctx *ctx, js_value t, js_value *a, int n)                                                    \
  {                                                                                                                    \
    (void)t;                                                                                                           \
    return js_number(FN(js_to_number(ctx, ARG(0))));                                                                   \
  }
MATH1(m_abs, fabs)
MATH1(m_floor, floor)
MATH1(m_ceil, ceil)
MATH1(m_round_, round)
MATH1(m_trunc, trunc)
MATH1(m_sqrt, sqrt)
MATH1(m_cbrt, cbrt)
MATH1(m_sin, sin)
MATH1(m_cos, cos)
MATH1(m_tan, tan)
MATH1(m_asin, asin)
MATH1(m_acos, acos)
MATH1(m_atan, atan)
MATH1(m_sinh, sinh)
MATH1(m_cosh, cosh)
MATH1(m_tanh, tanh)
MATH1(m_exp, exp)
MATH1(m_log, log)
MATH1(m_log2, log2)
MATH1(m_log10, log10)
#undef MATH1

static js_value m_round(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(floor(x + 0.5)); /* JS rounds half up toward +inf */
}
static js_value m_sign(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  if (x != x)
    return js_number(x);
  return js_number(x > 0 ? 1 : x < 0 ? -1 : x);
}
static js_value m_pow(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_number(pow(js_to_number(ctx, ARG(0)), js_to_number(ctx, ARG(1))));
}
static js_value m_atan2(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_number(atan2(js_to_number(ctx, ARG(0)), js_to_number(ctx, ARG(1))));
}
/* hypot is variadic in JS: sqrt of the sum of squares of every argument */
static js_value m_hypot(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double s = 0;
  int i;
  (void)t;
  for (i = 0; i < n; i++) {
    double x = js_to_number(ctx, a[i]);
    s += x * x;
  }
  return js_number(sqrt(s));
}
/* inverse hyperbolics, expm1, log1p — composed from the base libm calls (the
 * freestanding libm has log/exp/sqrt but not these). Out-of-domain inputs fall
 * out as NaN/Inf naturally, matching the spec's edge cases. */
static js_value m_asinh(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(log(x + sqrt(x * x + 1.0)));
}
static js_value m_acosh(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(log(x + sqrt(x * x - 1.0)));
}
static js_value m_atanh(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(0.5 * log((1.0 + x) / (1.0 - x)));
}
static js_value m_expm1(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(exp(x) - 1.0);
}
static js_value m_log1p(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  return js_number(log(1.0 + x));
}
static js_value m_fround(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_number((double)(float)js_to_number(ctx, ARG(0)));
}
static js_value m_clz32(js_ctx *ctx, js_value t, js_value *a, int n)
{
  unsigned x = js_to_uint32(ctx, ARG(0));
  int c = 0;
  (void)t;
  if (!x)
    return js_number(32);
  while (!(x & 0x80000000u)) {
    c++;
    x <<= 1;
  }
  return js_number(c);
}
static js_value m_imul(js_ctx *ctx, js_value t, js_value *a, int n)
{
  unsigned x = (unsigned)js_to_int32(ctx, ARG(0)), y = (unsigned)js_to_int32(ctx, ARG(1));
  (void)t;
  (void)n;
  return js_number((double)(int)(x * y));
}
static js_value m_max(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double r = -1.0 / 0.0;
  int i;
  (void)t;
  for (i = 0; i < n; i++) {
    double x = js_to_number(ctx, a[i]);
    if (x != x)
      return js_number(x);
    if (x > r)
      r = x;
  }
  return js_number(r);
}
static js_value m_min(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double r = 1.0 / 0.0;
  int i;
  (void)t;
  for (i = 0; i < n; i++) {
    double x = js_to_number(ctx, a[i]);
    if (x != x)
      return js_number(x);
    if (x < r)
      r = x;
  }
  return js_number(r);
}
static js_value m_random(js_ctx *ctx, js_value t, js_value *a, int n)
{
  unsigned x = ctx->rng;
  (void)t;
  (void)a;
  (void)n;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5; /* xorshift32 */
  ctx->rng = x;
  return js_number((double)(x >> 8) / (double)(1u << 24));
}

/* ------------------------------------------------------- Object ----------- */

static js_value native_object(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  if (x.type == JV_OBJ)
    return x;
  if (x.type == JV_UNDEF || x.type == JV_NULL)
    return js_obj_value(js_new_object(ctx, ctx->object_proto));
  /* Box a Symbol into its wrapper object (its internal OBJ_SYMBOL already has
   * Symbol.prototype as [[Prototype]]). Real-world feature tests rely on
   * `typeof Object(sym) === "object"` and `Object(sym) instanceof Symbol`
   * (e.g. core-js's NATIVE_SYMBOL check); failing them sends libraries down a
   * fake string-Symbol fallback that then breaks. */
  if (x.type == JV_SYMBOL || x.type == JV_BIGINT)
    return js_obj_value(x.u.obj);
  return x; /* other primitives: leave as-is (no boxing) */
}
static js_value obj_keys_impl(js_ctx *ctx, js_value o, int mode)
{
  /* mode: 0 keys, 1 values, 2 entries */
  js_object *arr, *s;
  int i;
  if (o.type != JV_OBJ)
    return js_obj_value(js_new_array(ctx, 0));
  s = o.u.obj;
  if (s->kind == OBJ_PROXY) { /* keys via the ownKeys trap, values via get */
    js_value keys = js_proxy_ownkeys(ctx, s);
    js_object *ka = keys.type == JV_OBJ ? keys.u.obj : 0;
    gc_push_scope();
    KEEP(o);
    KEEP(keys);
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    if (ka && ka->kind == OBJ_ARRAY) {
      unsigned e;
      for (e = 0; e < ka->elen; e++) {
        js_value k = ka->elems[e];
        const char *kk;
        if (k.type != JV_STR)
          continue;
        kk = k.u.str->data;
        if (mode == 0)
          js_array_push(ctx, arr, k);
        else if (mode == 1)
          js_array_push(ctx, arr, js_get(ctx, o, kk));
        else {
          js_object *pair = js_new_array(ctx, 0);
          gc_keep(pair);
          js_array_push(ctx, pair, k);
          js_array_push(ctx, pair, js_get(ctx, o, kk));
          js_array_push(ctx, arr, js_obj_value(pair));
        }
      }
    }
    gc_pop_scope();
    return js_obj_value(arr);
  }
  gc_push_scope();
  KEEP(o);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (s->kind == OBJ_ARRAY) {
    unsigned e;
    for (e = 0; e < s->elen; e++) {
      char ib[24];
      u64toa((unsigned long long)e, ib, 10);
      if (mode == 0)
        js_array_push(ctx, arr, js_mk_cstring(ctx, ib));
      else if (mode == 1)
        js_array_push(ctx, arr, s->elems[e]);
      else {
        js_object *pair = js_new_array(ctx, 0);
        gc_keep(pair);
        js_array_push(ctx, pair, js_mk_cstring(ctx, ib));
        js_array_push(ctx, pair, s->elems[e]);
        js_array_push(ctx, arr, js_obj_value(pair));
      }
    }
  }
  for (i = 0; i < s->props.n; i++) {
    pentry *pe = &s->props.e[i];
    if (!(pe->flags & P_ENUMERABLE))
      continue;
    if (mode == 0)
      js_array_push(ctx, arr, js_mk_cstring(ctx, pe->key));
    else if (mode == 1)
      js_array_push(ctx, arr, js_get(ctx, o, pe->key));
    else {
      js_object *pair = js_new_array(ctx, 0);
      gc_keep(pair);
      js_array_push(ctx, pair, js_mk_cstring(ctx, pe->key));
      js_array_push(ctx, pair, js_get(ctx, o, pe->key));
      js_array_push(ctx, arr, js_obj_value(pair));
    }
  }
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value object_keys(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return obj_keys_impl(ctx, ARG(0), 0);
}
static js_value object_values(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return obj_keys_impl(ctx, ARG(0), 1);
}
static js_value object_entries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return obj_keys_impl(ctx, ARG(0), 2);
}

/* structuredClone: deep-copy of plain objects/arrays with cycle preservation
 * (seen[] maps each source object to its clone). Functions and exotic hosts are
 * shared by reference — pragmatic vs the spec's DataCloneError. */
#define SCLONE_MAX 2048
static js_value sclone(js_ctx *ctx, js_value v, js_object **ss, js_object **sd, int *ns, int depth)
{
  js_object *o;
  int i;
  if (v.type != JV_OBJ)
    return v;
  o = v.u.obj;
  for (i = 0; i < *ns; i++)
    if (ss[i] == o)
      return js_obj_value(sd[i]);
  if (depth > 200 || o->kind == OBJ_FUNCTION)
    return v;
  if (o->kind == OBJ_ARRAY || o->kind == OBJ_PLAIN) {
    js_object *na;
    gc_push_scope();
    KEEP(v);
    na = (js_object *)gc_keep(o->kind == OBJ_ARRAY ? js_new_array(ctx, 0) : js_new_object(ctx, ctx->object_proto));
    if (*ns < SCLONE_MAX) {
      ss[*ns] = o;
      sd[*ns] = na;
      (*ns)++;
    }
    if (o->kind == OBJ_ARRAY) {
      unsigned e;
      for (e = 0; e < o->elen; e++)
        js_array_push(ctx, na, sclone(ctx, o->elems[e], ss, sd, ns, depth + 1));
    }
    for (i = 0; i < o->props.n; i++) {
      pentry *pe = &o->props.e[i];
      if (!(pe->flags & P_ENUMERABLE) || (pe->flags & P_ACCESSOR))
        continue;
      js_def(na, pe->key, sclone(ctx, pe->value, ss, sd, ns, depth + 1), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
    }
    gc_pop_scope();
    return js_obj_value(na);
  }
  return v; /* symbols, typed arrays, errors, … : shared */
}
static js_value g_structured_clone(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object **ss, **sd;
  int ns = 0;
  js_value r;
  (void)t;
  (void)n;
  ss = (js_object **)malloc(SCLONE_MAX * sizeof *ss);
  sd = (js_object **)malloc(SCLONE_MAX * sizeof *sd);
  if (!ss || !sd) {
    free(ss);
    free(sd);
    return ARG(0);
  }
  r = sclone(ctx, ARG(0), ss, sd, &ns, 0);
  free(ss);
  free(sd);
  return r;
}

static js_value object_assign(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value target = ARG(0);
  int i, k;
  (void)t;
  if (target.type != JV_OBJ)
    return target;
  gc_push_scope();
  KEEP(target);
  for (i = 1; i < n; i++) {
    js_value src = a[i];
    if (src.type != JV_OBJ)
      continue;
    KEEP(src);
    if (src.u.obj->kind == OBJ_ARRAY) {
      unsigned e;
      for (e = 0; e < src.u.obj->elen; e++)
        js_set_index(ctx, target, e, src.u.obj->elems[e]);
    }
    for (k = 0; k < src.u.obj->props.n; k++)
      if (src.u.obj->props.e[k].flags & P_ENUMERABLE)
        js_set(ctx, target, src.u.obj->props.e[k].key, js_get(ctx, src, src.u.obj->props.e[k].key));
  }
  gc_pop_scope();
  return target;
}
static js_value object_create(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value proto = ARG(0), props = ARG(1);
  js_object *o;
  (void)t;
  if (proto.type != JV_OBJ && proto.type != JV_NULL)
    return js_throw_type(ctx, "Object.create proto must be an object or null");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, proto.type == JV_OBJ ? proto.u.obj : NULL));
  if (props.type == JV_OBJ) {
    int i;
    KEEP(props);
    for (i = 0; i < props.u.obj->props.n && !ctx->has_exc; i++) {
      pentry *pe = &props.u.obj->props.e[i];
      if (!(pe->flags & P_ENUMERABLE))
        continue;
      define_one(ctx, o, pe->key, js_get(ctx, props, pe->key));
    }
  }
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value object_getproto(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  if (x.type == JV_OBJ && x.u.obj->proto)
    return js_obj_value(x.u.obj->proto);
  return js_null();
}
static js_value object_setproto(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0), p = ARG(1);
  (void)t;
  (void)ctx;
  if (x.type == JV_OBJ) {
    x.u.obj->proto = p.type == JV_OBJ ? p.u.obj : NULL;
    gc_write_barrier(x.u.obj);
  }
  return x;
}
static js_value object_freeze(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  int i;
  (void)t;
  (void)ctx;
  if (x.type == JV_OBJ) {
    x.u.obj->extensible = 0;
    for (i = 0; i < x.u.obj->props.n; i++)
      x.u.obj->props.e[i].flags &= (unsigned char)~(P_WRITABLE | P_CONFIGURABLE);
  }
  return x;
}
static js_value object_isfrozen(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)ctx;
  if (x.type != JV_OBJ)
    return js_bool(1);
  return js_bool(!x.u.obj->extensible);
}
/* HasProperty (walks the prototype chain), as ToPropertyDescriptor uses: an
 * attribute absent from the descriptor must be left unchanged when redefining
 * an existing property, which is indistinguishable from "present but undefined"
 * without this presence test. */
static int desc_has(js_object *o, const char *key)
{
  for (; o; o = o->proto)
    if (pmap_find_pub(o, key))
      return 1;
  return 0;
}
static int is_index_key(const char *key)
{
  unsigned i;
  if (!*key || (key[0] == '0' && key[1]))
    return 0;
  for (i = 0; key[i]; i++)
    if (key[i] < '0' || key[i] > '9')
      return 0;
  return i < 10;
}
/* Core of Object.defineProperty: apply descriptor `desc` to own property `key`
 * of `target`, preserving any attribute the descriptor omits. Returns -1 with a
 * pending exception on a bad descriptor, else 0. The caller must root `desc`
 * and keep `key` alive across the call. */
static int define_one(js_ctx *ctx, js_object *target, const char *key, js_value desc)
{
  js_value getter = js_undef(), setter = js_undef(), val = js_undef();
  pentry *cur;
  int has_value, has_writable, has_get, has_set, has_enum, has_conf, accessor;
  unsigned char flags;
  if (desc.type != JV_OBJ) {
    js_throw_type(ctx, "property descriptor must be an object");
    return -1;
  }
  gc_push_scope();
  KEEP(desc);

  has_value = desc_has(desc.u.obj, "value");
  has_writable = desc_has(desc.u.obj, "writable");
  has_get = desc_has(desc.u.obj, "get");
  has_set = desc_has(desc.u.obj, "set");
  has_enum = desc_has(desc.u.obj, "enumerable");
  has_conf = desc_has(desc.u.obj, "configurable");
  if ((has_get || has_set) && (has_value || has_writable)) {
    gc_pop_scope();
    js_throw_type(ctx, "property descriptor cannot be both accessor and data");
    return -1;
  }

  if (target->kind == OBJ_ARRAY && !has_get && !has_set && (is_index_key(key) || strcmp(key, "length") == 0)) {
    if (has_value)
      js_set(ctx, js_obj_value(target), key, js_get(ctx, desc, "value"));
    else if (strcmp(key, "length") != 0 && !js_has_own(target, key))
      js_set(ctx, js_obj_value(target), key, js_undef());
    gc_pop_scope();
    return 0;
  }

  cur = pmap_find_pub(target, key); /* existing own property, or NULL */
  flags = cur ? cur->flags : 0;     /* omitted attributes: keep existing, else default off */

  if (has_enum) {
    if (js_to_boolean(js_get(ctx, desc, "enumerable")))
      flags |= P_ENUMERABLE;
    else
      flags &= (unsigned char)~P_ENUMERABLE;
  }
  if (has_conf) {
    if (js_to_boolean(js_get(ctx, desc, "configurable")))
      flags |= P_CONFIGURABLE;
    else
      flags &= (unsigned char)~P_CONFIGURABLE;
  }

  /* Accessor if the descriptor supplies get/set, or it's silent on kind and the
   * existing property is already an accessor. */
  accessor = has_get || has_set || (!has_value && !has_writable && cur && (cur->flags & P_ACCESSOR));

  if (accessor) {
    if (has_get)
      getter = js_get(ctx, desc, "get");
    else if (cur && (cur->flags & P_ACCESSOR) && cur->getter)
      getter = js_obj_value(cur->getter);
    KEEP(getter);
    if (has_set)
      setter = js_get(ctx, desc, "set");
    else if (cur && (cur->flags & P_ACCESSOR) && cur->setter)
      setter = js_obj_value(cur->setter);
    KEEP(setter);
    js_def_accessor(target, key, js_is_callable(getter) ? getter.u.obj : NULL,
                    js_is_callable(setter) ? setter.u.obj : NULL, flags);
  } else {
    if (has_value)
      val = js_get(ctx, desc, "value");
    else if (cur && !(cur->flags & P_ACCESSOR))
      val = cur->value; /* preserve existing value */
    KEEP(val);
    if (has_writable) {
      if (js_to_boolean(js_get(ctx, desc, "writable")))
        flags |= P_WRITABLE;
      else
        flags &= (unsigned char)~P_WRITABLE;
    }
    flags &= (unsigned char)~P_ACCESSOR;
    js_def(target, key, val, flags);
  }
  gc_pop_scope();
  return 0;
}
/* Resolve a property-key value (Symbol or ToString) to its key string; *ks
 * holds a ToString result that must be kept alive while the key is used. */
static const char *to_key(js_ctx *ctx, js_value keyv, js_value *ks)
{
  *ks = js_undef();
  if (keyv.type == JV_SYMBOL)
    return js_symbol_key(keyv);
  *ks = js_to_string(ctx, keyv);
  if (ctx->has_exc || ks->type != JV_STR)
    return NULL;
  return ks->u.str->data;
}
static js_value object_defprop(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), desc = ARG(2), keyv = ARG(1), ks;
  const char *key;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "defineProperty requires objects");
  gc_push_scope();
  KEEP(o);
  KEEP(desc);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  if (!key) {
    gc_pop_scope();
    return ctx->has_exc ? js_undef() : js_throw_type(ctx, "invalid property key");
  }
  define_one(ctx, o.u.obj, key, desc);
  gc_pop_scope();
  return o;
}
static js_value object_defineprops(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), props = ARG(1);
  int i;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "defineProperties requires an object");
  if (props.type != JV_OBJ)
    return o;
  gc_push_scope();
  KEEP(o);
  KEEP(props);
  for (i = 0; i < props.u.obj->props.n && !ctx->has_exc; i++) {
    pentry *pe = &props.u.obj->props.e[i];
    if (!(pe->flags & P_ENUMERABLE))
      continue;
    define_one(ctx, o.u.obj, pe->key, js_get(ctx, props, pe->key));
  }
  gc_pop_scope();
  return o;
}
/* Build a property-descriptor object for own property `key`, or undefined if it
 * isn't an own property. */
static js_value make_desc(js_ctx *ctx, js_object *o, const char *key)
{
  pentry *pe = pmap_find_pub(o, key);
  js_object *d;
  js_value v;
  if (!pe && !js_has_own(o, key))
    return js_undef();
  gc_push_scope();
  d = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (pe && (pe->flags & P_ACCESSOR)) {
    js_def(d, "get", pe->getter ? js_obj_value(pe->getter) : js_undef(), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "set", pe->setter ? js_obj_value(pe->setter) : js_undef(), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "enumerable", js_bool(pe->flags & P_ENUMERABLE), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "configurable", js_bool(pe->flags & P_CONFIGURABLE), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  } else {
    int is_len = o->kind == OBJ_ARRAY && strcmp(key, "length") == 0;
    unsigned char f = pe ? pe->flags : (unsigned char)(P_WRITABLE | (is_len ? 0 : (P_ENUMERABLE | P_CONFIGURABLE)));
    v = pe ? pe->value : js_get(ctx, js_obj_value(o), key);
    gc_keep(refp(v));
    js_def(d, "value", v, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "writable", js_bool(f & P_WRITABLE), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "enumerable", js_bool(f & P_ENUMERABLE), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "configurable", js_bool(f & P_CONFIGURABLE), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  return js_obj_value(d);
}
static js_value object_getownpropdesc(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), ks;
  const char *key;
  (void)t;
  if (o.type != JV_OBJ)
    return js_undef();
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  if (!key) {
    gc_pop_scope();
    return js_undef();
  }
  {
    js_value r = make_desc(ctx, o.u.obj, key);
    gc_pop_scope();
    return r;
  }
}
static js_value object_getownpropdescs(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0);
  js_object *out;
  int i;
  (void)t;
  gc_push_scope();
  KEEP(o);
  out = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (o.type == JV_OBJ)
    for (i = 0; i < o.u.obj->props.n; i++) {
      const char *k = o.u.obj->props.e[i].key;
      js_value d = make_desc(ctx, o.u.obj, k);
      if (d.type == JV_OBJ)
        js_def(out, k, d, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value object_isextensible(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)ctx;
  (void)n;
  return js_bool(x.type == JV_OBJ && x.u.obj->extensible);
}
static js_value object_preventext(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)ctx;
  (void)n;
  if (x.type == JV_OBJ)
    x.u.obj->extensible = 0;
  return x;
}
static js_value object_seal(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  int i;
  (void)t;
  (void)ctx;
  (void)n;
  if (x.type == JV_OBJ) {
    x.u.obj->extensible = 0;
    for (i = 0; i < x.u.obj->props.n; i++)
      x.u.obj->props.e[i].flags &= (unsigned char)~P_CONFIGURABLE;
  }
  return x;
}
static js_value object_issealed(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  int i;
  (void)t;
  (void)ctx;
  (void)n;
  if (x.type != JV_OBJ)
    return js_bool(1);
  if (x.u.obj->extensible)
    return js_bool(0);
  for (i = 0; i < x.u.obj->props.n; i++)
    if (x.u.obj->props.e[i].flags & P_CONFIGURABLE)
      return js_bool(0);
  return js_bool(1);
}
static js_value object_is(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0), y = ARG(1);
  (void)t;
  (void)ctx;
  (void)n;
  if (x.type == JV_NUM && y.type == JV_NUM) {
    double dx = x.u.num, dy = y.u.num;
    if (dx != dx && dy != dy)
      return js_bool(1); /* NaN is NaN */
    if (dx == 0 && dy == 0)
      return js_bool(signbit(dx) == signbit(dy)); /* +0 / -0 differ */
    return js_bool(dx == dy);
  }
  return js_bool(js_strict_eq(x, y));
}
static js_value object_hasown(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), ks;
  const char *key;
  int r;
  (void)t;
  if (o.type != JV_OBJ)
    return js_bool(0);
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  r = key ? js_has_own(o.u.obj, key) : 0;
  gc_pop_scope();
  return js_bool(r);
}
static js_value proto_propisenum(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value keyv = ARG(0), ks;
  const char *key;
  pentry *pe;
  if (t.type != JV_OBJ)
    return js_bool(0);
  gc_push_scope();
  KEEP(t);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  pe = key ? pmap_find_pub(t.u.obj, key) : NULL;
  {
    int r = pe ? (pe->flags & P_ENUMERABLE) != 0
               : (key && t.u.obj->kind == OBJ_ARRAY && js_has_own(t.u.obj, key) && strcmp(key, "length") != 0);
    gc_pop_scope();
    return js_bool(r);
  }
}
static js_value object_groupby(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value items = ARG(0), fn = ARG(1);
  js_object *out, *arr;
  unsigned i;
  (void)t;
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "groupBy callback is not a function");
  arr = as_array(ctx, items);
  gc_push_scope();
  KEEP(items);
  KEEP(fn);
  out = (js_object *)gc_keep(js_new_object(ctx, NULL)); /* null-prototype object */
  if (arr)
    for (i = 0; i < arr->elen && !ctx->has_exc; i++) {
      js_value cargs[2], kv, ks, bucket;
      const char *key;
      cargs[0] = arr->elems[i];
      cargs[1] = js_number(i);
      kv = js_call(ctx, fn, js_undef(), cargs, 2);
      if (ctx->has_exc)
        break;
      ks = js_to_string(ctx, kv);
      if (ctx->has_exc)
        break;
      key = ks.u.str->data;
      bucket = js_get(ctx, js_obj_value(out), key);
      if (bucket.type != JV_OBJ || bucket.u.obj->kind != OBJ_ARRAY) {
        bucket = js_obj_value(js_new_array(ctx, 0));
        js_def(out, key, bucket, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
      }
      js_array_push(ctx, bucket.u.obj, arr->elems[i]);
    }
  gc_pop_scope();
  return js_obj_value(out);
}

/* ------------------------------------------------------- Reflect ---------- */

static int has_in_chain(js_object *o, const char *key)
{
  for (; o; o = o->proto)
    if (js_has_own(o, key))
      return 1;
  return 0;
}
static js_value reflect_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), ks;
  const char *key;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.get on non-object");
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  {
    js_value r = key ? js_get(ctx, o, key) : js_undef();
    gc_pop_scope();
    return r;
  }
}
static js_value reflect_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), v = ARG(2), ks;
  const char *key;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.set on non-object");
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  KEEP(v);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  if (key)
    js_set(ctx, o, key, v);
  gc_pop_scope();
  return js_bool(key != NULL && !ctx->has_exc);
}
static js_value reflect_has(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), ks;
  const char *key;
  int r;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.has on non-object");
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  r = key ? has_in_chain(o.u.obj, key) : 0;
  gc_pop_scope();
  return js_bool(r);
}
static js_value reflect_deleteprop(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), ks;
  const char *key;
  int r;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.deleteProperty on non-object");
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  r = key ? js_delete(o.u.obj, key) : 0;
  gc_pop_scope();
  return js_bool(r);
}
static js_value reflect_ownkeys(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0);
  js_object *arr, *s;
  int i;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.ownKeys on non-object");
  s = o.u.obj;
  gc_push_scope();
  KEEP(o);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (s->kind == OBJ_ARRAY) {
    unsigned e;
    for (e = 0; e < s->elen; e++) {
      char ib[24];
      u64toa((unsigned long long)e, ib, 10);
      js_array_push(ctx, arr, js_mk_cstring(ctx, ib));
    }
    js_array_push(ctx, arr, js_mk_cstring(ctx, "length"));
  }
  for (i = 0; i < s->props.n; i++) {
    const char *k = s->props.e[i].key;
    if ((unsigned char)k[0] >= 0x20)
      js_array_push(ctx, arr, js_mk_cstring(ctx, k));
  }
  if (ctx->sym_registry)
    for (i = 0; i < s->props.n; i++) {
      const char *k = s->props.e[i].key;
      unsigned r;
      if (!(k[0] == 1 && k[1] == 's'))
        continue;
      for (r = 0; r < ctx->sym_registry->elen; r++)
        if (strcmp(js_symbol_key(ctx->sym_registry->elems[r]), k) == 0) {
          js_array_push(ctx, arr, ctx->sym_registry->elems[r]);
          break;
        }
    }
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value reflect_defineprop(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0), keyv = ARG(1), desc = ARG(2), ks;
  const char *key;
  int rc;
  (void)t;
  if (o.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.defineProperty on non-object");
  gc_push_scope();
  KEEP(o);
  KEEP(keyv);
  KEEP(desc);
  key = to_key(ctx, keyv, &ks);
  if (ks.type == JV_STR)
    KEEP(ks);
  rc = key ? define_one(ctx, o.u.obj, key, desc) : -1;
  gc_pop_scope();
  if (rc < 0 && ctx->has_exc) {
    ctx->has_exc = 0;
    return js_bool(0);
  }
  return js_bool(rc == 0);
}
static js_value reflect_getownpropdesc(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return object_getownpropdesc(ctx, t, a, n);
}
static js_value reflect_getproto(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)n;
  if (x.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.getPrototypeOf on non-object");
  return x.u.obj->proto ? js_obj_value(x.u.obj->proto) : js_null();
}
static js_value reflect_setproto(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0), p = ARG(1);
  (void)t;
  (void)n;
  if (x.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.setPrototypeOf on non-object");
  x.u.obj->proto = p.type == JV_OBJ ? p.u.obj : NULL;
  gc_write_barrier(x.u.obj);
  return js_bool(1);
}
static js_value reflect_isext(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)n;
  if (x.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.isExtensible on non-object");
  return js_bool(x.u.obj->extensible);
}
static js_value reflect_preventext(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)t;
  (void)n;
  if (x.type != JV_OBJ)
    return js_throw_type(ctx, "Reflect.preventExtensions on non-object");
  x.u.obj->extensible = 0;
  return js_bool(1);
}
/* Spread an array-like's elements into a fresh argv; caller frees. */
static js_value *spread_args(js_ctx *ctx, js_value list, int *argc)
{
  js_object *arr = list.type == JV_OBJ ? as_array(ctx, list) : NULL;
  js_value *argv;
  unsigned i;
  *argc = arr ? (int)arr->elen : 0;
  argv = (js_value *)malloc((unsigned)(*argc > 0 ? *argc : 1) * sizeof(js_value));
  if (!argv) {
    js_throw_range(ctx, "out of memory");
    return NULL;
  }
  for (i = 0; arr && i < arr->elen; i++)
    argv[i] = arr->elems[i];
  return argv;
}
static js_value reflect_apply(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value fn = ARG(0), thisv = ARG(1), list = ARG(2), r;
  js_value *argv;
  int argc;
  (void)t;
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "Reflect.apply target is not a function");
  gc_push_scope();
  KEEP(fn);
  KEEP(thisv);
  KEEP(list);
  argv = spread_args(ctx, list, &argc);
  if (!argv) {
    gc_pop_scope();
    return js_undef();
  }
  {
    int i;
    for (i = 0; i < argc; i++)
      gc_keep(refp(argv[i]));
  }
  r = js_call(ctx, fn, thisv, argv, argc);
  free(argv);
  gc_pop_scope();
  return r;
}
static js_value reflect_construct(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value fn = ARG(0), list = ARG(1), r;
  js_value *argv;
  int argc;
  (void)t;
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "Reflect.construct target is not a function");
  gc_push_scope();
  KEEP(fn);
  KEEP(list);
  argv = spread_args(ctx, list, &argc);
  if (!argv) {
    gc_pop_scope();
    return js_undef();
  }
  {
    int i;
    for (i = 0; i < argc; i++)
      gc_keep(refp(argv[i]));
  }
  r = js_construct(ctx, fn, argv, argc);
  free(argv);
  gc_pop_scope();
  return r;
}

/* ------------------------------------------------------- Proxy ------------ *
 * A Proxy is an OBJ_PROXY object holding the target (@t) and handler (@h) as
 * hidden own props; the trap dispatch lives in value.c (js_proxy_*), hooked
 * into js_get/js_set, the in/delete operators, and call/construct. */
static js_value proxy_make(js_ctx *ctx, js_value target, js_value handler)
{
  js_object *o;
  if (target.type != JV_OBJ)
    return js_throw_type(ctx, "Cannot create proxy with a non-object as target");
  if (handler.type != JV_OBJ)
    return js_throw_type(ctx, "Cannot create proxy with a non-object as handler");
  gc_push_scope();
  KEEP(target);
  KEEP(handler);
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  o->kind = OBJ_PROXY;
  js_def(o, "@t", target, 0);
  js_def(o, "@h", handler, 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value proxy_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)n;
  return proxy_make(ctx, ARG(0), ARG(1));
}
static js_value proxy_revoke_fn(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value pv = ctx->native_data;
  (void)t;
  (void)a;
  (void)n;
  if (pv.type == JV_OBJ && pv.u.obj->kind == OBJ_PROXY) {
    js_def(pv.u.obj, "@t", js_null(), 0);
    js_def(pv.u.obj, "@h", js_null(), 0);
  }
  return js_undef();
}
static js_value proxy_revocable(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value p;
  js_object *res, *rev;
  unsigned char pf = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  (void)t;
  (void)n;
  p = proxy_make(ctx, ARG(0), ARG(1));
  if (ctx->has_exc)
    return js_undef();
  gc_push_scope();
  KEEP(p);
  res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  rev = (js_object *)gc_keep(js_new_native(ctx, proxy_revoke_fn, "", 0));
  rev->fn->data = p;
  gc_write_barrier(rev);
  js_def(res, "proxy", p, pf);
  js_def(res, "revoke", js_obj_value(rev), pf);
  gc_pop_scope();
  return js_obj_value(res);
}

/* ------------------------------------------------------- Array (iterators +
 * ES2022/2023) -- */

static js_value array_iter_next(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tgt = js_get(ctx, t, "@t");
  int i = js_to_int_sat(ctx, js_get(ctx, t, "@i"));
  int kind = js_to_int_sat(ctx, js_get(ctx, t, "@i2"));
  js_object *arr = (tgt.type == JV_OBJ && tgt.u.obj->kind == OBJ_ARRAY) ? tgt.u.obj : NULL;
  js_object *res;
  (void)a;
  (void)n;
  gc_push_scope();
  res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (!arr || i >= (int)arr->elen) {
    js_def(res, "value", js_undef(), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(res, "done", js_bool(1), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
    return js_obj_value(res);
  }
  js_set(ctx, t, "@i", js_number(i + 1));
  {
    js_value val;
    if (kind == 1)
      val = js_number(i);
    else if (kind == 2) {
      js_object *pair = js_new_array(ctx, 0);
      gc_keep(pair);
      js_array_push(ctx, pair, js_number(i));
      js_array_push(ctx, pair, arr->elems[i]);
      val = js_obj_value(pair);
    } else
      val = arr->elems[i];
    js_def(res, "value", val, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(res, "done", js_bool(0), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  return js_obj_value(res);
}
static js_value make_array_iter(js_ctx *ctx, js_value target, int kind)
{
  js_object *it;
  gc_push_scope();
  it = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  KEEP(target);
  js_def(it, "@t", target, P_WRITABLE);
  js_def(it, "@i", js_number(0), P_WRITABLE);
  js_def(it, "@i2", js_number(kind), P_WRITABLE);
  js_def_native(ctx, it, "next", array_iter_next, 0);
  {
    js_object *f = js_new_native(ctx, iter_self, "", 0);
    js_def(it, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  gc_pop_scope();
  return js_obj_value(it);
}
static js_value array_values_m(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_array_iter(ctx, t, 0);
}
static js_value array_keys_m(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_array_iter(ctx, t, 1);
}
static js_value array_entries_m(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_array_iter(ctx, t, 2);
}

/* String iterator: yields one code unit (byte) at a time, consistent with the
 * engine's byte-oriented string model (`length`, charAt, charCodeAt). Backs
 * `for (c of str)`, `[...str]`, and `String.prototype[Symbol.iterator]`. */
static js_value string_iter_next(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tgt = js_get(ctx, t, "@t");
  int i = js_to_int_sat(ctx, js_get(ctx, t, "@i"));
  js_object *res;
  (void)a;
  (void)n;
  gc_push_scope();
  res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (tgt.type != JV_STR || i >= (int)tgt.u.str->len) {
    js_def(res, "value", js_undef(), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(res, "done", js_bool(1), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
    return js_obj_value(res);
  }
  js_set(ctx, t, "@i", js_number(i + 1));
  js_def(res, "value", str_of(ctx, tgt.u.str->data + i, 1), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  js_def(res, "done", js_bool(0), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return js_obj_value(res);
}
static js_value string_values_m(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *it;
  js_value s = js_to_string(ctx, t);
  (void)a;
  (void)n;
  gc_push_scope();
  KEEP(s);
  it = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(it, "@t", s, P_WRITABLE);
  js_def(it, "@i", js_number(0), P_WRITABLE);
  js_def_native(ctx, it, "next", string_iter_next, 0);
  {
    js_object *f = js_new_native(ctx, iter_self, "", 0);
    js_def(it, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  gc_pop_scope();
  return js_obj_value(it);
}
static js_value array_at(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  int i;
  if (!arr)
    return js_undef();
  i = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  if (i < 0)
    i += (int)arr->elen;
  if (i < 0 || i >= (int)arr->elen)
    return js_undef();
  return arr->elems[i];
}
static js_value array_flatmap(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t), *out;
  js_value fn = ARG(0), self = ARG(1);
  unsigned i;
  if (!arr)
    return js_undef();
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "flatMap callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  KEEP(self);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < arr->elen && !ctx->has_exc; i++) {
    js_value cargs[3], r;
    cargs[0] = arr->elems[i];
    cargs[1] = js_number(i);
    cargs[2] = t;
    r = js_call(ctx, fn, self, cargs, 3);
    if (ctx->has_exc)
      break;
    if (r.type == JV_OBJ && r.u.obj->kind == OBJ_ARRAY) {
      unsigned j;
      for (j = 0; j < r.u.obj->elen; j++)
        js_array_push(ctx, out, r.u.obj->elems[j]);
    } else
      js_array_push(ctx, out, r);
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value array_findlast(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value fn = ARG(0), self = ARG(1);
  int i;
  if (!arr)
    return js_undef();
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "findLast callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  KEEP(self);
  for (i = (int)arr->elen - 1; i >= 0 && !ctx->has_exc; i--) {
    js_value cargs[3], r;
    cargs[0] = arr->elems[i];
    cargs[1] = js_number(i);
    cargs[2] = t;
    r = js_call(ctx, fn, self, cargs, 3);
    if (!ctx->has_exc && js_to_boolean(r)) {
      js_value v = arr->elems[i];
      gc_pop_scope();
      return v;
    }
  }
  gc_pop_scope();
  return js_undef();
}
static js_value array_findlastindex(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value fn = ARG(0), self = ARG(1);
  int i;
  if (!arr)
    return js_number(-1);
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "findLastIndex callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  KEEP(self);
  for (i = (int)arr->elen - 1; i >= 0 && !ctx->has_exc; i--) {
    js_value cargs[3], r;
    cargs[0] = arr->elems[i];
    cargs[1] = js_number(i);
    cargs[2] = t;
    r = js_call(ctx, fn, self, cargs, 3);
    if (!ctx->has_exc && js_to_boolean(r)) {
      gc_pop_scope();
      return js_number(i);
    }
  }
  gc_pop_scope();
  return js_number(-1);
}
static js_value array_reduceright(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value fn = ARG(0), acc;
  int i;
  if (!arr)
    return js_undef();
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "reduceRight callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  i = (int)arr->elen - 1;
  if (n > 1)
    acc = a[1];
  else {
    if (arr->elen == 0) {
      gc_pop_scope();
      return js_throw_type(ctx, "reduce of empty array with no initial value");
    }
    acc = arr->elems[i];
    i--;
  }
  gc_keep(refp(acc));
  for (; i >= 0 && !ctx->has_exc; i--) {
    js_value cargs[4];
    cargs[0] = acc;
    cargs[1] = arr->elems[i];
    cargs[2] = js_number(i);
    cargs[3] = t;
    acc = js_call(ctx, fn, js_undef(), cargs, 4);
    KEEP(acc);
  }
  gc_pop_scope();
  return acc;
}
static js_value array_copywithin(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  int len, target, start, end;
  js_value *tmp;
  int i, count;
  if (!arr)
    return t;
  len = (int)arr->elen;
  target = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  start = n > 1 ? js_to_int_sat(ctx, a[1]) : 0;
  end = n > 2 && a[2].type != JV_UNDEF ? js_to_int_sat(ctx, a[2]) : len;
  if (target < 0)
    target += len;
  if (target < 0)
    target = 0;
  if (target > len)
    target = len;
  if (start < 0)
    start += len;
  if (start < 0)
    start = 0;
  if (start > len)
    start = len;
  if (end < 0)
    end += len;
  if (end < 0)
    end = 0;
  if (end > len)
    end = len;
  count = end - start;
  if (count > len - target)
    count = len - target;
  if (count <= 0)
    return t;
  tmp = (js_value *)malloc((unsigned)count * sizeof(js_value));
  if (!tmp)
    return js_throw_range(ctx, "out of memory");
  for (i = 0; i < count; i++)
    tmp[i] = arr->elems[start + i];
  for (i = 0; i < count; i++)
    arr->elems[target + i] = tmp[i];
  free(tmp);
  return t;
}
static js_value object_ownnames(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0);
  js_object *arr, *s;
  int i;
  (void)t;
  if (o.type != JV_OBJ)
    return js_obj_value(js_new_array(ctx, 0));
  s = o.u.obj;
  gc_push_scope();
  KEEP(o);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (s->kind == OBJ_ARRAY) {
    unsigned e;
    for (e = 0; e < s->elen; e++) {
      char ib[24];
      u64toa((unsigned long long)e, ib, 10);
      js_array_push(ctx, arr, js_mk_cstring(ctx, ib));
    }
    js_array_push(ctx, arr, js_mk_cstring(ctx, "length"));
  }
  for (i = 0; i < s->props.n; i++) {
    const char *k = s->props.e[i].key;
    if (k[0] == 1)
      continue; /* a Symbol key — not a string name */
    js_array_push(ctx, arr, js_mk_cstring(ctx, k));
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

/* Object.getOwnPropertySymbols: the Symbol-keyed own properties, as Symbols.
 * Internal keys are the symbol's host string ("\x01s<id>"); the per-context
 * registry maps each such string back to the live Symbol object. */
static js_value object_getownsymbols(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value o = ARG(0);
  js_object *arr, *s, *reg = ctx->sym_registry;
  int i;
  (void)t;
  gc_push_scope();
  KEEP(o);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (o.type != JV_OBJ || !reg) {
    gc_pop_scope();
    return js_obj_value(arr);
  }
  s = o.u.obj;
  for (i = 0; i < s->props.n; i++) {
    const char *key = s->props.e[i].key;
    unsigned r;
    if (!(key[0] == 1 && key[1] == 's'))
      continue; /* not a Symbol key */
    for (r = 0; r < reg->elen; r++)
      if (strcmp(js_symbol_key(reg->elems[r]), key) == 0) {
        js_array_push(ctx, arr, reg->elems[r]);
        break;
      }
  }
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value object_fromentries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value it = ARG(0);
  js_object *obj;
  unsigned i;
  (void)t;
  gc_push_scope();
  KEEP(it);
  obj = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (it.type == JV_OBJ && it.u.obj->kind == OBJ_ARRAY)
    for (i = 0; i < it.u.obj->elen; i++) {
      js_value pair = it.u.obj->elems[i];
      js_value k = js_get_index(ctx, pair, 0), v = js_get_index(ctx, pair, 1);
      js_value ks = js_to_string(ctx, k);
      if (!ctx->has_exc)
        js_set(ctx, js_obj_value(obj), ks.u.str->data, v);
    }
  gc_pop_scope();
  return js_obj_value(obj);
}
static js_value op_hasown(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value kv = ARG(0), k;
  const char *key;
  if (t.type != JV_OBJ)
    return js_bool(0);
  if (kv.type == JV_SYMBOL)
    key = js_symbol_key(kv);
  else {
    k = js_to_string(ctx, kv);
    if (ctx->has_exc || k.type != JV_STR) {
      ctx->has_exc = 0;
      ctx->exc_root = 0;
      return js_bool(0);
    }
    key = k.u.str->data;
  }
  return js_bool(js_has_own(t.u.obj, key));
}
/* Object.prototype.toString: "[object Tag]" per the spec's builtinTag, with a
 * string Symbol.toStringTag overriding it. */
static js_value op_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *tag = "Object";
  char buf[80];
  int i = 0;
  const char *p;
  (void)a;
  (void)n;
  if (t.type == JV_UNDEF)
    tag = "Undefined";
  else if (t.type == JV_NULL)
    tag = "Null";
  else if (t.type == JV_NUM)
    tag = "Number";
  else if (t.type == JV_BOOL)
    tag = "Boolean";
  else if (t.type == JV_STR)
    tag = "String";
  else if (t.type == JV_SYMBOL)
    tag = "Symbol";
  else if (t.type == JV_BIGINT)
    tag = "BigInt";
  else if (t.type == JV_OBJ) {
    js_object *o = t.u.obj;
    if (o->kind == OBJ_ARRAY)
      tag = "Array";
    else if (o->kind == OBJ_FUNCTION)
      tag = "Function";
    else if (o->kind == OBJ_ERROR)
      tag = "Error";
    if (ctx->sym_tostringtag.type == JV_SYMBOL) {
      js_value tt = js_get(ctx, t, js_symbol_key(ctx->sym_tostringtag));
      if (tt.type == JV_STR)
        tag = tt.u.str->data; /* copied into buf below before any alloc */
    }
  }
  for (p = "[object "; *p; p++)
    buf[i++] = *p;
  for (p = tag; *p && i < 70; p++)
    buf[i++] = *p;
  buf[i++] = ']';
  buf[i] = 0;
  return js_mk_cstring(ctx, buf);
}
static js_value function_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  char buf[160];
  int i = 0;
  const char *p, *nm = "";
  (void)a;
  (void)n;
  if (t.type != JV_OBJ || t.u.obj->kind != OBJ_FUNCTION)
    return js_throw_type(ctx, "Function.prototype.toString requires a function");
  if (t.u.obj->fn && t.u.obj->fn->name)
    nm = t.u.obj->fn->name;
  for (p = "function "; *p; p++)
    buf[i++] = *p;
  for (p = nm; *p && i < 130; p++)
    buf[i++] = *p;
  for (p = "() { [native code] }"; *p; p++)
    buf[i++] = *p;
  buf[i] = 0;
  return js_mk_cstring(ctx, buf);
}
static js_value op_valueof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)a;
  (void)n;
  return t;
}
static js_value op_isproto(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  js_object *o;
  (void)ctx;
  if (x.type != JV_OBJ || t.type != JV_OBJ)
    return js_bool(0);
  for (o = x.u.obj->proto; o; o = o->proto)
    if (o == t.u.obj)
      return js_bool(1);
  return js_bool(0);
}

/* ------------------------------------------------------- Array ------------ */

static js_object *as_array(js_ctx *ctx, js_value t)
{
  if (t.type == JV_OBJ && t.u.obj->kind == OBJ_ARRAY)
    return t.u.obj;
  /* Array.prototype methods are generic (ES §22.1.3): they run on any
   * array-like `this` — `arguments`, a DOM collection, `{length, 0:…}`, a
   * string. Materialize a real array from `.length` + indexed elements so the
   * method body (written against the dense element vector) works. Mutating
   * methods then operate on this copy; `push` is separately generic so it
   * mutates the original array-like. */
  if (t.type == JV_OBJ || t.type == JV_STR) {
    unsigned len = js_to_uint32(ctx, js_get(ctx, t, "length")), i;
    js_object *arr;
    gc_push_scope();
    KEEP(t);
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < len; i++) {
      js_array_push(ctx, arr, js_get_index(ctx, t, i));
      if (ctx->has_exc)
        break;
    }
    gc_pop_scope();
    return ctx->has_exc ? NULL : arr;
  }
  js_throw_type(ctx, "Array.prototype method called on null or undefined");
  return NULL;
}

static js_value native_array(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr;
  int i;
  (void)t;
  if (n == 1 && a[0].type == JV_NUM) {
    arr = js_new_array(ctx, (unsigned)a[0].u.num);
    return js_obj_value(arr);
  }
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < n; i++)
    js_array_push(ctx, arr, a[i]);
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value array_isarray(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  return js_bool(ARG(0).type == JV_OBJ && ARG(0).u.obj->kind == OBJ_ARRAY);
}
static js_value array_of(js_ctx *ctx, js_value t, js_value *a, int n) { return native_array(ctx, t, a, n); }
static js_value array_from(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value src = ARG(0), fn = ARG(1);
  js_object *arr;
  unsigned i, len;
  int has_fn = js_is_callable(fn);
  (void)t;
  gc_push_scope();
  KEEP(src);
  KEEP(fn);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  /* Iterable (has a callable Symbol.iterator) that isn't a plain Array/String:
   * drive the iterator protocol — Map/Set, generators, the array iterators. */
  if (src.type == JV_OBJ && src.u.obj->kind != OBJ_ARRAY) {
    js_value itfn = js_get(ctx, src, js_symbol_key(ctx->sym_iterator));
    if (js_is_callable(itfn)) {
      js_value iter = js_call(ctx, itfn, src, NULL, 0);
      unsigned idx = 0;
      KEEP(iter);
      while (!ctx->has_exc && iter.type == JV_OBJ) {
        js_value nextfn = js_get(ctx, iter, "next"), res, val;
        if (!js_is_callable(nextfn))
          break;
        res = js_call(ctx, nextfn, iter, NULL, 0);
        if (ctx->has_exc || res.type != JV_OBJ)
          break;
        if (js_to_boolean(js_get(ctx, res, "done")))
          break;
        val = js_get(ctx, res, "value");
        KEEP(val);
        if (has_fn) {
          js_value cargs[2];
          cargs[0] = val;
          cargs[1] = js_number(idx);
          val = js_call(ctx, fn, js_undef(), cargs, 2);
        }
        js_array_push(ctx, arr, val);
        idx++;
      }
      gc_pop_scope();
      return js_obj_value(arr);
    }
  }
  if (src.type == JV_OBJ && src.u.obj->kind == OBJ_ARRAY)
    len = src.u.obj->elen;
  else if (src.type == JV_STR)
    len = (unsigned)src.u.str->len;
  else if (src.type == JV_OBJ) {
    js_value l = js_get(ctx, src, "length");
    len = js_to_uint32(ctx, l);
  } else
    len = 0;
  for (i = 0; i < len && !ctx->has_exc; i++) {
    js_value e = js_get_index(ctx, src, i);
    if (has_fn) {
      js_value cargs[2];
      cargs[0] = e;
      cargs[1] = js_number(i);
      e = js_call(ctx, fn, js_undef(), cargs, 2);
    }
    js_array_push(ctx, arr, e);
  }
  gc_pop_scope();
  return js_obj_value(arr);
}
static js_value array_push(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int i;
  if (t.type != JV_OBJ)
    return js_throw_type(ctx, "Array.prototype.push called on null or undefined");
  if (t.u.obj->kind == OBJ_ARRAY) { /* real array: fast dense path */
    js_object *arr = t.u.obj;
    for (i = 0; i < n; i++)
      js_array_push(ctx, arr, a[i]);
    return js_number(arr->elen);
  }
  /* generic array-like: append at [length..] and update length, so push mutates
   * the original object (materializing a copy, as the read methods do, would
   * silently drop the appends). */
  {
    unsigned len = js_to_uint32(ctx, js_get(ctx, t, "length"));
    gc_push_scope();
    KEEP(t);
    {
      int k;
      for (k = 0; k < n; k++)
        KEEP(a[k]);
    }
    for (i = 0; i < n; i++)
      js_set_index(ctx, t, len + (unsigned)i, a[i]);
    js_set(ctx, t, "length", js_number((double)(len + (unsigned)n)));
    gc_pop_scope();
    return js_number((double)(len + (unsigned)n));
  }
}
static js_value array_pop(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value v;
  (void)a;
  (void)n;
  if (!arr)
    return js_undef();
  if (arr->elen == 0)
    return js_undef();
  v = arr->elems[--arr->elen];
  return v;
}
static js_value array_shift(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value v;
  unsigned i;
  (void)a;
  (void)n;
  if (!arr || arr->elen == 0)
    return js_undef();
  v = arr->elems[0];
  for (i = 1; i < arr->elen; i++)
    arr->elems[i - 1] = arr->elems[i];
  arr->elen--;
  return v;
}
static js_value array_unshift(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  int i;
  unsigned j;
  if (!arr)
    return js_undef();
  gc_push_scope();
  {
    int k;
    for (k = 0; k < n; k++)
      KEEP(a[k]);
  }
  js_array_set_len(ctx, arr, arr->elen + (unsigned)n);
  for (j = arr->elen; j > (unsigned)n; j--)
    arr->elems[j - 1] = arr->elems[j - 1 - n];
  for (i = 0; i < n; i++)
    arr->elems[i] = a[i];
  gc_pop_scope();
  return js_number(arr->elen);
}
static js_value array_slice(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t), *out;
  int start, end, i;
  if (!arr)
    return js_undef();
  start = n > 0 ? clamp_index(js_to_number(ctx, a[0]), (int)arr->elen) : 0;
  end = n > 1 && a[1].type != JV_UNDEF ? clamp_index(js_to_number(ctx, a[1]), (int)arr->elen) : (int)arr->elen;
  gc_push_scope();
  KEEP(t);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = start; i < end; i++)
    js_array_push(ctx, out, arr->elems[i]);
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value array_splice(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t), *removed;
  int start, del, i, ins = n > 2 ? n - 2 : 0;
  if (!arr)
    return js_undef();
  start = n > 0 ? clamp_index(js_to_number(ctx, a[0]), (int)arr->elen) : 0;
  del = n > 1 ? js_to_int_sat(ctx, a[1]) : (int)arr->elen - start;
  if (del < 0)
    del = 0;
  if (start + del > (int)arr->elen)
    del = (int)arr->elen - start;
  gc_push_scope();
  KEEP(t);
  {
    int k;
    for (k = 0; k < n; k++)
      KEEP(a[k]);
  }
  removed = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < del; i++)
    js_array_push(ctx, removed, arr->elems[start + i]);
  if (ins != del) {
    unsigned oldlen = arr->elen, newlen = oldlen - (unsigned)del + (unsigned)ins;
    if (ins > del) {
      js_array_set_len(ctx, arr, newlen);
      for (i = (int)oldlen - 1; i >= start + del; i--)
        arr->elems[i + ins - del] = arr->elems[i];
    } else {
      for (i = start + del; i < (int)oldlen; i++)
        arr->elems[i + ins - del] = arr->elems[i];
      arr->elen = newlen;
    }
  }
  for (i = 0; i < ins; i++)
    arr->elems[start + i] = a[i + 2];
  gc_pop_scope();
  return js_obj_value(removed);
}
static js_value array_concat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *out;
  int i;
  gc_push_scope();
  KEEP(t);
  {
    int k;
    for (k = 0; k < n; k++)
      KEEP(a[k]);
  }
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (t.type == JV_OBJ && t.u.obj->kind == OBJ_ARRAY) {
    unsigned e;
    for (e = 0; e < t.u.obj->elen; e++)
      js_array_push(ctx, out, t.u.obj->elems[e]);
  } else
    js_array_push(ctx, out, t);
  for (i = 0; i < n; i++) {
    if (a[i].type == JV_OBJ && a[i].u.obj->kind == OBJ_ARRAY) {
      unsigned e;
      for (e = 0; e < a[i].u.obj->elen; e++)
        js_array_push(ctx, out, a[i].u.obj->elems[e]);
    } else
      js_array_push(ctx, out, a[i]);
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value array_join(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value sep = n > 0 && a[0].type != JV_UNDEF ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, ",");
  sbuf sb;
  unsigned i;
  if (!arr)
    return js_undef();
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  KEEP(t);
  KEEP(sep);
  for (i = 0; i < arr->elen; i++) {
    if (i)
      sb_put(&sb, sep.u.str->data, sep.u.str->len);
    if (!nullish(arr->elems[i])) {
      js_value s = js_to_string(ctx, arr->elems[i]);
      if (ctx->has_exc)
        break;
      sb_put(&sb, s.u.str->data, s.u.str->len);
    }
  }
  gc_pop_scope();
  {
    js_value r = str_of(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
}
static js_value array_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return array_join(ctx, t, a, 0);
  (void)n;
}
static js_value array_indexof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  unsigned i;
  js_value x = ARG(0);
  int from = n > 1 ? js_to_int_sat(ctx, a[1]) : 0;
  if (!arr)
    return js_undef();
  if (from < 0)
    from += (int)arr->elen;
  if (from < 0)
    from = 0;
  for (i = (unsigned)from; i < arr->elen; i++)
    if (js_strict_eq(arr->elems[i], x))
      return js_number(i);
  return js_number(-1);
}
static js_value array_lastindexof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  int i;
  js_value x = ARG(0);
  (void)n;
  if (!arr)
    return js_undef();
  for (i = (int)arr->elen - 1; i >= 0; i--)
    if (js_strict_eq(arr->elems[i], x))
      return js_number(i);
  return js_number(-1);
}
static js_value array_includes(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  unsigned i;
  js_value x = ARG(0);
  (void)n;
  if (!arr)
    return js_undef();
  for (i = 0; i < arr->elen; i++)
    if (js_strict_eq(arr->elems[i], x))
      return js_bool(1);
  return js_bool(0);
}
/* shared iteration core for forEach/map/filter/some/every/find/findIndex */
static js_value array_iter(js_ctx *ctx, js_value t, js_value *a, int n, int mode)
{
  /* mode: 0 forEach 1 map 2 filter 3 some 4 every 5 find 6 findIndex */
  js_object *arr = as_array(ctx, t), *out = NULL;
  js_value fn = ARG(0), self = ARG(1);
  unsigned i;
  if (!arr)
    return js_undef();
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  KEEP(self);
  if (mode == 1 || mode == 2)
    out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < arr->elen && !ctx->has_exc; i++) {
    js_value cargs[3], r;
    cargs[0] = arr->elems[i];
    cargs[1] = js_number(i);
    cargs[2] = t;
    r = js_call(ctx, fn, self, cargs, 3);
    if (ctx->has_exc)
      break;
    switch (mode) {
    case 1:
      js_array_push(ctx, out, r);
      break;
    case 2:
      if (js_to_boolean(r))
        js_array_push(ctx, out, cargs[0]);
      break;
    case 3:
      if (js_to_boolean(r)) {
        gc_pop_scope();
        return js_bool(1);
      }
      break;
    case 4:
      if (!js_to_boolean(r)) {
        gc_pop_scope();
        return js_bool(0);
      }
      break;
    case 5:
      if (js_to_boolean(r)) {
        gc_pop_scope();
        return cargs[0];
      }
      break;
    case 6:
      if (js_to_boolean(r)) {
        gc_pop_scope();
        return js_number(i);
      }
      break;
    default:
      break;
    }
  }
  gc_pop_scope();
  if (mode == 1 || mode == 2)
    return js_obj_value(out);
  if (mode == 3)
    return js_bool(0);
  if (mode == 4)
    return js_bool(1);
  if (mode == 5)
    return js_undef();
  if (mode == 6)
    return js_number(-1);
  return js_undef();
}
static js_value array_foreach(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 0); }
static js_value array_map(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 1); }
static js_value array_filter(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 2); }
static js_value array_some(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 3); }
static js_value array_every(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 4); }
static js_value array_find(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 5); }
static js_value array_findindex(js_ctx *ctx, js_value t, js_value *a, int n) { return array_iter(ctx, t, a, n, 6); }
static js_value array_reduce(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value fn = ARG(0), acc;
  unsigned i = 0;
  if (!arr)
    return js_undef();
  if (!js_is_callable(fn))
    return js_throw_type(ctx, "reduce callback is not a function");
  gc_push_scope();
  KEEP(t);
  KEEP(fn);
  if (n > 1)
    acc = a[1];
  else {
    if (arr->elen == 0) {
      gc_pop_scope();
      return js_throw_type(ctx, "reduce of empty array with no initial value");
    }
    acc = arr->elems[0];
    i = 1;
  }
  gc_keep(refp(acc));
  for (; i < arr->elen && !ctx->has_exc; i++) {
    js_value cargs[4];
    cargs[0] = acc;
    cargs[1] = arr->elems[i];
    cargs[2] = js_number(i);
    cargs[3] = t;
    acc = js_call(ctx, fn, js_undef(), cargs, 4);
    KEEP(acc);
  }
  gc_pop_scope();
  return acc;
}
static js_value array_reverse(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  unsigned i;
  (void)a;
  (void)n;
  if (!arr)
    return js_undef();
  for (i = 0; i < arr->elen / 2; i++) {
    js_value tmp = arr->elems[i];
    arr->elems[i] = arr->elems[arr->elen - 1 - i];
    arr->elems[arr->elen - 1 - i] = tmp;
  }
  return t;
}
static js_value array_fill(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  int start, end, i;
  js_value v = ARG(0);
  if (!arr)
    return js_undef();
  start = n > 1 ? clamp_index(js_to_number(ctx, a[1]), (int)arr->elen) : 0;
  end = n > 2 ? clamp_index(js_to_number(ctx, a[2]), (int)arr->elen) : (int)arr->elen;
  for (i = start; i < end; i++)
    arr->elems[i] = v;
  return t;
}
static js_value array_flat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t), *out;
  unsigned i;
  int depth = n > 0 ? js_to_int_sat(ctx, a[0]) : 1;
  if (!arr)
    return js_undef();
  gc_push_scope();
  KEEP(t);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < arr->elen; i++) {
    js_value e = arr->elems[i];
    if (depth > 0 && e.type == JV_OBJ && e.u.obj->kind == OBJ_ARRAY) {
      unsigned j;
      for (j = 0; j < e.u.obj->elen; j++)
        js_array_push(ctx, out, e.u.obj->elems[j]);
    } else
      js_array_push(ctx, out, e);
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value array_sort(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *arr = as_array(ctx, t);
  js_value cmp = ARG(0);
  int has_cmp = js_is_callable(cmp);
  unsigned i, j;
  (void)n;
  if (!arr)
    return js_undef();
  gc_push_scope();
  KEEP(t);
  KEEP(cmp);
  for (i = 1; i < arr->elen; i++) { /* insertion sort (stable) */
    js_value key = arr->elems[i];
    j = i;
    while (j > 0 && !ctx->has_exc) {
      int swap;
      if (has_cmp) {
        js_value cargs[2];
        js_value r;
        cargs[0] = arr->elems[j - 1];
        cargs[1] = key;
        r = js_call(ctx, cmp, js_undef(), cargs, 2);
        swap = js_to_number(ctx, r) > 0;
      } else {
        js_value sa = js_to_string(ctx, arr->elems[j - 1]), sb = js_to_string(ctx, key);
        swap = sa.u.str->len && sb.u.str->len
                   ? (memcmp_(sa.u.str->data, sb.u.str->data,
                              sa.u.str->len < sb.u.str->len ? sa.u.str->len : sb.u.str->len) > 0 ||
                      (memcmp_(sa.u.str->data, sb.u.str->data,
                               sa.u.str->len < sb.u.str->len ? sa.u.str->len : sb.u.str->len) == 0 &&
                       sa.u.str->len > sb.u.str->len))
                   : sa.u.str->len > sb.u.str->len;
      }
      if (!swap)
        break;
      arr->elems[j] = arr->elems[j - 1];
      j--;
    }
    arr->elems[j] = key;
  }
  gc_pop_scope();
  return t;
}

/* ------------------------------------------------------- String ----------- */

static js_value this_str(js_ctx *ctx, js_value t) { return js_to_string(ctx, t); }

/* No Unicode tables: normalize() keeps the text as is */
static js_value str_normalize(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return this_str(ctx, t);
}
static js_value string_fromcharcode(js_ctx *ctx, js_value t, js_value *a, int n)
{
  sbuf sb;
  int i;
  (void)t;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  for (i = 0; i < n; i++) {
    char tmp[4];
    unsigned cp = js_to_uint32(ctx, a[i]) & 0xFFFF;
    int k;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { /* join a surrogate pair */
      unsigned lo = js_to_uint32(ctx, a[i + 1]) & 0xFFFF;
      if (lo >= 0xDC00 && lo < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    k = utf8_encode(cp, tmp);
    sb_put(&sb, tmp, k);
  }
  {
    js_value r = str_of(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
}
static js_value string_fromcodepoint(js_ctx *ctx, js_value t, js_value *a, int n)
{
  sbuf sb;
  int i;
  js_value r;
  (void)t;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  for (i = 0; i < n; i++) {
    char tmp[4];
    double d = js_to_number(ctx, a[i]);
    if (!(d >= 0 && d <= 0x10FFFF) || d != (double)(unsigned)d) {
      free(sb.b);
      return js_throw_range(ctx, "Invalid code point");
    }
    sb_put(&sb, tmp, utf8_encode((unsigned)d, tmp));
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}
/* String.raw`...` — concatenate the template's raw cooked-less segments
 * (strings.raw) interleaved with the substitutions. Used by tagged-template
 * desugaring. */
static js_value string_raw(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tmpl = ARG(0), raw, out;
  sbuf sb;
  int len, i;
  (void)t;
  if (tmpl.type != JV_OBJ)
    return js_mk_cstring(ctx, "");
  raw = js_get(ctx, tmpl, "raw");
  if (raw.type != JV_OBJ)
    return js_mk_cstring(ctx, "");
  len = (int)js_to_uint32(ctx, js_get(ctx, raw, "length"));
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  KEEP(raw);
  for (i = 0; i < len; i++) {
    js_value seg = js_to_string(ctx, js_get_index(ctx, raw, (unsigned)i));
    if (seg.type == JV_STR)
      sb_put(&sb, seg.u.str->data, seg.u.str->len);
    if (i + 1 < len && i + 1 < n) {
      js_value sub = js_to_string(ctx, a[i + 1]);
      if (sub.type == JV_STR)
        sb_put(&sb, sub.u.str->data, sub.u.str->len);
    }
  }
  gc_pop_scope();
  out = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return out;
}
static js_value str_charat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int i = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  if (i < 0 || i >= s.u.str->len)
    return js_mk_cstring(ctx, "");
  return str_of(ctx, s.u.str->data + i, 1);
}
static js_value str_at(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int i = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  if (i < 0)
    i += s.u.str->len;
  if (i < 0 || i >= s.u.str->len)
    return js_undef();
  return str_of(ctx, s.u.str->data + i, 1);
}
static js_value str_charcode(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int i = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  if (i < 0 || i >= s.u.str->len)
    return js_number(0.0 / 0.0);
  return js_number((unsigned char)s.u.str->data[i]);
}
static js_value str_localecompare(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), o = js_to_string(ctx, ARG(0));
  (void)n;
  const unsigned char *pa = (const unsigned char *)s.u.str->data;
  const unsigned char *pb = (const unsigned char *)o.u.str->data;
  int la = s.u.str->len, lb = o.u.str->len, i;
  for (i = 0; i < la && i < lb; i++)
    if (pa[i] != pb[i])
      return js_number(pa[i] < pb[i] ? -1 : 1);
  return js_number(la < lb ? -1 : la > lb ? 1 : 0);
}
static js_value str_indexof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), needle = js_to_string(ctx, ARG(0));
  int from = n > 1 ? js_to_int_sat(ctx, a[1]) : 0;
  if (from < 0)
    from = 0;
  return js_number(find_sub(s.u.str->data, s.u.str->len, needle.u.str->data, needle.u.str->len, from));
}
static js_value str_lastindexof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), needle = js_to_string(ctx, ARG(0));
  int i, last = -1;
  (void)n;
  for (i = 0; (i = find_sub(s.u.str->data, s.u.str->len, needle.u.str->data, needle.u.str->len, i)) >= 0; i++)
    last = i;
  return js_number(last);
}
static js_value str_includes(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), needle = js_to_string(ctx, ARG(0));
  (void)n;
  return js_bool(find_sub(s.u.str->data, s.u.str->len, needle.u.str->data, needle.u.str->len, 0) >= 0);
}
static js_value str_startswith(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), p = js_to_string(ctx, ARG(0));
  int from = n > 1 ? js_to_int_sat(ctx, a[1]) : 0;
  if (from < 0)
    from = 0;
  if (from + p.u.str->len > s.u.str->len)
    return js_bool(0);
  return js_bool(memcmp_(s.u.str->data + from, p.u.str->data, p.u.str->len) == 0);
}
static js_value str_endswith(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), p = js_to_string(ctx, ARG(0));
  int end = n > 1 ? js_to_int_sat(ctx, a[1]) : s.u.str->len;
  if (end > s.u.str->len)
    end = s.u.str->len;
  if (p.u.str->len > end)
    return js_bool(0);
  return js_bool(memcmp_(s.u.str->data + end - p.u.str->len, p.u.str->data, p.u.str->len) == 0);
}
static js_value str_slice(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int len = s.u.str->len;
  int start = n > 0 ? clamp_index(js_to_number(ctx, a[0]), len) : 0;
  int end = n > 1 && a[1].type != JV_UNDEF ? clamp_index(js_to_number(ctx, a[1]), len) : len;
  if (end < start)
    end = start;
  return str_of(ctx, s.u.str->data + start, end - start);
}
static js_value str_substring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int len = s.u.str->len, tmp;
  int start = n > 0 ? clamp_index(js_to_number(ctx, a[0]), len) : 0;
  int end = n > 1 && a[1].type != JV_UNDEF ? clamp_index(js_to_number(ctx, a[1]), len) : len;
  if (start > end) {
    tmp = start;
    start = end;
    end = tmp;
  }
  return str_of(ctx, s.u.str->data + start, end - start);
}
static js_value str_substr(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int len = s.u.str->len;
  int start = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  int cnt = n > 1 ? js_to_int_sat(ctx, a[1]) : len;
  if (start < 0) {
    start += len;
    if (start < 0)
      start = 0;
  }
  if (start > len)
    start = len;
  if (cnt < 0)
    cnt = 0;
  if (start + cnt > len)
    cnt = len - start;
  return str_of(ctx, s.u.str->data + start, cnt);
}
static js_value str_case(js_ctx *ctx, js_value t, int upper)
{
  js_value s = this_str(ctx, t);
  char *buf = (char *)malloc((unsigned)s.u.str->len + 1);
  int i;
  if (!buf)
    return js_throw_range(ctx, "out of memory");
  for (i = 0; i < s.u.str->len; i++) {
    char c = s.u.str->data[i];
    if (upper && c >= 'a' && c <= 'z')
      c = (char)(c - 32);
    else if (!upper && c >= 'A' && c <= 'Z')
      c = (char)(c + 32);
    buf[i] = c;
  }
  {
    js_value r = str_of(ctx, buf, s.u.str->len);
    free(buf);
    return r;
  }
}
static js_value str_upper(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return str_case(ctx, t, 1);
}
static js_value str_lower(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return str_case(ctx, t, 0);
}
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static js_value str_trim(js_ctx *ctx, js_value t, int left, int right)
{
  js_value s = this_str(ctx, t);
  int a = 0, b = s.u.str->len;
  if (left)
    while (a < b && is_ws(s.u.str->data[a]))
      a++;
  if (right)
    while (b > a && is_ws(s.u.str->data[b - 1]))
      b--;
  return str_of(ctx, s.u.str->data + a, b - a);
}
static js_value strm_trim(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return str_trim(ctx, t, 1, 1);
}
static js_value strm_trimstart(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return str_trim(ctx, t, 1, 0);
}
static js_value strm_trimend(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return str_trim(ctx, t, 0, 1);
}
static js_value str_repeat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int count = n > 0 ? js_to_int_sat(ctx, a[0]) : 0, i;
  sbuf sb;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  if (count < 0)
    return js_throw_range(ctx, "invalid count value");
  gc_push_scope();
  KEEP(s);
  for (i = 0; i < count; i++)
    sb_put(&sb, s.u.str->data, s.u.str->len);
  gc_pop_scope();
  {
    js_value r = str_of(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
}
static js_value str_pad(js_ctx *ctx, js_value t, js_value *a, int n, int start)
{
  js_value s = this_str(ctx, t);
  int target = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  js_value pad = n > 1 && a[1].type != JV_UNDEF ? js_to_string(ctx, a[1]) : js_mk_cstring(ctx, " ");
  sbuf sb;
  int need;
  if (target <= s.u.str->len || pad.u.str->len == 0)
    return s;
  need = target - s.u.str->len;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  KEEP(s);
  KEEP(pad);
  if (!start)
    sb_put(&sb, s.u.str->data, s.u.str->len);
  {
    int i = 0;
    while (i < need) {
      sb_putc(&sb, pad.u.str->data[i % pad.u.str->len]);
      i++;
    }
  }
  if (start)
    sb_put(&sb, s.u.str->data, s.u.str->len);
  gc_pop_scope();
  {
    js_value r = str_of(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
}
static js_value str_padstart(js_ctx *ctx, js_value t, js_value *a, int n) { return str_pad(ctx, t, a, n, 1); }
static js_value str_padend(js_ctx *ctx, js_value t, js_value *a, int n) { return str_pad(ctx, t, a, n, 0); }
static js_value str_concat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  int i;
  gc_push_scope();
  KEEP(s);
  for (i = 0; i < n; i++) {
    js_value o = js_to_string(ctx, a[i]);
    s.u.str = js_str_concat(ctx, s.u.str, o.u.str);
    gc_keep(s.u.str);
  }
  gc_pop_scope();
  return s;
}
/* ------------------------------------------------------- RegExp ----------- *
 * A compact backtracking regex engine: the pattern is parsed into a small AST,
 * compiled to a flat instruction list, then matched by a recursive backtracking
 * VM (CHAR/ANY/CLASS/anchors + SPLIT/JMP/SAVE/MATCH, à la Russ Cox's
 * "regexp2"). Supported: literals, ., char classes [..]/[^..] with ranges and
 * \d\w\s, anchors ^ $ \b \B, groups (capturing + (?:..)), alternation |, the
 * quantifiers * + ? and {n,m} (greedy + lazy with a trailing ?), and the
 * i/g/m/s flags. NOT supported: lookahead/behind (parsed as a plain group),
 * backreferences (a \1 is a literal digit), and Unicode property escapes. A
 * step + recursion-depth guard bounds catastrophic backtracking (a runaway
 * match just reports "no match").
 *
 * RegExp objects carry only their source + flag strings; test/exec recompile
 * per call (cheap for page scripts, and leak-free — the engine has no host
 * finalizer). */

enum {
  I_CHAR,
  I_ANY,
  I_CLASS,
  I_BOL,
  I_EOL,
  I_WORDB,
  I_NWORDB,
  I_SPLIT,
  I_JMP,
  I_SAVE,
  I_MATCH,
  I_AHEAD,
  I_AEND,
  I_BEHIND,
  I_BEND,
  I_BACKREF
};
typedef struct {
  unsigned char op, neg;
  char ch;
  int x, y;
  int roff, nrng;        /* I_CLASS: code point ranges >= 128 in reprog.pool, as lo/hi pairs */
  unsigned char set[16]; /* I_CLASS: code points below 128 */
} reinst;
typedef struct {
  reinst *ins;
  int n, cap;
  int ngroups;
  char *names[32];
  unsigned *pool; /* class ranges for every I_CLASS */
  int npool, cappool;
  int refs; /* the compile cache holds one reference */
} reprog;

enum {
  N_EMPTY,
  N_CHAR,
  N_ANY,
  N_CLASS,
  N_BOL,
  N_EOL,
  N_WORDB,
  N_NWORDB,
  N_CAT,
  N_ALT,
  N_STAR,
  N_PLUS,
  N_QUEST,
  N_GROUP,
  N_LOOKAHEAD,
  N_LOOKBEHIND,
  N_BACKREF
};
typedef struct rnode {
  int type;
  char ch;
  unsigned char neg, greedy;
  int gidx;
  unsigned char set[16]; /* N_CLASS: code points below 128 */
  unsigned *rng;         /* N_CLASS: ranges >= 128 as lo/hi pairs */
  int nrng, caprng;
  const char *name;
  int namelen; /* N_BACKREF: pending \k<name> to resolve */
  struct rnode *a, *b;
} rnode;

#define RE_GROUPS_MAX 31

typedef struct {
  const char *p, *end;
  int ngroups, err;
  rnode **all;
  int nall, callc; /* every node, freed together (some are shared) */
  const char *gname[RE_GROUPS_MAX + 1];
  int gnamelen[RE_GROUPS_MAX + 1];
} recomp;
#define RE_DEPTH_MAX 6000
#define RE_STEPS_MAX 2000000

static void re_setbit(unsigned char *set, unsigned c) { set[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static int re_getbit(const unsigned char *set, unsigned c) { return (set[c >> 3] >> (c & 7)) & 1; }
static int re_flag(const char *flags, char f)
{
  for (; flags && *flags; flags++)
    if (*flags == f)
      return 1;
  return 0;
}
static char re_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char re_swapcase(char c)
{
  if (c >= 'a' && c <= 'z')
    return (char)(c - 32);
  if (c >= 'A' && c <= 'Z')
    return (char)(c + 32);
  return c;
}
static int re_word(int c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* Lowest native SP the regex compiler/matcher may recurse to; set per call from the context. */
static const char *re_floor;
static int re_deep(void)
{
  char probe;
  return re_floor && &probe < re_floor;
}

/* Add the code points lo..hi to a class: below 128 as bits, the rest as a range */
static void re_add_range(recomp *c, rnode *nd, unsigned lo, unsigned hi)
{
  unsigned i;
  for (i = lo; i <= hi && i < 128; i++)
    re_setbit(nd->set, i);
  if (hi < 128)
    return;
  if (lo < 128)
    lo = 128;
  if (nd->nrng + 2 > nd->caprng) {
    int nc = nd->caprng ? nd->caprng * 2 : 8;
    unsigned *nr = (unsigned *)realloc(nd->rng, (unsigned)nc * sizeof *nr);
    if (!nr) {
      c->err = 1;
      return;
    }
    nd->rng = nr;
    nd->caprng = nc;
  }
  nd->rng[nd->nrng++] = lo;
  nd->rng[nd->nrng++] = hi;
}

/* Unicode White_Space beyond ASCII, as lo/hi pairs (JS \s) */
static const unsigned re_space_rng[] = {0xA0,   0xA0,   0x1680, 0x1680, 0x2000, 0x200A, 0x2028, 0x2029,
                                        0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF};

/* Add the \d \w \s escapes (and complements) to a class; 1 if `e` was one, else 0. */
static int re_class_escape(recomp *c, rnode *nd, char e)
{
  unsigned char tmp[16] = {0};
  unsigned i, next = 128;
  int neg = e >= 'A' && e <= 'Z';
  char l = re_lc(e);
  if (l == 'd') {
    for (i = '0'; i <= '9'; i++)
      re_setbit(tmp, i);
  } else if (l == 'w') {
    for (i = '0'; i <= '9'; i++)
      re_setbit(tmp, i);
    for (i = 'A'; i <= 'Z'; i++)
      re_setbit(tmp, i);
    for (i = 'a'; i <= 'z'; i++)
      re_setbit(tmp, i);
    re_setbit(tmp, '_');
  } else if (l == 's') {
    re_setbit(tmp, ' ');
    for (i = 9; i <= 13; i++)
      re_setbit(tmp, i);
  } else
    return 0;
  for (i = 0; i < 128; i++)
    if (re_getbit(tmp, i) != neg)
      re_setbit(nd->set, i);
  if (l != 's') {
    if (neg)
      re_add_range(c, nd, 128, 0x10FFFF);
    return 1;
  }
  for (i = 0; i < sizeof re_space_rng / sizeof re_space_rng[0]; i += 2) {
    if (!neg) {
      re_add_range(c, nd, re_space_rng[i], re_space_rng[i + 1]);
      continue;
    }
    if (re_space_rng[i] > next)
      re_add_range(c, nd, next, re_space_rng[i] - 1);
    next = re_space_rng[i + 1] + 1;
  }
  if (neg)
    re_add_range(c, nd, next, 0x10FFFF);
  return 1;
}

static rnode *rn_new(recomp *c, int type)
{
  rnode *n;
  if (c->nall == c->callc) {
    int nc = c->callc ? c->callc * 2 : 32;
    rnode **na = (rnode **)realloc(c->all, (unsigned)nc * sizeof *na);
    if (!na) {
      c->err = 1;
      return 0;
    }
    c->all = na;
    c->callc = nc;
  }
  n = (rnode *)calloc(1, sizeof *n);
  if (!n) {
    c->err = 1;
    return 0;
  }
  n->type = type;
  c->all[c->nall++] = n;
  return n;
}
static rnode *rn_cat(recomp *c, rnode *a, rnode *b)
{
  rnode *n;
  if (!a)
    return b;
  if (!b)
    return a;
  n = rn_new(c, N_CAT);
  if (!n)
    return a;
  n->a = a;
  n->b = b;
  return n;
}

static rnode *re_parse_alt(recomp *c);

/* one hex digit, or -1 */
static int re_hex(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}
/* up to ndig hex digits at c->p (stops at the first non-digit) */
static unsigned re_read_hex(recomp *c, int ndig)
{
  unsigned v = 0;
  int i, h;
  for (i = 0; i < ndig && c->p < c->end; i++) {
    h = re_hex(*c->p);
    if (h < 0)
      break;
    v = v * 16 + (unsigned)h;
    c->p++;
  }
  return v;
}
/* One pattern code point at c->p: raw UTF-8 or an escape (after the '\' when `esc`); advances. */
static unsigned re_cp(recomp *c, int esc)
{
  unsigned cp;
  char e;
  if (!esc) {
    int n = utf8_decode(c->p, c->end, &cp);
    if ((unsigned char)*c->p >= 0x80 && cp == 0xFFFD)
      cp = (unsigned char)*c->p;
    c->p += n;
    return cp;
  }
  if (c->p >= c->end) {
    c->err = 1;
    return '\\';
  }
  e = *c->p++;
  switch (e) {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case 'r':
    return '\r';
  case 'f':
    return '\f';
  case 'v':
    return '\v';
  case '0':
    return 0;
  case 'x':
    return re_read_hex(c, 2);
  case 'c':
    if (c->p < c->end && re_lc(*c->p) >= 'a' && re_lc(*c->p) <= 'z') {
      char ch = *c->p++;
      return (unsigned)(re_lc(ch) - 'a' + 1);
    }
    return 'c';
  case 'u':
    if (c->p < c->end && *c->p == '{') {
      c->p++;
      cp = re_read_hex(c, 8);
      if (c->p < c->end && *c->p == '}')
        c->p++;
      else
        c->err = 1;
      return cp > 0x10FFFF ? 0xFFFD : cp;
    }
    cp = re_read_hex(c, 4);
    if (cp >= 0xD800 && cp < 0xDC00 && c->p + 1 < c->end && c->p[0] == '\\' && c->p[1] == 'u') {
      const char *save = c->p;
      unsigned lo;
      c->p += 2;
      lo = re_read_hex(c, 4);
      if (lo >= 0xDC00 && lo < 0xE000)
        return 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      c->p = save;
    }
    return cp;
  default:
    c->p--; /* an escaped literal, possibly multi-byte */
    return re_cp(c, 0);
  }
}
/* A literal code point as a node: one byte, or the byte sequence of its UTF-8 form */
static rnode *re_char_node(recomp *c, unsigned cp)
{
  char enc[4];
  int k = utf8_encode(cp, enc), i;
  rnode *head = 0;
  for (i = 0; i < k; i++) {
    rnode *nn = rn_new(c, N_CHAR);
    if (!nn)
      return head;
    nn->ch = enc[i];
    head = rn_cat(c, head, nn);
  }
  return head;
}

static rnode *re_parse_class(recomp *c)
{
  rnode *nd = rn_new(c, N_CLASS);
  if (!nd)
    return 0;
  c->p++; /* '[' */
  if (c->p < c->end && *c->p == '^') {
    nd->neg = 1;
    c->p++;
  }
  while (c->p < c->end && *c->p != ']') {
    unsigned lo, hi;
    if (*c->p == '\\') {
      char peek = (c->p + 1 < c->end) ? c->p[1] : 0;
      if (re_lc(peek) == 'd' || re_lc(peek) == 'w' || re_lc(peek) == 's') {
        c->p += 2;
        re_class_escape(c, nd, peek);
        continue;
      }
      c->p++;
      if (c->p < c->end && *c->p == 'b') {
        c->p++;
        lo = 8; /* \b inside a class is a backspace */
      } else
        lo = re_cp(c, 1);
    } else
      lo = re_cp(c, 0);
    if (c->p + 1 < c->end && *c->p == '-' && c->p[1] != ']') {
      c->p++;
      if (*c->p == '\\') {
        c->p++;
        hi = re_cp(c, 1);
      } else
        hi = re_cp(c, 0);
      if (hi < lo) {
        c->err = 1;
        break;
      }
      re_add_range(c, nd, lo, hi);
    } else
      re_add_range(c, nd, lo, lo);
  }
  if (c->p < c->end && *c->p == ']')
    c->p++;
  else
    c->err = 1;
  return nd;
}

static rnode *re_parse_atom(recomp *c)
{
  char ch;
  if (c->p >= c->end)
    return 0;
  ch = *c->p;
  if (ch == '(') {
    rnode *inner, *g;
    int cap = 1, gi = 0, look = 0, lneg = 0;
    const char *nm = 0;
    int nmlen = 0;
    c->p++;
    if (c->p < c->end && *c->p == '?') {
      c->p++;
      cap = 0;
      if (c->p < c->end && *c->p == ':')
        c->p++; /* (?:..) non-capturing */
      else if (c->p < c->end && *c->p == '=') {
        c->p++;
        look = 1;
      } /* (?=..) lookahead   */
      else if (c->p < c->end && *c->p == '!') {
        c->p++;
        look = 1;
        lneg = 1;
      } /* (?!..) neg      */
      else if (c->p + 1 < c->end && *c->p == '<' && (c->p[1] == '=' || c->p[1] == '!')) {
        lneg = (c->p[1] == '!');
        c->p += 2;
        look = 2;                                 /* (?<=..)/(?<!..) lookbehind */
      } else if (c->p < c->end && *c->p == '<') { /* (?<name>..) named group */
        c->p++;
        nm = c->p;
        while (c->p < c->end && *c->p != '>')
          c->p++;
        nmlen = (int)(c->p - nm);
        if (c->p < c->end && *c->p == '>') {
          c->p++;
          cap = 1;
        } else
          c->err = 1;
      }
      /* else: unknown (?..) form — left as a non-capturing group body */
    }
    if (cap && c->ngroups < RE_GROUPS_MAX) {
      gi = ++c->ngroups;
      if (nm) {
        c->gname[gi] = nm;
        c->gnamelen[gi] = nmlen;
      }
    } else
      cap = 0;
    inner = re_parse_alt(c);
    if (c->p < c->end && *c->p == ')')
      c->p++;
    else
      c->err = 1;
    if (look) {
      g = rn_new(c, look == 1 ? N_LOOKAHEAD : N_LOOKBEHIND);
      if (!g)
        return inner;
      g->a = inner;
      g->neg = (unsigned char)lneg;
      return g;
    }
    g = rn_new(c, N_GROUP);
    if (!g)
      return inner;
    g->a = inner;
    g->gidx = cap ? gi : 0;
    return g;
  }
  if (ch == '[')
    return re_parse_class(c);
  if (ch == '.') {
    c->p++;
    return rn_new(c, N_ANY);
  }
  if (ch == '^') {
    c->p++;
    return rn_new(c, N_BOL);
  }
  if (ch == '$') {
    c->p++;
    return rn_new(c, N_EOL);
  }
  if (ch == '\\') {
    char peek = (c->p + 1 < c->end) ? c->p[1] : 0;
    if (re_lc(peek) == 'd' || re_lc(peek) == 'w' || re_lc(peek) == 's') {
      rnode *nd = rn_new(c, N_CLASS);
      c->p++;
      if (nd)
        re_class_escape(c, nd, *c->p);
      c->p++;
      return nd;
    }
    if (peek == 'b') {
      c->p += 2;
      return rn_new(c, N_WORDB);
    }
    if (peek == 'B') {
      c->p += 2;
      return rn_new(c, N_NWORDB);
    }
    if (peek >= '1' && peek <= '9') { /* \1..\N numeric backreference */
      rnode *bn;
      int num = 0;
      c->p++; /* skip '\' */
      while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        num = num * 10 + (*c->p - '0');
        c->p++;
      }
      bn = rn_new(c, N_BACKREF);
      if (bn)
        bn->gidx = num;
      return bn;
    }
    if (peek == 'k' && c->p + 2 < c->end && c->p[2] == '<') { /* \k<name> named backreference */
      rnode *bn;
      const char *bnm;
      c->p += 3; /* skip '\k<' */
      bnm = c->p;
      while (c->p < c->end && *c->p != '>')
        c->p++;
      bn = rn_new(c, N_BACKREF);
      if (bn) {
        bn->name = bnm;
        bn->namelen = (int)(c->p - bnm);
        bn->gidx = -1;
      }
      if (c->p < c->end && *c->p == '>')
        c->p++;
      else
        c->err = 1;
      return bn;
    }
    c->p++;
    return re_char_node(c, re_cp(c, 1));
  }
  return re_char_node(c, re_cp(c, 0));
}

static rnode *re_parse_quant(recomp *c)
{
  rnode *atom = re_parse_atom(c);
  char q;
  if (!atom || c->p >= c->end)
    return atom;
  q = *c->p;
  if (q == '*' || q == '+' || q == '?') {
    rnode *r;
    int greedy = 1;
    c->p++;
    if (c->p < c->end && *c->p == '?') {
      greedy = 0;
      c->p++;
    }
    r = rn_new(c, q == '*' ? N_STAR : q == '+' ? N_PLUS : N_QUEST);
    if (!r)
      return atom;
    r->a = atom;
    r->greedy = (unsigned char)greedy;
    return r;
  }
  if (q == '{') {
    const char *save = c->p;
    int mn = 0, mx = -1, have = 0, greedy = 1, i;
    rnode *result = 0;
    c->p++;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
      mn = mn * 10 + (*c->p - '0');
      have = 1;
      c->p++;
    }
    if (c->p < c->end && *c->p == ',') {
      c->p++;
      if (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        mx = 0;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
          mx = mx * 10 + (*c->p - '0');
          c->p++;
        }
      }
    } else
      mx = mn;
    if (!have || c->p >= c->end || *c->p != '}') {
      c->p = save;
      return atom;
    } /* literal '{' */
    c->p++;
    if (c->p < c->end && *c->p == '?') {
      greedy = 0;
      c->p++;
    }
    for (i = 0; i < mn; i++)
      result = rn_cat(c, result, atom);
    if (mx < 0) {
      rnode *st = rn_new(c, N_STAR);
      if (st) {
        st->a = atom;
        st->greedy = (unsigned char)greedy;
        result = rn_cat(c, result, st);
      }
    } else
      for (i = mn; i < mx; i++) {
        rnode *qn = rn_new(c, N_QUEST);
        if (qn) {
          qn->a = atom;
          qn->greedy = (unsigned char)greedy;
          result = rn_cat(c, result, qn);
        }
      }
    if (!result)
      result = rn_new(c, N_EMPTY);
    return result;
  }
  return atom;
}

static rnode *re_parse_cat(recomp *c)
{
  rnode *head = 0;
  while (!c->err && c->p < c->end && *c->p != '|' && *c->p != ')')
    head = rn_cat(c, head, re_parse_quant(c));
  return head ? head : rn_new(c, N_EMPTY);
}
static rnode *re_parse_alt(recomp *c)
{
  rnode *a;
  if (re_deep()) {
    c->err = 1;
    return 0;
  }
  a = re_parse_cat(c);
  while (!c->err && c->p < c->end && *c->p == '|') {
    rnode *alt, *b;
    c->p++;
    b = re_parse_cat(c);
    alt = rn_new(c, N_ALT);
    if (!alt)
      break;
    alt->a = a;
    alt->b = b;
    a = alt;
  }
  return a;
}

static int re_emit_at(reprog *p, reinst in)
{
  if (p->n == p->cap) {
    int nc = p->cap ? p->cap * 2 : 64;
    reinst *ni = (reinst *)realloc(p->ins, (unsigned)nc * sizeof *ni);
    if (!ni)
      return -1;
    p->ins = ni;
    p->cap = nc;
  }
  p->ins[p->n] = in;
  return p->n++;
}
static int re_emit(reprog *p, rnode *nd)
{
  reinst in;
  int i, j;
  memset(&in, 0, sizeof in);
  if (!nd)
    return 0;
  if (re_deep())
    return -1;
  switch (nd->type) {
  case N_EMPTY:
    return 0;
  case N_CHAR:
    in.op = I_CHAR;
    in.ch = nd->ch;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_ANY:
    in.op = I_ANY;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_CLASS:
    in.op = I_CLASS;
    in.neg = nd->neg;
    memcpy(in.set, nd->set, sizeof in.set);
    if (nd->nrng) {
      if (p->npool + nd->nrng > p->cappool) {
        int nc = p->cappool ? p->cappool * 2 : 32;
        unsigned *np;
        while (nc < p->npool + nd->nrng)
          nc *= 2;
        np = (unsigned *)realloc(p->pool, (unsigned)nc * sizeof *np);
        if (!np)
          return -1;
        p->pool = np;
        p->cappool = nc;
      }
      memcpy(p->pool + p->npool, nd->rng, (unsigned)nd->nrng * sizeof *nd->rng);
      in.roff = p->npool;
      in.nrng = nd->nrng;
      p->npool += nd->nrng;
    }
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_BOL:
    in.op = I_BOL;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_EOL:
    in.op = I_EOL;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_WORDB:
    in.op = I_WORDB;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_NWORDB:
    in.op = I_NWORDB;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_CAT:
    if (re_emit(p, nd->a) < 0)
      return -1;
    return re_emit(p, nd->b);
  case N_ALT:
    in.op = I_SPLIT;
    i = re_emit_at(p, in);
    if (i < 0)
      return -1;
    p->ins[i].x = p->n;
    if (re_emit(p, nd->a) < 0)
      return -1;
    in.op = I_JMP;
    j = re_emit_at(p, in);
    if (j < 0)
      return -1;
    p->ins[i].y = p->n;
    if (re_emit(p, nd->b) < 0)
      return -1;
    p->ins[j].x = p->n;
    return 0;
  case N_STAR: {
    int l1 = p->n;
    in.op = I_SPLIT;
    i = re_emit_at(p, in);
    if (i < 0)
      return -1;
    p->ins[i].x = p->n;
    if (re_emit(p, nd->a) < 0)
      return -1;
    in.op = I_JMP;
    in.x = l1;
    if (re_emit_at(p, in) < 0)
      return -1;
    p->ins[i].y = p->n;
    if (!nd->greedy) {
      int tx = p->ins[i].x;
      p->ins[i].x = p->ins[i].y;
      p->ins[i].y = tx;
    }
    return 0;
  }
  case N_PLUS: {
    int l1 = p->n;
    if (re_emit(p, nd->a) < 0)
      return -1;
    in.op = I_SPLIT;
    in.x = l1;
    in.y = p->n + 1;
    i = re_emit_at(p, in);
    if (i < 0)
      return -1;
    if (!nd->greedy) {
      int tx = p->ins[i].x;
      p->ins[i].x = p->ins[i].y;
      p->ins[i].y = tx;
    }
    return 0;
  }
  case N_QUEST:
    in.op = I_SPLIT;
    i = re_emit_at(p, in);
    if (i < 0)
      return -1;
    p->ins[i].x = p->n;
    if (re_emit(p, nd->a) < 0)
      return -1;
    p->ins[i].y = p->n;
    if (!nd->greedy) {
      int tx = p->ins[i].x;
      p->ins[i].x = p->ins[i].y;
      p->ins[i].y = tx;
    }
    return 0;
  case N_GROUP:
    if (nd->gidx > 0) {
      in.op = I_SAVE;
      in.x = 2 * nd->gidx;
      if (re_emit_at(p, in) < 0)
        return -1;
    }
    if (re_emit(p, nd->a) < 0)
      return -1;
    if (nd->gidx > 0) {
      memset(&in, 0, sizeof in);
      in.op = I_SAVE;
      in.x = 2 * nd->gidx + 1;
      if (re_emit_at(p, in) < 0)
        return -1;
    }
    return 0;
  case N_BACKREF:
    in.op = I_BACKREF;
    in.x = nd->gidx;
    return re_emit_at(p, in) < 0 ? -1 : 0;
  case N_LOOKAHEAD:
  case N_LOOKBEHIND: {
    /* [AHEAD|BEHIND neg,cont] body... [AEND|BEND]; the assertion body is a
     * sub-match run at the current position — zero-width for the outer VM. */
    reinst e;
    in.op = (nd->type == N_LOOKAHEAD) ? I_AHEAD : I_BEHIND;
    in.neg = nd->neg;
    i = re_emit_at(p, in);
    if (i < 0)
      return -1;
    if (re_emit(p, nd->a) < 0)
      return -1;
    memset(&e, 0, sizeof e);
    e.op = (nd->type == N_LOOKAHEAD) ? I_AEND : I_BEND;
    if (re_emit_at(p, e) < 0)
      return -1;
    p->ins[i].x = p->n; /* continuation just past the body */
    return 0;
  }
  }
  return 0;
}

static void re_free(reprog *p)
{
  if (p && --p->refs <= 0) {
    int g;
    for (g = 1; g <= RE_GROUPS_MAX; g++)
      if (p->names[g])
        free(p->names[g]);
    free(p->ins);
    free(p->pool);
    free(p);
  }
}

static reprog *re_compile(const char *src, int srclen)
{
  recomp c;
  rnode *tree;
  reprog *p;
  reinst in;
  int i, ok, g;
  c.p = src;
  c.end = src + srclen;
  c.ngroups = 0;
  c.err = 0;
  c.all = 0;
  c.nall = 0;
  c.callc = 0;
  for (g = 0; g <= RE_GROUPS_MAX; g++) {
    c.gname[g] = 0;
    c.gnamelen[g] = 0;
  }
  tree = re_parse_alt(&c);
  if (c.p != c.end)
    c.err = 1; /* trailing ')' or junk */
  /* resolve \k<name> backrefs against the collected named groups */
  for (i = 0; i < c.nall; i++) {
    rnode *nd = c.all[i];
    if (nd->type == N_BACKREF && nd->gidx < 0 && nd->name) {
      nd->gidx = 0;
      for (g = 1; g <= c.ngroups; g++)
        if (c.gname[g] && c.gnamelen[g] == nd->namelen && !memcmp(c.gname[g], nd->name, (unsigned)nd->namelen)) {
          nd->gidx = g;
          break;
        }
    }
  }
  ok = !c.err;
  p = (reprog *)calloc(1, sizeof *p);
  if (p)
    p->refs = 1;
  if (p && ok) {
    p->ngroups = c.ngroups;
    memset(&in, 0, sizeof in);
    in.op = I_SAVE;
    in.x = 0;
    ok = re_emit_at(p, in) >= 0;
    if (ok)
      ok = re_emit(p, tree) >= 0;
    if (ok) {
      memset(&in, 0, sizeof in);
      in.op = I_SAVE;
      in.x = 1;
      ok = re_emit_at(p, in) >= 0;
    }
    if (ok) {
      memset(&in, 0, sizeof in);
      in.op = I_MATCH;
      ok = re_emit_at(p, in) >= 0;
    }
    for (g = 1; ok && g <= c.ngroups; g++) {
      if (c.gname[g] && c.gnamelen[g] > 0) {
        char *nm = (char *)malloc((unsigned)c.gnamelen[g] + 1);
        if (nm) {
          memcpy(nm, c.gname[g], (unsigned)c.gnamelen[g]);
          nm[c.gnamelen[g]] = 0;
          p->names[g] = nm;
        }
      }
    }
  }
  for (i = 0; i < c.nall; i++) {
    free(c.all[i]->rng);
    free(c.all[i]);
  }
  if (c.all)
    free(c.all);
  if (!ok) {
    re_free(p);
    return 0;
  }
  return p;
}

typedef struct {
  const char *s;
  int slen, icase, multiline, dotall, depth, steps, look_end;
} restate;

static int re_class_hit(const reprog *p, const reinst *in, unsigned cp, int icase)
{
  int hit = 0, i;
  if (cp < 128) {
    hit = re_getbit(in->set, cp);
    if (!hit && icase) {
      char alt = re_swapcase((char)cp);
      if (alt != (char)cp)
        hit = re_getbit(in->set, (unsigned char)alt);
    }
  } else
    for (i = 0; i < in->nrng && !hit; i += 2)
      hit = cp >= p->pool[in->roff + i] && cp <= p->pool[in->roff + i + 1];
  return in->neg ? !hit : hit;
}
/* the code point at s[sp] and its byte length (a stray byte decodes as itself) */
static int re_decode(const restate *st, int sp, unsigned *cp)
{
  int n;
  if ((unsigned char)st->s[sp] < 0x80) {
    *cp = (unsigned char)st->s[sp];
    return 1;
  }
  n = utf8_decode(st->s + sp, st->s + st->slen, cp);
  if (*cp == 0xFFFD && n == 1)
    *cp = (unsigned char)st->s[sp];
  return n;
}
static int re_run(reprog *p, int pc, int sp, int *caps, restate *st)
{
  if (++st->depth > RE_DEPTH_MAX || re_deep()) {
    st->depth--;
    return -2;
  }
  for (;;) {
    reinst *in;
    if (++st->steps > RE_STEPS_MAX) {
      st->depth--;
      return -2;
    }
    in = &p->ins[pc];
    switch (in->op) {
    case I_CHAR:
      if (sp < st->slen && (st->s[sp] == in->ch || (st->icase && re_lc(st->s[sp]) == re_lc(in->ch)))) {
        pc++;
        sp++;
        break;
      }
      st->depth--;
      return -1;
    case I_ANY:
      if (sp < st->slen) {
        unsigned cp;
        int n = re_decode(st, sp, &cp);
        if (st->dotall || !(cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029)) {
          pc++;
          sp += n;
          break;
        }
      }
      st->depth--;
      return -1;
    case I_CLASS:
      if (sp < st->slen) {
        unsigned cp;
        int n = re_decode(st, sp, &cp);
        if (re_class_hit(p, in, cp, st->icase)) {
          pc++;
          sp += n;
          break;
        }
      }
      st->depth--;
      return -1;
    case I_BOL:
      if (sp == 0 || (st->multiline && st->s[sp - 1] == '\n')) {
        pc++;
        break;
      }
      st->depth--;
      return -1;
    case I_EOL:
      if (sp == st->slen || (st->multiline && st->s[sp] == '\n')) {
        pc++;
        break;
      }
      st->depth--;
      return -1;
    case I_WORDB:
    case I_NWORDB: {
      int a = sp > 0 && re_word((unsigned char)st->s[sp - 1]);
      int b = sp < st->slen && re_word((unsigned char)st->s[sp]);
      int boundary = a != b;
      if ((in->op == I_WORDB) == (boundary != 0)) {
        pc++;
        break;
      }
      st->depth--;
      return -1;
    }
    case I_JMP:
      pc = in->x;
      break;
    case I_SPLIT: {
      int r = re_run(p, in->x, sp, caps, st);
      if (r >= 0 || r == -2) {
        st->depth--;
        return r;
      }
      pc = in->y;
      break; /* x failed (and restored its saves): try y */
    }
    case I_SAVE: {
      int old = caps[in->x], r;
      caps[in->x] = sp;
      r = re_run(p, pc + 1, sp, caps, st);
      if (r < 0)
        caps[in->x] = old;
      st->depth--;
      return r;
    }
    case I_MATCH:
      st->depth--;
      return sp;
    case I_AHEAD: {
      int r = re_run(p, pc + 1, sp, caps, st); /* body ends in I_AEND (returns on match) */
      if (r == -2) {
        st->depth--;
        return -2;
      }
      if (in->neg) {
        if (r >= 0) {
          st->depth--;
          return -1;
        }
      } /* (?!): fail if body matched */
      else if (r < 0) {
        st->depth--;
        return -1;
      } /* (?=): fail if body didn't  */
      pc = in->x;
      break; /* zero-width: continue past  */
    }
    case I_AEND:
      st->depth--;
      return sp; /* lookahead body matched     */
    case I_BEHIND: {
      int saved = st->look_end, j, matched = 0, r = -1;
      for (j = sp; j >= 0; j--) { /* body must end exactly at sp */
        st->look_end = sp;
        r = re_run(p, pc + 1, j, caps, st);
        if (r == -2) {
          st->look_end = saved;
          st->depth--;
          return -2;
        }
        if (r >= 0) {
          matched = 1;
          break;
        }
      }
      st->look_end = saved;
      if (in->neg) {
        if (matched) {
          st->depth--;
          return -1;
        }
      } else if (!matched) {
        st->depth--;
        return -1;
      }
      pc = in->x;
      break;
    }
    case I_BEND:
      if (sp == st->look_end) {
        st->depth--;
        return sp;
      }
      st->depth--;
      return -1;
    case I_BACKREF: {
      int g = in->x, a, b, len, k;
      if (g < 1 || g > p->ngroups) {
        pc++;
        break;
      } /* no such group: empty match  */
      a = caps[2 * g];
      b = caps[2 * g + 1];
      if (a < 0 || b < a) {
        pc++;
        break;
      } /* unset group: empty match    */
      len = b - a;
      if (sp + len > st->slen) {
        st->depth--;
        return -1;
      }
      for (k = 0; k < len; k++) {
        char c1 = st->s[a + k], c2 = st->s[sp + k];
        if (c1 != c2 && !(st->icase && re_lc(c1) == re_lc(c2))) {
          st->depth--;
          return -1;
        }
      }
      pc++;
      sp += len;
      break;
    }
    default:
      st->depth--;
      return -1;
    }
  }
}

/* Search `s` for `prog` starting at `from`. Fills caps[0..2*(ng+1)) with match
 * + group offsets (-1 = unset). Returns the start index of the match, or -1. */
static int re_search(reprog *p, const char *s, int slen, int from, int icase, int ml, int dotall, int *caps, int ncap)
{
  int start, i;
  restate st;
  st.s = s;
  st.slen = slen;
  st.icase = icase;
  st.multiline = ml;
  st.dotall = dotall;
  st.look_end = -1;
  for (start = from; start <= slen; start++) {
    if (start > 0 && start < slen && ((unsigned char)s[start] & 0xC0) == 0x80)
      continue; /* inside a multi-byte character */
    for (i = 0; i < ncap; i++)
      caps[i] = -1;
    st.depth = 0;
    st.steps = 0;
    if (re_run(p, 0, start, caps, &st) >= 0)
      return caps[0] >= 0 ? caps[0] : start;
  }
  return -1;
}

static int re_is_regexp(js_ctx *ctx, js_value v) { return v.type == JV_OBJ && v.u.obj->proto == ctx->regexp_proto; }

/* Compile the source+flags of a RegExp object; *caps_n = 2*(groups+1). */
#define RE_CACHE_N 16
static struct {
  char *src;
  int len;
  reprog *prog;
} re_cache[RE_CACHE_N];
static int re_cache_next;

/* Compile the source+flags carried by a RegExp object; *caps_n = 2*(groups+1).
 */
static reprog *re_compile_obj(js_ctx *ctx, js_value rx, int *caps_n)
{
  js_value src = js_get(ctx, rx, "source");
  reprog *p = NULL;
  const char *s = src.type == JV_STR ? src.u.str->data : "";
  int len = src.type == JV_STR ? src.u.str->len : 0, i;
  re_floor = ctx->stack_limit;
  for (i = 0; i < RE_CACHE_N && !p; i++)
    if (re_cache[i].prog && re_cache[i].len == len && memcmp_(re_cache[i].src, s, len) == 0) {
      p = re_cache[i].prog;
      p->refs++;
    }
  if (!p && (p = re_compile(s, len))) {
    char *key = (char *)malloc((unsigned)len + 1);
    if (key) {
      i = re_cache_next++ % RE_CACHE_N;
      re_free(re_cache[i].prog);
      free(re_cache[i].src);
      memcpy(key, s, (unsigned)len);
      re_cache[i].src = key;
      re_cache[i].len = len;
      re_cache[i].prog = p;
      p->refs++;
    }
  }
  if (p && caps_n)
    *caps_n = 2 * (p->ngroups + 1);
  return p;
}
static int re_obj_flag(js_ctx *ctx, js_value rx, const char *name) { return js_to_boolean(js_get(ctx, rx, name)); }

/* Build the match result array [full, g1, g2, …] with .index, .input and
 * .groups. */
static js_value re_make_result(js_ctx *ctx, js_value input, int *caps, int ng, char **names)
{
  js_object *arr;
  int k, has_named = 0;
  const char *s = input.type == JV_STR ? input.u.str->data : "";
  gc_push_scope();
  KEEP(input);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (k = 0; k <= ng; k++) {
    int a = caps[2 * k], b = caps[2 * k + 1];
    if (a >= 0 && b >= a)
      js_array_push(ctx, arr, str_of(ctx, s + a, b - a));
    else
      js_array_push(ctx, arr, js_undef());
  }
  js_def(arr, "index", js_number(caps[0]), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  js_def(arr, "input", input, P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  for (k = 1; k <= ng; k++)
    if (names && names[k]) {
      has_named = 1;
      break;
    }
  if (has_named) {
    js_object *groups = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    for (k = 1; k <= ng; k++) {
      if (names && names[k]) {
        int a = caps[2 * k], b = caps[2 * k + 1];
        js_def(groups, names[k], (a >= 0 && b >= a) ? str_of(ctx, s + a, b - a) : js_undef(),
               P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
      }
    }
    js_def(arr, "groups", js_obj_value(groups), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  } else {
    js_def(arr, "groups", js_undef(), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

static js_value re_exec(js_ctx *ctx, js_value rx, js_value t, js_value *a, int n)
{
  js_value input = js_to_string(ctx, ARG(0));
  reprog *p;
  int caps_n = 2, *caps, global, start = 0, found;
  (void)n;
  p = re_compile_obj(ctx, rx, &caps_n);
  if (!p)
    return js_null();
  global = re_obj_flag(ctx, rx, "global") || re_obj_flag(ctx, rx, "sticky");
  if (global)
    start = js_to_int_sat(ctx, js_get(ctx, rx, "lastIndex"));
  if (start < 0 || start > input.u.str->len) {
    if (global)
      js_set(ctx, rx, "lastIndex", js_number(0));
    re_free(p);
    return js_null();
  }
  caps = (int *)malloc((unsigned)caps_n * sizeof *caps);
  if (!caps) {
    re_free(p);
    return js_null();
  }
  found = re_search(p, input.u.str->data, input.u.str->len, start, re_obj_flag(ctx, rx, "ignoreCase"),
                    re_obj_flag(ctx, rx, "multiline"), re_obj_flag(ctx, rx, "dotAll"), caps, caps_n);
  if (found < 0) {
    if (global)
      js_set(ctx, rx, "lastIndex", js_number(0));
    free(caps);
    re_free(p);
    return js_null();
  }
  if (global)
    js_set(ctx, rx, "lastIndex", js_number(caps[1] > caps[0] ? caps[1] : caps[0] + 1));
  {
    js_value r = re_make_result(ctx, input, caps, p->ngroups, p->names);
    free(caps);
    re_free(p);
    return r;
  }
}
static js_value re_test(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value r = re_exec(ctx, t, t, a, n);
  return js_bool(r.type != JV_NULL);
}
static js_value re_proto_exec(js_ctx *ctx, js_value t, js_value *a, int n) { return re_exec(ctx, t, t, a, n); }
static js_value re_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value src = js_get(ctx, t, "source"), fl = js_get(ctx, t, "flags");
  sbuf sb;
  js_value r;
  (void)a;
  (void)n;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  sb_putc(&sb, '/');
  if (src.type == JV_STR)
    sb_put(&sb, src.u.str->data, src.u.str->len);
  else
    sb_put(&sb, "(?:)", 4);
  sb_putc(&sb, '/');
  if (fl.type == JV_STR)
    sb_put(&sb, fl.u.str->data, fl.u.str->len);
  r = str_of(ctx, sb.b ? sb.b : "//", sb.b ? sb.n : 2);
  if (sb.b)
    free(sb.b);
  return r;
}

/* String.prototype.match / .search, and the regex paths of split / replace. */
static js_value re_str_match(js_ctx *ctx, js_value s, js_value rx)
{
  if (!re_obj_flag(ctx, rx, "global")) {
    js_value a0 = s;
    return re_exec(ctx, rx, js_undef(), &a0, 1);
  }
  { /* /g: array of all match strings (no groups), or null */
    reprog *p;
    int caps_n = 2, *caps, pos = 0, any = 0;
    js_object *out;
    const char *str = s.type == JV_STR ? s.u.str->data : "";
    int slen = s.type == JV_STR ? s.u.str->len : 0;
    p = re_compile_obj(ctx, rx, &caps_n);
    if (!p)
      return js_null();
    caps = (int *)malloc((unsigned)caps_n * sizeof *caps);
    if (!caps) {
      re_free(p);
      return js_null();
    }
    gc_push_scope();
    KEEP(s);
    out = (js_object *)gc_keep(js_new_array(ctx, 0));
    while (pos <= slen &&
           re_search(p, str, slen, pos, re_obj_flag(ctx, rx, "ignoreCase"), re_obj_flag(ctx, rx, "multiline"),
                     re_obj_flag(ctx, rx, "dotAll"), caps, caps_n) >= 0) {
      js_array_push(ctx, out, str_of(ctx, str + caps[0], caps[1] - caps[0]));
      any = 1;
      pos = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
    }
    gc_pop_scope();
    free(caps);
    re_free(p);
    js_set(ctx, rx, "lastIndex", js_number(0));
    return any ? js_obj_value(out) : js_null();
  }
}
/* String.prototype.matchAll: an array (iterable) of full match-result objects,
 * each with .index/.input/.groups — spread and for-of consume it directly. */
static js_value re_str_matchall(js_ctx *ctx, js_value s, js_value rx)
{
  reprog *p;
  int caps_n = 2, *caps, pos = 0, ic, ml, da;
  js_object *out;
  const char *str = s.type == JV_STR ? s.u.str->data : "";
  int slen = s.type == JV_STR ? s.u.str->len : 0;
  gc_push_scope();
  KEEP(s);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  p = re_compile_obj(ctx, rx, &caps_n);
  if (!p) {
    gc_pop_scope();
    return js_obj_value(out);
  }
  caps = (int *)malloc((unsigned)caps_n * sizeof *caps);
  if (!caps) {
    re_free(p);
    gc_pop_scope();
    return js_obj_value(out);
  }
  ic = re_obj_flag(ctx, rx, "ignoreCase");
  ml = re_obj_flag(ctx, rx, "multiline");
  da = re_obj_flag(ctx, rx, "dotAll");
  while (pos <= slen && re_search(p, str, slen, pos, ic, ml, da, caps, caps_n) >= 0) {
    js_array_push(ctx, out, re_make_result(ctx, s, caps, p->ngroups, p->names));
    pos = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
  }
  free(caps);
  re_free(p);
  js_set(ctx, rx, "lastIndex", js_number(0));
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value re_str_split(js_ctx *ctx, js_value s, js_value rx, js_value limv)
{
  reprog *p;
  int caps_n = 2, *caps, pos = 0, last = 0;
  js_object *out;
  const char *str = s.type == JV_STR ? s.u.str->data : "";
  int slen = s.type == JV_STR ? s.u.str->len : 0;
  int limit = limv.type != JV_UNDEF ? (int)js_to_uint32(ctx, limv) : -1;
  int ic, ml, da;
  p = re_compile_obj(ctx, rx, &caps_n);
  gc_push_scope();
  KEEP(s);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (!p) {
    js_array_push(ctx, out, s);
    gc_pop_scope();
    return js_obj_value(out);
  }
  caps = (int *)malloc((unsigned)caps_n * sizeof *caps);
  if (!caps) {
    js_array_push(ctx, out, s);
    gc_pop_scope();
    re_free(p);
    return js_obj_value(out);
  }
  ic = re_obj_flag(ctx, rx, "ignoreCase");
  ml = re_obj_flag(ctx, rx, "multiline");
  da = re_obj_flag(ctx, rx, "dotAll");
  while (pos <= slen) {
    int at = re_search(p, str, slen, pos, ic, ml, da, caps, caps_n);
    int mstart = caps[0], mend = caps[1];
    if (at < 0 || mstart >= slen)
      break;
    if (mend == last && mstart == last) {
      pos = mstart + 1;
      continue;
    } /* empty match: advance */
    if (limit >= 0 && (int)out->elen >= limit) {
      gc_pop_scope();
      free(caps);
      re_free(p);
      return js_obj_value(out);
    }
    js_array_push(ctx, out, str_of(ctx, str + last, mstart - last));
    {
      int g;
      for (g = 1; g <= p->ngroups; g++) {
        int ga = caps[2 * g], gb = caps[2 * g + 1];
        if (limit >= 0 && (int)out->elen >= limit)
          break;
        js_array_push(ctx, out, ga >= 0 && gb >= ga ? str_of(ctx, str + ga, gb - ga) : js_undef());
      }
    }
    last = mend;
    pos = mend > mstart ? mend : mstart + 1;
  }
  if (!(limit >= 0 && (int)out->elen >= limit))
    js_array_push(ctx, out, str_of(ctx, str + last, slen - last));
  gc_pop_scope();
  free(caps);
  re_free(p);
  return js_obj_value(out);
}
/* expand a replacement template's $&, $1..$N, $<name>, $`, $' (and $$ -> $) */
static void re_expand_repl(sbuf *sb, const char *tmpl, int tlen, const char *str, int slen, int *caps, int ng,
                           char **names)
{
  int i;
  for (i = 0; i < tlen; i++) {
    if (tmpl[i] == '$' && i + 1 < tlen) {
      char d = tmpl[i + 1];
      if (d == '$') {
        sb_putc(sb, '$');
        i++;
        continue;
      }
      if (d == '&') {
        sb_put(sb, str + caps[0], caps[1] - caps[0]);
        i++;
        continue;
      }
      if (d == '`') {
        sb_put(sb, str, caps[0]);
        i++;
        continue;
      }
      if (d == '\'') {
        sb_put(sb, str + caps[1], slen - caps[1]);
        i++;
        continue;
      }
      if (d == '<' && names) { /* $<name> named-group reference */
        int j = i + 2, g;
        while (j < tlen && tmpl[j] != '>')
          j++;
        if (j < tlen) {
          int nl = j - (i + 2);
          for (g = 1; g <= ng; g++)
            if (names[g] && (int)strlen(names[g]) == nl && !memcmp(names[g], tmpl + i + 2, (unsigned)nl)) {
              int a = caps[2 * g], b = caps[2 * g + 1];
              if (a >= 0 && b >= a)
                sb_put(sb, str + a, b - a);
              break;
            }
          i = j;
          continue; /* unknown name expands to empty */
        }
      }
      if (d >= '0' && d <= '9') {
        int gi = d - '0', adv = 1;
        if (i + 2 < tlen && tmpl[i + 2] >= '0' && tmpl[i + 2] <= '9' && (gi * 10 + tmpl[i + 2] - '0') <= ng) {
          gi = gi * 10 + tmpl[i + 2] - '0';
          adv = 2;
        }
        if (gi >= 1 && gi <= ng) {
          int a = caps[2 * gi], b = caps[2 * gi + 1];
          if (a >= 0 && b >= a)
            sb_put(sb, str + a, b - a);
          i += adv;
          continue;
        }
      }
    }
    sb_putc(sb, tmpl[i]);
  }
}
static js_value re_str_replace(js_ctx *ctx, js_value s, js_value rx, js_value repl, int all_hint)
{
  reprog *p;
  int caps_n = 2, *caps, pos = 0, last = 0;
  int global = re_obj_flag(ctx, rx, "global") || all_hint;
  int isfn = js_is_callable(repl), ic, ml, da;
  sbuf sb;
  js_value out;
  const char *str = s.type == JV_STR ? s.u.str->data : "";
  int slen = s.type == JV_STR ? s.u.str->len : 0;
  p = re_compile_obj(ctx, rx, &caps_n);
  if (!p)
    return s;
  caps = (int *)malloc((unsigned)caps_n * sizeof *caps);
  if (!caps) {
    re_free(p);
    return s;
  }
  ic = re_obj_flag(ctx, rx, "ignoreCase");
  ml = re_obj_flag(ctx, rx, "multiline");
  da = re_obj_flag(ctx, rx, "dotAll");
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  KEEP(s);
  KEEP(repl);
  while (pos <= slen && re_search(p, str, slen, pos, ic, ml, da, caps, caps_n) >= 0) {
    sb_put(&sb, str + last, caps[0] - last);
    if (isfn) {
      js_value cargs[16], rv;
      int g, na = 0;
      cargs[na++] = str_of(ctx, str + caps[0], caps[1] - caps[0]);
      for (g = 1; g <= p->ngroups && na < 13; g++) {
        int a = caps[2 * g], b = caps[2 * g + 1];
        cargs[na++] = (a >= 0 && b >= a) ? str_of(ctx, str + a, b - a) : js_undef();
      }
      cargs[na++] = js_number(caps[0]);
      cargs[na++] = s;
      rv = js_call(ctx, repl, js_undef(), cargs, na);
      if (ctx->has_exc)
        break;
      {
        js_value rs = js_to_string(ctx, rv);
        sb_put(&sb, rs.u.str->data, rs.u.str->len);
      }
    } else {
      js_value rs = js_to_string(ctx, repl);
      re_expand_repl(&sb, rs.u.str->data, rs.u.str->len, str, slen, caps, p->ngroups, p->names);
    }
    last = caps[1];
    pos = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
    if (caps[1] == caps[0] && caps[0] < slen)
      sb_putc(&sb, str[caps[0]]); /* keep the char an empty match sat on */
    if (caps[1] == caps[0])
      last = pos;
    if (!global)
      break;
  }
  sb_put(&sb, str + last, slen - last);
  gc_pop_scope();
  if (global)
    js_set(ctx, rx, "lastIndex", js_number(0));
  out = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  free(caps);
  re_free(p);
  return out;
}

js_value js_make_regexp(js_ctx *ctx, const char *source, const char *flags)
{
  js_object *o;
  js_value ov;
  if (!source)
    source = "";
  if (!flags)
    flags = "";
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->regexp_proto));
  js_def(o, "source", js_mk_cstring(ctx, source[0] ? source : "(?:)"), P_ENUMERABLE);
  js_def(o, "flags", js_mk_cstring(ctx, flags), P_ENUMERABLE);
  js_def(o, "global", js_bool(re_flag(flags, 'g')), P_ENUMERABLE);
  js_def(o, "ignoreCase", js_bool(re_flag(flags, 'i')), P_ENUMERABLE);
  js_def(o, "multiline", js_bool(re_flag(flags, 'm')), P_ENUMERABLE);
  js_def(o, "dotAll", js_bool(re_flag(flags, 's')), P_ENUMERABLE);
  js_def(o, "sticky", js_bool(re_flag(flags, 'y')), P_ENUMERABLE);
  js_def(o, "lastIndex", js_number(0), P_WRITABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
static js_value regexp_ctor(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  const char *src = "", *fl = "";
  js_value sv, fv;
  (void)thisv;
  if (n > 0 && re_is_regexp(ctx, a[0])) {
    sv = js_get(ctx, a[0], "source");
    fv = (n > 1 && a[1].type != JV_UNDEF) ? js_to_string(ctx, a[1]) : js_get(ctx, a[0], "flags");
  } else {
    sv = (n > 0 && a[0].type != JV_UNDEF) ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, "");
    fv = (n > 1 && a[1].type != JV_UNDEF) ? js_to_string(ctx, a[1]) : js_mk_cstring(ctx, "");
  }
  if (sv.type == JV_STR)
    src = sv.u.str->data;
  if (fv.type == JV_STR)
    fl = fv.u.str->data;
  return js_make_regexp(ctx, src, fl);
}

static js_value str_match(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), rx, r;
  gc_push_scope();
  KEEP(s);
  if (n > 0 && re_is_regexp(ctx, a[0]))
    rx = a[0];
  else {
    js_value pat = n > 0 && a[0].type != JV_UNDEF ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, "");
    rx = js_make_regexp(ctx, pat.u.str->data, "");
  }
  if (rx.type == JV_OBJ)
    gc_keep(rx.u.obj);
  r = re_str_match(ctx, s, rx);
  gc_pop_scope();
  return r;
}
static js_value str_matchall(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), rx, r;
  gc_push_scope();
  KEEP(s);
  if (n > 0 && re_is_regexp(ctx, a[0]))
    rx = a[0];
  else {
    js_value pat = n > 0 && a[0].type != JV_UNDEF ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, "");
    rx = js_make_regexp(ctx, pat.u.str->data, "g");
  }
  if (rx.type == JV_OBJ)
    gc_keep(rx.u.obj);
  r = re_str_matchall(ctx, s, rx);
  gc_pop_scope();
  return r;
}
static js_value str_search(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t), rx, m, a0;
  gc_push_scope();
  KEEP(s);
  if (n > 0 && re_is_regexp(ctx, a[0]))
    rx = a[0];
  else {
    js_value pat = n > 0 && a[0].type != JV_UNDEF ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, "");
    rx = js_make_regexp(ctx, pat.u.str->data, "");
  }
  if (rx.type == JV_OBJ)
    gc_keep(rx.u.obj);
  a0 = s;
  m = re_exec(ctx, rx, js_undef(), &a0, 1);
  gc_pop_scope();
  if (m.type == JV_NULL)
    return js_number(-1);
  return js_get(ctx, m, "index");
}

static js_value str_split(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = this_str(ctx, t);
  if (n > 0 && re_is_regexp(ctx, a[0]))
    return re_str_split(ctx, s, a[0], n > 1 ? a[1] : js_undef());
  js_object *out;
  int limit = n > 1 && a[1].type != JV_UNDEF ? (int)js_to_uint32(ctx, a[1]) : -1;
  gc_push_scope();
  KEEP(s);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n == 0 || a[0].type == JV_UNDEF) {
    js_array_push(ctx, out, s);
    gc_pop_scope();
    return js_obj_value(out);
  }
  {
    js_value sep = js_to_string(ctx, a[0]);
    KEEP(sep);
    if (sep.u.str->len == 0) {
      int i;
      for (i = 0; i < s.u.str->len; i++) {
        if (limit >= 0 && (int)out->elen >= limit)
          break;
        js_array_push(ctx, out, str_of(ctx, s.u.str->data + i, 1));
      }
    } else {
      int start = 0, at;
      while ((at = find_sub(s.u.str->data, s.u.str->len, sep.u.str->data, sep.u.str->len, start)) >= 0) {
        if (limit >= 0 && (int)out->elen >= limit)
          break;
        js_array_push(ctx, out, str_of(ctx, s.u.str->data + start, at - start));
        start = at + sep.u.str->len;
      }
      if (!(limit >= 0 && (int)out->elen >= limit))
        js_array_push(ctx, out, str_of(ctx, s.u.str->data + start, s.u.str->len - start));
    }
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value str_replace_impl(js_ctx *ctx, js_value t, js_value *a, int n, int all)
{
  js_value s = this_str(ctx, t);
  if (n > 0 && re_is_regexp(ctx, ARG(0)))
    return re_str_replace(ctx, s, ARG(0), ARG(1), all);
  {
    js_value search = js_to_string(ctx, ARG(0)), repl = ARG(1);
    int fn = js_is_callable(repl);
    sbuf sb;
    int start = 0, at;
    sb.b = NULL;
    sb.n = 0;
    sb.cap = 0;
    (void)n;
    gc_push_scope();
    KEEP(s);
    KEEP(search);
    KEEP(repl);
    while ((at = find_sub(s.u.str->data, s.u.str->len, search.u.str->data, search.u.str->len, start)) >= 0) {
      sb_put(&sb, s.u.str->data + start, at - start);
      if (fn) {
        js_value cargs[3], rv;
        cargs[0] = str_of(ctx, search.u.str->data, search.u.str->len);
        cargs[1] = js_number(at);
        cargs[2] = s;
        rv = js_call(ctx, repl, js_undef(), cargs, 3);
        if (ctx->has_exc)
          break;
        {
          js_value rs = js_to_string(ctx, rv);
          sb_put(&sb, rs.u.str->data, rs.u.str->len);
        }
      } else {
        js_value rs = js_to_string(ctx, repl);
        sb_put(&sb, rs.u.str->data, rs.u.str->len);
      }
      start = at + (search.u.str->len ? search.u.str->len : 1);
      if (!all)
        break;
      if (search.u.str->len == 0 && start <= s.u.str->len) {
        if (start <= s.u.str->len && at < s.u.str->len)
          sb_putc(&sb, s.u.str->data[at]);
      }
    }
    sb_put(&sb, s.u.str->data + start, s.u.str->len - start);
    gc_pop_scope();
    {
      js_value r = str_of(ctx, sb.b ? sb.b : "", sb.n);
      if (sb.b)
        free(sb.b);
      return r;
    }
  }
}
static js_value str_replace(js_ctx *ctx, js_value t, js_value *a, int n) { return str_replace_impl(ctx, t, a, n, 0); }
static js_value str_replaceall(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return str_replace_impl(ctx, t, a, n, 1);
}
static js_value str_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return this_str(ctx, t);
}

/* ------------------------------------------------------- Number ----------- */

static double parse_int(const char *s, int len, int radix)
{
  int i = 0, neg = 0;
  double v = 0;
  int any = 0;
  while (i < len && is_ws(s[i]))
    i++;
  if (i < len && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    i++;
  }
  if ((radix == 16 || radix == 0) && i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
    i += 2;
    radix = 16;
  }
  if (radix == 0)
    radix = 10;
  for (; i < len; i++) {
    int d;
    char c = s[i];
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'z')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
      d = c - 'A' + 10;
    else
      break;
    if (d >= radix)
      break;
    v = v * radix + d;
    any = 1;
  }
  if (!any)
    return 0.0 / 0.0;
  return neg ? -v : v;
}
static js_value g_parseint(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  int radix = n > 1 ? js_to_int_sat(ctx, a[1]) : 0;
  (void)t;
  return js_number(parse_int(s.u.str->data, s.u.str->len, radix));
}
static js_value g_parsefloat(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  const char *p = s.u.str->data;
  char buf[340];
  int i = 0, j = 0;
  char *end;
  double v;
  (void)t;
  (void)n;
  while (i < s.u.str->len && is_ws(p[i]))
    i++;
  while (i < s.u.str->len && j < 338 &&
         (p[i] == '+' || p[i] == '-' || p[i] == '.' || p[i] == 'e' || p[i] == 'E' || (p[i] >= '0' && p[i] <= '9')))
    buf[j++] = p[i++];
  buf[j] = 0;
  if (j == 0)
    return js_number(0.0 / 0.0);
  v = strtod(buf, &end);
  if (end == buf)
    return js_number(0.0 / 0.0);
  return js_number(v);
}
static js_value g_isnan(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  (void)n;
  return js_bool(x != x);
}
static js_value g_isfinite(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  (void)t;
  (void)n;
  return js_bool(x == x && x != 1.0 / 0.0 && x != -1.0 / 0.0);
}

/* encode/decodeURI{,Component}. The engine stores strings as UTF-8 bytes, so
 * the spec's "encode each code point's UTF-8" reduces to percent-encoding raw
 * bytes. */
static int uri_unreserved(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}
static int uri_reserved(unsigned char c)
{
  return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' || c == '=' || c == '+' || c == '$' ||
         c == ',' || c == '#';
}
static js_value uri_encode(js_ctx *ctx, js_value s, int keep_reserved)
{
  static const char hex[] = "0123456789ABCDEF";
  sbuf sb;
  int i;
  js_value r;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  for (i = 0; i < s.u.str->len; i++) {
    unsigned char c = (unsigned char)s.u.str->data[i];
    if (uri_unreserved(c) || (keep_reserved && uri_reserved(c)))
      sb_putc(&sb, (char)c);
    else {
      sb_putc(&sb, '%');
      sb_putc(&sb, hex[c >> 4]);
      sb_putc(&sb, hex[c & 15]);
    }
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static int uri_hex(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}
static js_value uri_decode(js_ctx *ctx, js_value s, int keep_reserved)
{
  sbuf sb;
  int i;
  js_value r;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  for (i = 0; i < s.u.str->len; i++) {
    char c = s.u.str->data[i];
    if (c != '%') {
      sb_putc(&sb, c);
      continue;
    }
    if (i + 2 >= s.u.str->len) {
      if (sb.b)
        free(sb.b);
      return js_throw_error(ctx, ctx->error_proto, "URIError", "URI malformed");
    }
    {
      int hi = uri_hex(s.u.str->data[i + 1]), lo = uri_hex(s.u.str->data[i + 2]);
      if (hi < 0 || lo < 0) {
        if (sb.b)
          free(sb.b);
        return js_throw_error(ctx, ctx->error_proto, "URIError", "URI malformed");
      }
      {
        unsigned char b = (unsigned char)((hi << 4) | lo);
        if (keep_reserved && b < 0x80 && uri_reserved(b)) {
          sb_putc(&sb, '%');
          sb_putc(&sb, s.u.str->data[i + 1]);
          sb_putc(&sb, s.u.str->data[i + 2]);
        } else
          sb_putc(&sb, (char)b);
        i += 2;
      }
    }
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value g_encode_uri_component(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)n;
  return uri_encode(ctx, js_to_string(ctx, ARG(0)), 0);
}
static js_value g_encode_uri(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)n;
  return uri_encode(ctx, js_to_string(ctx, ARG(0)), 1);
}
static js_value g_decode_uri_component(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)n;
  return uri_decode(ctx, js_to_string(ctx, ARG(0)), 0);
}
static js_value g_decode_uri(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)n;
  return uri_decode(ctx, js_to_string(ctx, ARG(0)), 1);
}

/* Legacy escape()/unescape(): %XX (and %uXXXX) over the unreserved set
 * [A-Za-z0-9 @*_+-./]. Byte-oriented over our UTF-8 strings. */
static int esc_unreserved(int c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '@' || c == '*' ||
         c == '_' || c == '+' || c == '-' || c == '.' || c == '/';
}
static js_value g_escape(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  js_value r;
  sbuf sb;
  int i;
  static const char hex[] = "0123456789ABCDEF";
  (void)t;
  (void)n;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  KEEP(s);
  for (i = 0; i < s.u.str->len; i++) {
    unsigned char c = (unsigned char)s.u.str->data[i];
    if (esc_unreserved(c))
      sb_putc(&sb, (char)c);
    else {
      sb_putc(&sb, '%');
      sb_putc(&sb, hex[c >> 4]);
      sb_putc(&sb, hex[c & 15]);
    }
  }
  gc_pop_scope();
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static int esc_hex(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static js_value g_unescape(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  js_value r;
  sbuf sb;
  int i, len;
  char tmp[4];
  (void)t;
  (void)n;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  len = s.u.str->len;
  gc_push_scope();
  KEEP(s);
  for (i = 0; i < len; i++) {
    const char *d = s.u.str->data;
    if (d[i] == '%' && i + 5 < len + 1 && d[i + 1] == 'u' && esc_hex(d[i + 2]) >= 0 && esc_hex(d[i + 3]) >= 0 &&
        esc_hex(d[i + 4]) >= 0 && esc_hex(d[i + 5]) >= 0) {
      unsigned cp = (unsigned)((esc_hex(d[i + 2]) << 12) | (esc_hex(d[i + 3]) << 8) | (esc_hex(d[i + 4]) << 4) |
                               esc_hex(d[i + 5]));
      int k = utf8_encode(cp, tmp);
      sb_put(&sb, tmp, k);
      i += 5;
    } else if (d[i] == '%' && i + 2 < len && esc_hex(d[i + 1]) >= 0 && esc_hex(d[i + 2]) >= 0) {
      sb_putc(&sb, (char)((esc_hex(d[i + 1]) << 4) | esc_hex(d[i + 2])));
      i += 2;
    } else
      sb_putc(&sb, d[i]);
  }
  gc_pop_scope();
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value num_isinteger(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)ctx;
  (void)t;
  (void)n;
  return js_bool(x.type == JV_NUM && x.u.num == x.u.num && x.u.num != 1.0 / 0.0 && x.u.num != -1.0 / 0.0 &&
                 floor(x.u.num) == x.u.num);
}
static js_value num_isfinite(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)ctx;
  (void)t;
  (void)n;
  return js_bool(x.type == JV_NUM && x.u.num == x.u.num && x.u.num != 1.0 / 0.0 && x.u.num != -1.0 / 0.0);
}
static js_value num_isnan(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)ctx;
  (void)t;
  (void)n;
  return js_bool(x.type == JV_NUM && x.u.num != x.u.num);
}
static js_value num_issafe(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value x = ARG(0);
  (void)ctx;
  (void)t;
  (void)n;
  return js_bool(x.type == JV_NUM && floor(x.u.num) == x.u.num && fabs(x.u.num) <= 9007199254740991.0);
}
static js_value native_number(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  if (n == 0)
    return js_number(0);
  if (a[0].type == JV_BIGINT)
    return js_number(bi_to_double(js_bigint_of(a[0])));
  return js_number(js_to_number(ctx, a[0]));
}
static js_value native_string(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  if (n == 0)
    return js_mk_cstring(ctx, "");
  if (a[0].type == JV_SYMBOL)
    return symbol_tostring(ctx, a[0], 0, 0);
  return js_to_string(ctx, a[0]);
}
static js_value native_boolean(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)ctx;
  return js_bool(js_to_boolean(ARG(0)));
}

/* ---- BigInt ---- */
static js_value bigint_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value v = ARG(0);
  (void)t;
  (void)n;
  if (v.type == JV_BIGINT)
    return v;
  if (v.type == JV_BOOL)
    return js_mk_bigint(ctx, bi_from_i64(v.u.boolean ? 1 : 0));
  if (v.type == JV_NUM) {
    int ok;
    bigint *b = bi_from_double(v.u.num, &ok);
    if (!ok) {
      if (b)
        bi_free(b);
      return js_throw_range(ctx, "The number is not a safe integer");
    }
    return js_mk_bigint(ctx, b);
  }
  if (v.type == JV_STR) {
    int ok;
    bigint *b = bi_from_str(v.u.str->data, v.u.str->len, &ok);
    if (!ok) {
      if (b)
        bi_free(b);
      return js_throw_error(ctx, ctx->syntax_error_proto, "SyntaxError", "Cannot convert string to a BigInt");
    }
    return js_mk_bigint(ctx, b);
  }
  if (v.type == JV_OBJ) {
    js_value p = js_to_primitive(ctx, v, 1);
    if (ctx->has_exc)
      return js_undef();
    if (p.type != JV_OBJ)
      return bigint_ctor(ctx, t, &p, 1);
  }
  return js_throw_type(ctx, "Cannot convert value to a BigInt");
}
static js_value bigint_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  bigint *b = js_bigint_of(t);
  char *s;
  js_value r;
  int radix = 10;
  if (!b)
    return js_throw_type(ctx, "BigInt.prototype.toString requires a BigInt");
  if (n > 0 && a[0].type != JV_UNDEF) {
    radix = js_to_int_sat(ctx, a[0]);
    if (radix < 2 || radix > 36)
      return js_throw_range(ctx, "toString() radix must be between 2 and 36");
  }
  s = bi_to_str(b, radix);
  if (!s)
    return js_throw_range(ctx, "out of memory");
  r = js_mk_cstring(ctx, s);
  free(s);
  return r;
}
static js_value bigint_valueof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  bigint *b = js_bigint_of(t);
  (void)a;
  (void)n;
  if (!b)
    return js_throw_type(ctx, "BigInt.prototype.valueOf requires a BigInt");
  if (t.type == JV_BIGINT)
    return t;
  return js_mk_bigint(ctx, bi_copy(b)); /* unbox Object(bigint) */
}
static js_value bigint_asuintn(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int bits;
  bigint *b;
  (void)t;
  (void)n;
  bits = js_to_int_sat(ctx, ARG(0));
  b = js_bigint_of(ARG(1));
  if (!b)
    return js_throw_type(ctx, "BigInt.asUintN requires a BigInt");
  if (bits < 0)
    return js_throw_range(ctx, "Invalid number of bits");
  return js_mk_bigint(ctx, bi_as_uintn(bits, b));
}
static js_value bigint_asintn(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int bits;
  bigint *b;
  (void)t;
  (void)n;
  bits = js_to_int_sat(ctx, ARG(0));
  b = js_bigint_of(ARG(1));
  if (!b)
    return js_throw_type(ctx, "BigInt.asIntN requires a BigInt");
  if (bits < 0)
    return js_throw_range(ctx, "Invalid number of bits");
  return js_mk_bigint(ctx, bi_as_intn(bits, b));
}

static js_value num_tofixed(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, t);
  int digits = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  char buf[80], ds[40];
  int p = 0, i, dl, neg = 0;
  double scale = 1, scaled;
  if (x != x)
    return js_mk_cstring(ctx, "NaN");
  if (x == 1.0 / 0.0)
    return js_mk_cstring(ctx, "Infinity");
  if (x == -1.0 / 0.0)
    return js_mk_cstring(ctx, "-Infinity");
  if (digits < 0)
    digits = 0;
  if (digits > 20)
    digits = 20;
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  for (i = 0; i < digits; i++)
    scale *= 10;
  scaled = floor(x * scale + 0.5); /* round once, as an integer     */
  /* render `scaled` as a digit string, then place the decimal point exactly */
  if (scaled < 1.8e19)
    dl = u64toa((unsigned long long)scaled, ds, 10);
  else
    dl = dtoa(scaled, ds);
  if (neg)
    buf[p++] = '-';
  if (digits == 0) {
    memcpy(buf + p, ds, (unsigned)dl);
    p += dl;
    return str_of(ctx, buf, p);
  }
  if (dl <= digits) {
    buf[p++] = '0';
    buf[p++] = '.';
    for (i = 0; i < digits - dl; i++)
      buf[p++] = '0';
    memcpy(buf + p, ds, (unsigned)dl);
    p += dl;
  } else {
    int intlen = dl - digits;
    memcpy(buf + p, ds, (unsigned)intlen);
    p += intlen;
    buf[p++] = '.';
    memcpy(buf + p, ds + intlen, (unsigned)digits);
    p += digits;
  }
  return str_of(ctx, buf, p);
}
static js_value num_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, t);
  int radix = n > 0 && a[0].type != JV_UNDEF ? js_to_int_sat(ctx, a[0]) : 10;
  char buf[DTOA_RADIX_MAX];
  if (radix == 10) {
    dtoa(x, buf);
    return js_mk_cstring(ctx, buf);
  }
  if (radix < 2 || radix > 36)
    return js_throw_range(ctx, "toString() radix must be between 2 and 36");
  dtoa_radix(x, radix, buf);
  return js_mk_cstring(ctx, buf);
}
/* x>0: write exactly `sig` (>=1) significant ASCII digits to ds (no NUL) and
 * set *expo so x ~= ds[0].ds[1..] * 10^(*expo). Rounds like toFixed
 * (round-half-up on a scaled integer); real precision caps at 17 digits, the
 * rest pad with zeros. */
static void num_sigdigits(double x, int sig, char *ds, int *expo)
{
  int real = sig > 17 ? 17 : sig;
  int e = (int)floor(log10(x));
  double lo = pow(10.0, (double)(real - 1)), hi = lo * 10.0;
  double m = floor(x * pow(10.0, (double)(real - 1 - e)) + 0.5);
  char tmp[24];
  int dl, i;
  if (m >= hi) {
    e++;
    m = floor(x * pow(10.0, (double)(real - 1 - e)) + 0.5);
  } else if (m < lo) {
    e--;
    m = floor(x * pow(10.0, (double)(real - 1 - e)) + 0.5);
  }
  if (m >= hi) {
    m = floor(m / 10.0 + 0.5);
    e++;
  } /* boundary carry guard */
  dl = u64toa((unsigned long long)m, tmp, 10);
  for (i = 0; i < dl && i < sig; i++)
    ds[i] = tmp[i];
  for (; i < sig; i++)
    ds[i] = '0';
  *expo = e;
}

/* x>0: derive the shortest significant digits (and their exponent) from dtoa's
 * shortest decimal. Returns the digit count; ds gets that many ASCII digits. */
static int num_shortest_sig(double x, char *ds, int *expo)
{
  char t[DTOA_MAX];
  char digs[40];
  int i = 0, ndig = 0, pointidx = -1, exp10 = 0, first, last, cnt = 0, k;
  dtoa(x, t);
  while (t[i] && t[i] != 'e' && t[i] != 'E') {
    if (t[i] == '.')
      pointidx = ndig;
    else if (ndig < 40)
      digs[ndig++] = t[i];
    i++;
  }
  if (pointidx < 0)
    pointidx = ndig; /* no '.' -> point after all digits */
  if (t[i] == 'e' || t[i] == 'E') {
    int es = 1, ev = 0;
    i++;
    if (t[i] == '+')
      i++;
    else if (t[i] == '-') {
      es = -1;
      i++;
    }
    while (t[i] >= '0' && t[i] <= '9') {
      ev = ev * 10 + (t[i] - '0');
      i++;
    }
    exp10 = es * ev;
  }
  first = 0;
  while (first < ndig && digs[first] == '0')
    first++;
  if (first == ndig) {
    ds[0] = '0';
    *expo = 0;
    return 1;
  }
  last = ndig;
  while (last > first && digs[last - 1] == '0')
    last--;
  *expo = (pointidx - 1 - first) + exp10;
  for (k = first; k < last; k++)
    ds[cnt++] = digs[k];
  return cnt;
}

static void num_put_exp(char *buf, int *pp, int expo)
{
  int p = *pp, ax = expo < 0 ? -expo : expo, el, i;
  char eb[8];
  buf[p++] = 'e';
  buf[p++] = expo < 0 ? '-' : '+';
  el = u64toa((unsigned long long)ax, eb, 10);
  for (i = 0; i < el; i++)
    buf[p++] = eb[i];
  *pp = p;
}

static js_value num_toexp(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, t);
  int has = n > 0 && a[0].type != JV_UNDEF;
  int f = has ? js_to_int_sat(ctx, a[0]) : 0;
  char buf[160], ds[128] = {0};
  int p = 0, neg = 0, expo = 0, sig, i;
  if (x != x)
    return js_mk_cstring(ctx, "NaN");
  if (x == 1.0 / 0.0)
    return js_mk_cstring(ctx, "Infinity");
  if (x == -1.0 / 0.0)
    return js_mk_cstring(ctx, "-Infinity");
  if (has && (f < 0 || f > 100))
    return js_throw_range(ctx, "toExponential() argument must be between 0 and 100");
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  if (x == 0) {
    sig = has ? f + 1 : 1;
    for (i = 0; i < sig; i++)
      ds[i] = '0';
    expo = 0;
  } else if (has) {
    sig = f + 1;
    num_sigdigits(x, sig, ds, &expo);
  } else
    sig = num_shortest_sig(x, ds, &expo);
  if (neg)
    buf[p++] = '-';
  buf[p++] = ds[0];
  if (sig > 1) {
    buf[p++] = '.';
    for (i = 1; i < sig; i++)
      buf[p++] = ds[i];
  }
  num_put_exp(buf, &p, expo);
  return str_of(ctx, buf, p);
}

static js_value num_toprecision(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, t);
  int prec, p = 0, neg = 0, expo = 0, i, k;
  char buf[160], ds[128];
  if (n == 0 || a[0].type == JV_UNDEF)
    return num_tostring(ctx, t, a, 0);
  if (x != x)
    return js_mk_cstring(ctx, "NaN");
  if (x == 1.0 / 0.0)
    return js_mk_cstring(ctx, "Infinity");
  if (x == -1.0 / 0.0)
    return js_mk_cstring(ctx, "-Infinity");
  prec = js_to_int_sat(ctx, a[0]);
  if (prec < 1 || prec > 100)
    return js_throw_range(ctx, "toPrecision() argument must be between 1 and 100");
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  if (x == 0) {
    for (i = 0; i < prec; i++)
      ds[i] = '0';
    expo = 0;
  } else
    num_sigdigits(x, prec, ds, &expo);
  if (neg)
    buf[p++] = '-';
  if (expo < -6 || expo >= prec) { /* exponential notation */
    buf[p++] = ds[0];
    if (prec > 1) {
      buf[p++] = '.';
      for (i = 1; i < prec; i++)
        buf[p++] = ds[i];
    }
    num_put_exp(buf, &p, expo);
  } else if (expo >= 0) { /* point after expo+1 digits */
    for (i = 0; i <= expo; i++)
      buf[p++] = ds[i];
    if (prec > expo + 1) {
      buf[p++] = '.';
      for (i = expo + 1; i < prec; i++)
        buf[p++] = ds[i];
    }
  } else { /* 0.00...digits, expo in [-6,-1] */
    buf[p++] = '0';
    buf[p++] = '.';
    for (k = 0; k < -expo - 1; k++)
      buf[p++] = '0';
    for (i = 0; i < prec; i++)
      buf[p++] = ds[i];
  }
  return str_of(ctx, buf, p);
}

static js_value prim_valueof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)a;
  (void)n;
  return t;
}
static js_value bool_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return js_mk_cstring(ctx, js_to_boolean(t) ? "true" : "false");
}

/* ------------------------------------------------------- Error ------------ */

static js_value make_error_native(js_ctx *ctx, js_object *proto, const char *name, js_value *args, int argc)
{
  const char *msg = "";
  js_value ms = js_undef();
  if (argc > 0 && args[0].type != JV_UNDEF) {
    ms = js_to_string(ctx, args[0]);
    msg = ms.u.str->data;
  }
  return js_make_error(ctx, proto, name, msg);
}
static js_value err_error(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->error_proto, "Error", a, n);
}
static js_value err_type(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->type_error_proto, "TypeError", a, n);
}
static js_value err_range(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->range_error_proto, "RangeError", a, n);
}
static js_value err_syntax(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->syntax_error_proto, "SyntaxError", a, n);
}
static js_value err_ref(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->ref_error_proto, "ReferenceError", a, n);
}
static js_value err_evalerr(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->eval_error_proto, "EvalError", a, n);
}
static js_value err_uri(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->uri_error_proto, "URIError", a, n);
}
/* AggregateError(errors, message): the message is arg 1, so pass it as the
 * error text. */
static js_value err_aggregate(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return make_error_native(ctx, ctx->aggregate_error_proto, "AggregateError", a + (n > 0 ? 1 : 0), n > 0 ? n - 1 : 0);
}
static js_value err_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value nm = js_get(ctx, t, "name"), msg = js_get(ctx, t, "message");
  js_value ns = js_to_string(ctx, nm), ms = js_to_string(ctx, msg);
  (void)a;
  (void)n;
  if (ms.u.str->len == 0)
    return ns;
  gc_push_scope();
  KEEP(ns);
  KEEP(ms);
  {
    js_string *sep = js_mk_cstring(ctx, ": ").u.str;
    js_string *r = js_str_concat(ctx, js_str_concat(ctx, ns.u.str, sep), ms.u.str);
    js_value out;
    out.type = JV_STR;
    out.u.str = r;
    gc_pop_scope();
    return out;
  }
}

/* ------------------------------------------------------- JSON ------------- */

static void json_quote(sbuf *sb, const char *s, int len)
{
  int i;
  sb_putc(sb, '"');
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"':
      sb_puts(sb, "\\\"");
      break;
    case '\\':
      sb_puts(sb, "\\\\");
      break;
    case '\n':
      sb_puts(sb, "\\n");
      break;
    case '\t':
      sb_puts(sb, "\\t");
      break;
    case '\r':
      sb_puts(sb, "\\r");
      break;
    case '\b':
      sb_puts(sb, "\\b");
      break;
    case '\f':
      sb_puts(sb, "\\f");
      break;
    default:
      if (c < 0x20) {
        char h[8];
        const char *hx = "0123456789abcdef";
        h[0] = '\\';
        h[1] = 'u';
        h[2] = '0';
        h[3] = '0';
        h[4] = hx[c >> 4];
        h[5] = hx[c & 15];
        sb_put(sb, h, 6);
      } else
        sb_putc(sb, (char)c);
    }
  }
  sb_putc(sb, '"');
}
typedef struct {
  js_ctx *ctx;
  sbuf sb;
  char gap[11];
  js_value fn;       /* replacer function, or undefined */
  js_object *allow;  /* replacer array: the property whitelist */
  js_object **stack; /* objects being serialized, for cycle detection */
  int nstack, capstack;
} jstr;

static char *json_dup(const char *s)
{
  unsigned n = strlen(s) + 1;
  char *d = (char *)malloc(n);
  if (d)
    memcpy(d, s, n);
  return d; /* NULL on OOM: the caller skips that key */
}
static void json_indent(jstr *j, int depth)
{
  int i;
  if (!j->gap[0])
    return;
  sb_putc(&j->sb, '\n');
  for (i = 0; i < depth; i++)
    sb_puts(&j->sb, j->gap);
}
static int jstr_enter(jstr *j, js_object *o)
{
  int i;
  for (i = 0; i < j->nstack; i++)
    if (j->stack[i] == o) {
      js_throw_type(j->ctx, "Converting circular structure to JSON");
      return 0;
    }
  if (j->nstack == j->capstack) {
    int nc = j->capstack ? j->capstack * 2 : 16;
    js_object **ns = (js_object **)realloc(j->stack, (unsigned)nc * sizeof *ns);
    if (!ns) {
      js_throw_range(j->ctx, "out of memory");
      return 0;
    }
    j->stack = ns;
    j->capstack = nc;
  }
  j->stack[j->nstack++] = o;
  return 1;
}

/* Serialize holder[key] (read into v); 1 if emitted, 0 if omitted or on error */
static int jstr_val(jstr *j, js_value holder, const char *key, js_value v, int depth)
{
  js_ctx *ctx = j->ctx;
  char tmp[DTOA_MAX], probe;
  js_value kv = js_undef();
  int emitted = 0;
  if (ctx->stack_limit && &probe < ctx->stack_limit) {
    js_throw_range(ctx, "maximum call stack size exceeded");
    return 0;
  }
  gc_push_scope();
  KEEP(holder);
  KEEP(v);
  kv = js_mk_cstring(ctx, key);
  KEEP(kv);
  /* toJSON hook */
  if (v.type == JV_OBJ) {
    js_value tj = js_get(ctx, v, "toJSON");
    if (js_is_callable(tj)) {
      KEEP(tj);
      v = js_call(ctx, tj, v, &kv, 1);
      KEEP(v);
    }
  }
  if (!ctx->has_exc && js_is_callable(j->fn)) {
    js_value args[2];
    args[0] = kv;
    args[1] = v;
    v = js_call(ctx, j->fn, holder, args, 2);
    KEEP(v);
  }
  if (ctx->has_exc) {
    gc_pop_scope();
    return 0;
  }
  switch (v.type) {
  case JV_NULL:
    sb_puts(&j->sb, "null");
    emitted = 1;
    break;
  case JV_BOOL:
    sb_puts(&j->sb, v.u.boolean ? "true" : "false");
    emitted = 1;
    break;
  case JV_NUM:
    if (v.u.num != v.u.num || v.u.num == 1.0 / 0.0 || v.u.num == -1.0 / 0.0)
      sb_puts(&j->sb, "null");
    else {
      dtoa(v.u.num, tmp);
      sb_puts(&j->sb, tmp);
    }
    emitted = 1;
    break;
  case JV_STR:
    json_quote(&j->sb, v.u.str->data, v.u.str->len);
    emitted = 1;
    break;
  case JV_BIGINT:
    js_throw_type(ctx, "Do not know how to serialize a BigInt");
    break;
  case JV_OBJ:
    if (js_is_callable(v))
      break;
    if (!jstr_enter(j, v.u.obj))
      break;
    emitted = 1;
    if (v.u.obj->kind == OBJ_ARRAY) {
      js_object *arr = v.u.obj;
      unsigned i;
      sb_putc(&j->sb, '[');
      for (i = 0; i < arr->elen && !ctx->has_exc; i++) {
        char idx[16];
        js_value ev = arr->elems[i];
        u64toa(i, idx, 10);
        if (i)
          sb_putc(&j->sb, ',');
        json_indent(j, depth + 1);
        if (!jstr_val(j, v, idx, ev, depth + 1) && !ctx->has_exc)
          sb_puts(&j->sb, "null");
      }
      if (arr->elen)
        json_indent(j, depth);
      sb_putc(&j->sb, ']');
    } else {
      js_object *o = v.u.obj;
      char **keys = NULL;
      int nk = 0, i, first = 1;
      if (j->allow) {
        int na = (int)js_array_len(j->allow);
        keys = (char **)calloc((unsigned)na + 1, sizeof *keys);
        for (i = 0; keys && i < na; i++) {
          js_value kk = js_to_string(ctx, js_get_index(ctx, js_obj_value(j->allow), (unsigned)i));
          if (kk.type == JV_STR)
            keys[nk++] = json_dup(kk.u.str->data);
        }
      } else {
        keys = (char **)calloc((unsigned)o->props.n + 1, sizeof *keys);
        for (i = 0; keys && i < o->props.n; i++) {
          pentry *pe = &o->props.e[i];
          if ((pe->flags & P_ENUMERABLE) && (unsigned char)pe->key[0] >= 0x20)
            keys[nk++] = json_dup(pe->key);
        }
      }
      sb_putc(&j->sb, '{');
      for (i = 0; keys && i < nk && !ctx->has_exc; i++) {
        js_value pv;
        int mark;
        if (!keys[i])
          continue;
        pv = js_get(ctx, v, keys[i]);
        if (ctx->has_exc)
          break;
        KEEP(pv);
        mark = j->sb.n;
        if (!first)
          sb_putc(&j->sb, ',');
        json_indent(j, depth + 1);
        json_quote(&j->sb, keys[i], (int)strlen(keys[i]));
        sb_putc(&j->sb, ':');
        if (j->gap[0])
          sb_putc(&j->sb, ' ');
        if (jstr_val(j, v, keys[i], pv, depth + 1))
          first = 0;
        else
          j->sb.n = mark;
      }
      if (!first)
        json_indent(j, depth);
      sb_putc(&j->sb, '}');
      for (i = 0; keys && i < nk; i++)
        free(keys[i]);
      free(keys);
    }
    j->nstack--;
    break;
  default:
    break;
  }
  gc_pop_scope();
  return emitted;
}
static js_value json_stringify(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value v = ARG(0), repl = ARG(1), space = ARG(2), holder = js_undef(), r = js_undef();
  jstr j;
  int ok;
  (void)t;
  memset(&j, 0, sizeof j);
  j.ctx = ctx;
  j.fn = js_undef();
  if (space.type == JV_NUM) {
    int k = js_to_int_sat(ctx, space), i;
    if (k > 10)
      k = 10;
    for (i = 0; i < k; i++)
      j.gap[i] = ' ';
  } else if (space.type == JV_STR) {
    int k = space.u.str->len;
    if (k > 10)
      k = 10;
    memcpy(j.gap, space.u.str->data, (unsigned)k);
  }
  gc_push_scope();
  KEEP(v);
  KEEP(repl);
  if (js_is_callable(repl)) {
    j.fn = repl;
    holder = js_obj_value((js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)));
    js_set(ctx, holder, "", v);
  } else if (repl.type == JV_OBJ && repl.u.obj->kind == OBJ_ARRAY)
    j.allow = repl.u.obj;
  ok = jstr_val(&j, holder, "", v, 0);
  gc_pop_scope();
  free(j.stack);
  if (j.sb.failed && !ctx->has_exc)
    js_throw_range(ctx, "out of memory");
  else if (!ctx->has_exc && ok)
    r = str_of(ctx, j.sb.b ? j.sb.b : "", j.sb.n);
  free(j.sb.b);
  return r;
}

typedef struct {
  js_ctx *ctx;
  const char *s;
  int len, pos, depth;
} jparse;
static void jp_ws(jparse *p)
{
  while (p->pos < p->len && is_ws(p->s[p->pos]))
    p->pos++;
}
static js_value jp_value(jparse *p);
static js_value jp_fail(jparse *p)
{
  js_throw_error(p->ctx, p->ctx->syntax_error_proto, "SyntaxError", "Unexpected token in JSON");
  return js_undef();
}
static js_value jp_string(jparse *p)
{
  sbuf sb;
  js_value r;
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  p->pos++; /* opening quote */
  while (p->pos < p->len && p->s[p->pos] != '"') {
    char c = p->s[p->pos++];
    if (c == '\\' && p->pos < p->len) {
      char e = p->s[p->pos++];
      switch (e) {
      case 'n':
        sb_putc(&sb, '\n');
        break;
      case 't':
        sb_putc(&sb, '\t');
        break;
      case 'r':
        sb_putc(&sb, '\r');
        break;
      case 'b':
        sb_putc(&sb, '\b');
        break;
      case 'f':
        sb_putc(&sb, '\f');
        break;
      case '/':
        sb_putc(&sb, '/');
        break;
      case '"':
        sb_putc(&sb, '"');
        break;
      case '\\':
        sb_putc(&sb, '\\');
        break;
      case 'u': {
        unsigned cp = 0;
        int i;
        char enc[4];
        int k;
        for (i = 0; i < 4 && p->pos < p->len; i++) {
          char h = p->s[p->pos++];
          cp <<= 4;
          if (h >= '0' && h <= '9')
            cp |= h - '0';
          else if (h >= 'a' && h <= 'f')
            cp |= h - 'a' + 10;
          else if (h >= 'A' && h <= 'F')
            cp |= h - 'A' + 10;
        }
        if (cp >= 0xD800 && cp < 0xDC00 && p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
          unsigned lo = 0; /* join a \uD83D\uDE00 pair */
          for (i = 0; i < 4; i++) {
            char h = p->s[p->pos + 2 + i];
            lo <<= 4;
            if (h >= '0' && h <= '9')
              lo |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f')
              lo |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
              lo |= (unsigned)(h - 'A' + 10);
            else
              lo = 0;
          }
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p->pos += 6;
          }
        }
        k = utf8_encode(cp, enc);
        sb_put(&sb, enc, k);
        break;
      }
      default:
        sb_putc(&sb, e);
      }
    } else
      sb_putc(&sb, c);
  }
  p->pos++; /* closing quote */
  r = str_of(p->ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value jp_value1(jparse *p);
static js_value jp_value(jparse *p)
{
  char probe;
  js_value r;
  if (p->ctx->stack_limit && &probe < p->ctx->stack_limit) {
    js_throw_range(p->ctx, "maximum call stack size exceeded");
    return js_undef();
  }
  p->depth++;
  r = jp_value1(p);
  p->depth--;
  return r;
}
static js_value jp_value1(jparse *p)
{
  jp_ws(p);
  if (p->pos >= p->len)
    return jp_fail(p);
  {
    char c = p->s[p->pos];
    if (c == '"')
      return jp_string(p);
    if (c == '{') {
      js_object *o;
      p->pos++;
      gc_push_scope();
      o = (js_object *)gc_keep(js_new_object(p->ctx, p->ctx->object_proto));
      jp_ws(p);
      if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        gc_pop_scope();
        return js_obj_value(o);
      }
      for (;;) {
        js_value k, v;
        jp_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != '"') {
          gc_pop_scope();
          return jp_fail(p);
        }
        k = jp_string(p);
        if (k.type != JV_STR) {
          gc_pop_scope();
          return jp_fail(p);
        }
        gc_keep(refp(k));
        jp_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
          gc_pop_scope();
          return jp_fail(p);
        }
        p->pos++;
        v = jp_value(p);
        if (p->ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        js_set(p->ctx, js_obj_value(o), k.u.str->data, v);
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
          p->pos++;
          continue;
        }
        if (p->pos < p->len && p->s[p->pos] == '}') {
          p->pos++;
          break;
        }
        gc_pop_scope();
        return jp_fail(p);
      }
      gc_pop_scope();
      return js_obj_value(o);
    }
    if (c == '[') {
      js_object *arr;
      p->pos++;
      gc_push_scope();
      arr = (js_object *)gc_keep(js_new_array(p->ctx, 0));
      jp_ws(p);
      if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        gc_pop_scope();
        return js_obj_value(arr);
      }
      for (;;) {
        js_value v = jp_value(p);
        if (p->ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        js_array_push(p->ctx, arr, v);
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
          p->pos++;
          continue;
        }
        if (p->pos < p->len && p->s[p->pos] == ']') {
          p->pos++;
          break;
        }
        gc_pop_scope();
        return jp_fail(p);
      }
      gc_pop_scope();
      return js_obj_value(arr);
    }
    if (c == 't' && p->pos + 4 <= p->len && memcmp_(p->s + p->pos, "true", 4) == 0) {
      p->pos += 4;
      return js_bool(1);
    }
    if (c == 'f' && p->pos + 5 <= p->len && memcmp_(p->s + p->pos, "false", 5) == 0) {
      p->pos += 5;
      return js_bool(0);
    }
    if (c == 'n' && p->pos + 4 <= p->len && memcmp_(p->s + p->pos, "null", 4) == 0) {
      p->pos += 4;
      return js_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      char buf[340];
      int j = 0;
      while (p->pos < p->len && j < 338 &&
             (p->s[p->pos] == '-' || p->s[p->pos] == '+' || p->s[p->pos] == '.' || p->s[p->pos] == 'e' ||
              p->s[p->pos] == 'E' || (p->s[p->pos] >= '0' && p->s[p->pos] <= '9')))
        buf[j++] = p->s[p->pos++];
      buf[j] = 0;
      return js_number(strtod(buf, NULL));
    }
  }
  return jp_fail(p);
}
/* JSON.parse reviver: walk bottom-up, replacing or dropping each member */
static js_value jp_revive(js_ctx *ctx, js_value holder, const char *key, js_value reviver)
{
  char probe;
  js_value val, kv, args[2];
  if (ctx->stack_limit && &probe < ctx->stack_limit)
    return js_throw_range(ctx, "maximum call stack size exceeded");
  gc_push_scope();
  KEEP(holder);
  KEEP(reviver);
  val = js_get(ctx, holder, key);
  KEEP(val);
  if (val.type == JV_OBJ && !ctx->has_exc) {
    js_object *o = val.u.obj;
    if (o->kind == OBJ_ARRAY) {
      unsigned i;
      for (i = 0; i < o->elen && !ctx->has_exc; i++) {
        char idx[16];
        js_value nv;
        u64toa(i, idx, 10);
        nv = jp_revive(ctx, val, idx, reviver);
        if (ctx->has_exc)
          break;
        KEEP(nv);
        js_set(ctx, val, idx, nv);
      }
    } else {
      int i, nk = 0;
      char **keys = (char **)calloc((unsigned)o->props.n + 1, sizeof *keys);
      for (i = 0; keys && i < o->props.n; i++) {
        pentry *pe = &o->props.e[i];
        unsigned kl = strlen(pe->key) + 1;
        if (!(pe->flags & P_ENUMERABLE) || !(keys[nk] = (char *)malloc(kl)))
          continue;
        memcpy(keys[nk++], pe->key, kl);
      }
      for (i = 0; i < nk && !ctx->has_exc; i++) {
        js_value nv = jp_revive(ctx, val, keys[i], reviver);
        if (ctx->has_exc)
          break;
        KEEP(nv);
        if (nv.type == JV_UNDEF)
          js_delete(o, keys[i]);
        else
          js_set(ctx, val, keys[i], nv);
      }
      for (i = 0; i < nk; i++)
        free(keys[i]);
      free(keys);
    }
  }
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  kv = js_mk_cstring(ctx, key);
  KEEP(kv);
  args[0] = kv;
  args[1] = val;
  val = js_call(ctx, reviver, holder, args, 2);
  gc_pop_scope();
  return val;
}
static js_value json_parse(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0)), reviver = ARG(1);
  jparse p;
  js_value r;
  (void)t;
  if (ctx->has_exc || s.type != JV_STR)
    return js_undef();
  p.ctx = ctx;
  p.s = s.u.str->data;
  p.len = s.u.str->len;
  p.pos = 0;
  p.depth = 0;
  gc_push_scope();
  KEEP(s);
  KEEP(reviver);
  r = jp_value(&p);
  if (!ctx->has_exc) {
    jp_ws(&p);
    if (p.pos < p.len)
      jp_fail(&p);
  }
  if (!ctx->has_exc && js_is_callable(reviver)) {
    js_value holder = js_obj_value((js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)));
    KEEP(r);
    js_set(ctx, holder, "", r);
    r = jp_revive(ctx, holder, "", reviver);
  }
  gc_pop_scope();
  return ctx->has_exc ? js_undef() : r;
}

/* ------------------------------------------------------- Promise ---------- *
 * A synchronous-settlement Promise: executors and .then reactions run
 * immediately (a "microtask" is run-now, since there is no event loop yet).
 * That makes await of an already-resolved promise correct; a promise that never
 * settles synchronously (e.g. waiting on a timer) just stays pending. State
 * lives in hidden own properties @s (0 pending/1 fulfilled/2 rejected), @v
 * (value/reason) and @r (a list of pending reactions). Resolver functions carry
 * their target promise in the native closure data (ctx->native_data). */

static int is_promise(js_value v) { return v.type == JV_OBJ && pmap_find_pub(v.u.obj, "@s") != 0; }
static int prom_state(js_object *p)
{
  pentry *pe = pmap_find_pub(p, "@s");
  return pe ? (int)pe->value.u.num : 0;
}
static js_value prom_value(js_object *p)
{
  pentry *pe = pmap_find_pub(p, "@v");
  return pe ? pe->value : js_undef();
}

static js_object *new_promise(js_ctx *ctx);
static void prom_settle(js_ctx *ctx, js_object *p, int state, js_value value);
static void prom_resolve(js_ctx *ctx, js_object *p, js_value v);
static js_value promise_then(js_ctx *ctx, js_value thisv, js_value *args, int argc);

static js_object *new_promise(js_ctx *ctx)
{
  js_object *p;
  gc_push_scope();
  p = (js_object *)gc_keep(js_new_object(ctx, ctx->promise_proto));
  js_def(p, "@s", js_number(0), 0);
  js_def(p, "@v", js_undef(), 0);
  js_def(p, "@r", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
  return p;
}
static js_object *resolver(js_ctx *ctx, js_native fn, js_object *p)
{
  js_object *f = js_new_native(ctx, fn, "", 1);
  f->fn->data = js_obj_value(p);
  gc_write_barrier(f);
  return f;
}
/* run one stored reaction [onF, onR, childResolve, childReject] */
static void run_reaction(js_ctx *ctx, int state, js_value value, js_value reaction)
{
  js_value onF = js_get_index(ctx, reaction, 0), onR = js_get_index(ctx, reaction, 1);
  js_value cres = js_get_index(ctx, reaction, 2), crej = js_get_index(ctx, reaction, 3);
  js_value handler = state == 1 ? onF : onR;
  gc_push_scope();
  KEEP(value);
  KEEP(cres);
  KEEP(crej);
  if (js_is_callable(handler)) {
    js_value out = js_call(ctx, handler, js_undef(), &value, 1);
    if (ctx->has_exc) {
      js_value e = ctx->exc;
      ctx->has_exc = 0;
      js_call(ctx, crej, js_undef(), &e, 1);
    } else
      js_call(ctx, cres, js_undef(), &out, 1);
  } else {
    js_call(ctx, state == 1 ? cres : crej, js_undef(), &value, 1); /* pass through */
  }
  gc_pop_scope();
}
static void prom_settle(js_ctx *ctx, js_object *p, int state, js_value value)
{
  js_value rs;
  if (prom_state(p) != 0)
    return; /* settle once */
  gc_push_scope();
  gc_keep(p);
  KEEP(value);
  js_def(p, "@s", js_number(state), 0);
  js_def(p, "@v", value, 0);
  rs = js_get(ctx, js_obj_value(p), "@r");
  if (rs.type == JV_OBJ) {
    unsigned i;
    for (i = 0; i < rs.u.obj->elen; i++)
      run_reaction(ctx, state, value, rs.u.obj->elems[i]);
  }
  js_def(p, "@r", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
}
static js_value prom_res_fn(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value pv = ctx->native_data;
  (void)t;
  if (pv.type == JV_OBJ)
    prom_resolve(ctx, pv.u.obj, ARG(0));
  return js_undef();
}
static js_value prom_rej_fn(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value pv = ctx->native_data;
  (void)t;
  if (pv.type == JV_OBJ)
    prom_settle(ctx, pv.u.obj, 2, ARG(0));
  return js_undef();
}
static void prom_resolve(js_ctx *ctx, js_object *p, js_value v)
{
  if (v.type == JV_OBJ && v.u.obj == p) {
    prom_settle(ctx, p, 2, js_make_error(ctx, ctx->type_error_proto, "TypeError", "chaining cycle"));
    return;
  }
  if (is_promise(v)) {
    js_object *vp = v.u.obj;
    int st = prom_state(vp);
    if (st == 1)
      prom_resolve(ctx, p, prom_value(vp));
    else if (st == 2)
      prom_settle(ctx, p, 2, prom_value(vp));
    else {
      js_value a[2];
      a[0] = js_obj_value(resolver(ctx, prom_res_fn, p));
      a[1] = js_obj_value(resolver(ctx, prom_rej_fn, p));
      promise_then(ctx, v, a, 2);
    }
    return;
  }
  if (v.type == JV_OBJ) {
    js_value then = js_get(ctx, v, "then");
    if (js_is_callable(then)) {
      js_value a[2];
      gc_push_scope();
      gc_keep(p);
      KEEP(v);
      a[0] = js_obj_value(resolver(ctx, prom_res_fn, p));
      a[1] = js_obj_value(resolver(ctx, prom_rej_fn, p));
      js_call(ctx, then, v, a, 2);
      if (ctx->has_exc) {
        js_value e = ctx->exc;
        ctx->has_exc = 0;
        prom_settle(ctx, p, 2, e);
      }
      gc_pop_scope();
      return;
    }
  }
  prom_settle(ctx, p, 1, v);
}

static js_value promise_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value exec = ARG(0);
  js_object *p;
  (void)t;
  if (!js_is_callable(exec))
    return js_throw_type(ctx, "Promise resolver is not a function");
  gc_push_scope();
  p = (js_object *)gc_keep(new_promise(ctx));
  {
    js_value ea[2];
    ea[0] = js_obj_value(resolver(ctx, prom_res_fn, p));
    ea[1] = js_obj_value(resolver(ctx, prom_rej_fn, p));
    js_call(ctx, exec, js_undef(), ea, 2);
    if (ctx->has_exc) {
      js_value e = ctx->exc;
      ctx->has_exc = 0;
      prom_settle(ctx, p, 2, e);
    }
  }
  gc_pop_scope();
  return js_obj_value(p);
}
static js_value promise_then(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  js_object *p, *child;
  js_value onF = ARG(0), onR = ARG(1);
  int st;
  if (!is_promise(thisv))
    return js_throw_type(ctx, "Promise.prototype.then on non-promise");
  p = thisv.u.obj;
  gc_push_scope();
  gc_keep(p);
  KEEP(onF);
  KEEP(onR);
  child = (js_object *)gc_keep(new_promise(ctx));
  {
    js_object *cres = resolver(ctx, prom_res_fn, child), *crej = resolver(ctx, prom_rej_fn, child);
    js_object *reaction = js_new_array(ctx, 0);
    gc_keep(reaction);
    js_array_push(ctx, reaction, onF);
    js_array_push(ctx, reaction, onR);
    js_array_push(ctx, reaction, js_obj_value(cres));
    js_array_push(ctx, reaction, js_obj_value(crej));
    st = prom_state(p);
    if (st == 0) {
      js_value rs = js_get(ctx, thisv, "@r");
      if (rs.type == JV_OBJ)
        js_array_push(ctx, rs.u.obj, js_obj_value(reaction));
    } else
      run_reaction(ctx, st, prom_value(p), js_obj_value(reaction));
  }
  gc_pop_scope();
  return js_obj_value(child);
}
static js_value promise_catch(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  js_value ar[2];
  ar[0] = js_undef();
  ar[1] = ARG(0);
  return promise_then(ctx, thisv, ar, 2);
}
static js_value promise_finally(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  /* simplified: run cb on settle, pass the value/reason through */
  js_value cb = ARG(0);
  if (js_is_callable(cb) && is_promise(thisv)) {
    int st = prom_state(thisv.u.obj);
    if (st != 0)
      js_call(ctx, cb, js_undef(), NULL, 0);
  }
  return thisv;
}
static js_value promise_resolve_s(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value v = ARG(0);
  js_object *p;
  (void)t;
  if (is_promise(v))
    return v;
  gc_push_scope();
  p = (js_object *)gc_keep(new_promise(ctx));
  prom_resolve(ctx, p, v);
  gc_pop_scope();
  return js_obj_value(p);
}
static js_value promise_reject_s(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *p;
  (void)t;
  gc_push_scope();
  p = (js_object *)gc_keep(new_promise(ctx));
  prom_settle(ctx, p, 2, ARG(0));
  gc_pop_scope();
  return js_obj_value(p);
}
static js_value promise_all_s(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value it = ARG(0);
  js_object *result, *vals;
  int rejected = 0;
  (void)t;
  gc_push_scope();
  result = (js_object *)gc_keep(new_promise(ctx));
  vals = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (it.type == JV_OBJ && it.u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    for (i = 0; i < it.u.obj->elen; i++) {
      js_value e = it.u.obj->elems[i];
      if (is_promise(e)) {
        int st = prom_state(e.u.obj);
        if (st == 2) {
          prom_settle(ctx, result, 2, prom_value(e.u.obj));
          rejected = 1;
          break;
        }
        js_array_push(ctx, vals, st == 1 ? prom_value(e.u.obj) : js_undef());
      } else
        js_array_push(ctx, vals, e);
    }
  }
  if (!rejected)
    prom_settle(ctx, result, 1, js_obj_value(vals));
  gc_pop_scope();
  return js_obj_value(result);
}
static js_value promise_allsettled_s(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value it = ARG(0);
  js_object *result, *vals;
  unsigned char pf = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  (void)t;
  (void)n;
  gc_push_scope();
  result = (js_object *)gc_keep(new_promise(ctx));
  vals = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (it.type == JV_OBJ && it.u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    for (i = 0; i < it.u.obj->elen; i++) {
      js_value e = it.u.obj->elems[i];
      js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
      if (is_promise(e) && prom_state(e.u.obj) == 2) {
        js_def(o, "status", js_mk_cstring(ctx, "rejected"), pf);
        js_def(o, "reason", prom_value(e.u.obj), pf);
      } else {
        js_def(o, "status", js_mk_cstring(ctx, "fulfilled"), pf);
        js_def(o, "value", is_promise(e) ? (prom_state(e.u.obj) == 1 ? prom_value(e.u.obj) : js_undef()) : e, pf);
      }
      js_array_push(ctx, vals, js_obj_value(o));
    }
  }
  prom_settle(ctx, result, 1, js_obj_value(vals));
  gc_pop_scope();
  return js_obj_value(result);
}
static js_value promise_any_s(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value it = ARG(0);
  js_object *result;
  int found = 0;
  (void)t;
  (void)n;
  gc_push_scope();
  result = (js_object *)gc_keep(new_promise(ctx));
  if (it.type == JV_OBJ && it.u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    for (i = 0; i < it.u.obj->elen; i++) {
      js_value e = it.u.obj->elems[i];
      if (is_promise(e)) {
        if (prom_state(e.u.obj) == 1) {
          prom_settle(ctx, result, 1, prom_value(e.u.obj));
          found = 1;
          break;
        }
      } else {
        prom_settle(ctx, result, 1, e);
        found = 1;
        break;
      }
    }
  }
  if (!found)
    prom_settle(ctx, result, 2,
                js_make_error(ctx, ctx->type_error_proto, "AggregateError", "All promises were rejected"));
  gc_pop_scope();
  return js_obj_value(result);
}

js_value js_promise_resolved(js_ctx *ctx, js_value v) { return promise_resolve_s(ctx, js_undef(), &v, 1); }
js_value js_promise_rejected(js_ctx *ctx, js_value v) { return promise_reject_s(ctx, js_undef(), &v, 1); }
int js_promise_inspect(js_ctx *ctx, js_value v, js_value *out)
{
  (void)ctx;
  if (!is_promise(v))
    return -1;
  if (out)
    *out = prom_value(v.u.obj);
  return prom_state(v.u.obj);
}

/* ------------------------------------------------------- Symbol ----------- */

static js_value symbol_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *desc = 0;
  char buf[160];
  (void)t;
  if (n >= 1 && !nullish(ARG(0))) {
    js_value d = js_to_string(ctx, a[0]);
    int l;
    if (ctx->has_exc)
      return js_undef();
    l = d.u.str->len;
    if (l > 158)
      l = 158;
    memcpy(buf, d.u.str->data, (unsigned)l);
    buf[l] = 0;
    desc = buf;
  }
  return js_mk_symbol(ctx, desc);
}
static js_value symbol_for(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value key = js_to_string(ctx, ARG(0)), reg, existing;
  (void)t;
  (void)n;
  if (!ctx->sym_for_reg)
    return js_undef();
  reg = js_obj_value(ctx->sym_for_reg); /* the shared global registry (not `this`) */
  existing = js_get(ctx, reg, key.u.str->data);
  if (existing.type == JV_SYMBOL)
    return existing;
  gc_push_scope();
  KEEP(key);
  {
    js_value sym = js_mk_symbol(ctx, key.u.str->data);
    js_set(ctx, reg, key.u.str->data, sym);
    gc_pop_scope();
    return sym;
  }
}
/* Symbol.keyFor: the registry key for a symbol made by Symbol.for, else
 * undefined. */
static js_value symbol_keyfor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = ARG(0);
  js_object *reg = ctx->sym_for_reg;
  int i;
  (void)t;
  (void)n;
  if (s.type != JV_SYMBOL)
    return js_throw_type(ctx, "Symbol.keyFor requires a symbol");
  if (!reg)
    return js_undef();
  for (i = 0; i < reg->props.n; i++)
    if (reg->props.e[i].value.type == JV_SYMBOL && reg->props.e[i].value.u.obj == s.u.obj)
      return js_mk_cstring(ctx, reg->props.e[i].key);
  return js_undef();
}
/* Accept either a primitive Symbol or a boxed one (Object(sym), an OBJ_SYMBOL
 * exposed as JV_OBJ), so Symbol.prototype methods work on both receivers. */
static int symbol_this(js_value t, js_object **o)
{
  if (t.type == JV_SYMBOL || (t.type == JV_OBJ && t.u.obj->kind == OBJ_SYMBOL)) {
    *o = t.u.obj;
    return 1;
  }
  return 0;
}
static js_value symbol_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value d;
  sbuf sb;
  js_object *so;
  (void)a;
  (void)n;
  if (!symbol_this(t, &so))
    return js_mk_cstring(ctx, "Symbol()");
  d = js_get(ctx, js_obj_value(so), "@desc");
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  sb_puts(&sb, "Symbol(");
  if (d.type == JV_STR)
    sb_put(&sb, d.u.str->data, d.u.str->len);
  sb_putc(&sb, ')');
  {
    js_value r = str_of(ctx, sb.b ? sb.b : "Symbol()", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
}
static js_value symbol_desc_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *so;
  (void)a;
  (void)n;
  if (!symbol_this(t, &so))
    return js_undef();
  return js_get(ctx, js_obj_value(so), "@desc");
}
static js_value symbol_valueof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *so;
  (void)ctx;
  (void)a;
  (void)n;
  if (symbol_this(t, &so)) {
    js_value v;
    v.type = JV_SYMBOL;
    v.u.obj = so;
    return v;
  }
  return t;
}

/* ------------------------------------------------------- Map / Set -------- */

static void arr_remove(js_object *arr, int i)
{
  unsigned j;
  for (j = (unsigned)i; j + 1 < arr->elen; j++)
    arr->elems[j] = arr->elems[j + 1];
  if (arr->elen)
    arr->elen--;
}
static int coll_find(js_object *keys, js_value k)
{
  unsigned i;
  for (i = 0; i < keys->elen; i++)
    if (js_same_value_zero(keys->elems[i], k))
      return (int)i;
  return -1;
}
static js_object *coll_keys(js_ctx *ctx, js_value t)
{
  js_value v = js_get(ctx, t, "@k");
  return v.type == JV_OBJ ? v.u.obj : 0;
}
static js_object *coll_vals(js_ctx *ctx, js_value t)
{
  js_value v = js_get(ctx, t, "@v");
  return v.type == JV_OBJ ? v.u.obj : 0;
}

/* iterator object: {@t, @i, @kind} + next() + [Symbol.iterator]->self */
static js_value coll_iter_next(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tgt = js_get(ctx, t, "@t");
  int i = js_to_int_sat(ctx, js_get(ctx, t, "@i"));
  int kind = js_to_int_sat(ctx, js_get(ctx, t, "@i2"));
  js_object *keys =
      (tgt.type == JV_OBJ && tgt.u.obj->kind == OBJ_ARRAY) ? tgt.u.obj : (tgt.type == JV_OBJ ? coll_keys(ctx, tgt) : 0);
  js_object *res;
  (void)a;
  (void)n;
  gc_push_scope();
  res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (!keys || i >= (int)keys->elen) {
    js_def(res, "value", js_undef(), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(res, "done", js_bool(1), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
    return js_obj_value(res);
  }
  js_set(ctx, t, "@i", js_number(i + 1));
  {
    js_value val;
    if (kind == 0)
      val = keys->elems[i]; /* keys, or set values */
    else if (kind == 1) {
      js_object *vv = coll_vals(ctx, tgt);
      val = vv ? vv->elems[i] : js_undef();
    } else { /* entries [k,v] */
      js_object *vv = coll_vals(ctx, tgt), *pair = js_new_array(ctx, 0);
      gc_keep(pair);
      js_array_push(ctx, pair, keys->elems[i]);
      js_array_push(ctx, pair, vv ? vv->elems[i] : keys->elems[i]);
      val = js_obj_value(pair);
    }
    js_def(res, "value", val, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(res, "done", js_bool(0), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  return js_obj_value(res);
}
static js_value iter_self(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)a;
  (void)n;
  return t;
}
static js_value make_coll_iter(js_ctx *ctx, js_value target, int kind)
{
  js_object *it;
  gc_push_scope();
  it = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  KEEP(target);
  js_def(it, "@t", target, P_WRITABLE);
  js_def(it, "@i", js_number(0), P_WRITABLE);
  js_def(it, "@i2", js_number(kind), P_WRITABLE);
  js_def_native(ctx, it, "next", coll_iter_next, 0);
  {
    js_object *f = js_new_native(ctx, iter_self, "", 0);
    js_def(it, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  gc_pop_scope();
  return js_obj_value(it);
}

static js_object *make_coll(js_ctx *ctx, js_object *proto, int with_values)
{
  js_object *o;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, proto));
  js_def(o, "@k", js_obj_value(js_new_array(ctx, 0)), 0);
  if (with_values)
    js_def(o, "@v", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
  return o;
}

static js_value map_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t), *vv = coll_vals(ctx, t);
  js_value k = ARG(0), v = ARG(1);
  int i;
  if (!kk || !vv)
    return js_throw_type(ctx, "Map.prototype.set on non-map");
  i = coll_find(kk, k);
  if (i >= 0)
    vv->elems[i] = v;
  else {
    gc_push_scope();
    KEEP(t);
    KEEP(k);
    KEEP(v);
    js_array_push(ctx, kk, k);
    js_array_push(ctx, vv, v);
    gc_pop_scope();
  }
  return t;
}
static js_value map_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t), *vv = coll_vals(ctx, t);
  int i;
  (void)n;
  if (!kk || !vv)
    return js_undef();
  i = coll_find(kk, ARG(0));
  return i >= 0 ? vv->elems[i] : js_undef();
}
static js_value coll_has(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t);
  (void)n;
  return js_bool(kk && coll_find(kk, ARG(0)) >= 0);
}
static js_value map_delete(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t), *vv = coll_vals(ctx, t);
  int i;
  (void)n;
  if (!kk)
    return js_bool(0);
  i = coll_find(kk, ARG(0));
  if (i < 0)
    return js_bool(0);
  arr_remove(kk, i);
  if (vv)
    arr_remove(vv, i);
  return js_bool(1);
}
static js_value coll_clear(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t), *vv = coll_vals(ctx, t);
  (void)a;
  (void)n;
  if (kk)
    kk->elen = 0;
  if (vv)
    vv->elen = 0;
  return js_undef();
}
static js_value coll_size(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t);
  (void)a;
  (void)n;
  return js_number(kk ? kk->elen : 0);
}
static js_value map_foreach(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t), *vv = coll_vals(ctx, t);
  js_value cb = ARG(0), self = ARG(1);
  unsigned i;
  (void)n;
  if (!kk || !js_is_callable(cb))
    return js_undef();
  gc_push_scope();
  KEEP(t);
  KEEP(cb);
  KEEP(self);
  for (i = 0; i < kk->elen && !ctx->has_exc; i++) {
    js_value cargs[3];
    cargs[0] = vv ? vv->elems[i] : kk->elems[i];
    cargs[1] = kk->elems[i];
    cargs[2] = t;
    js_call(ctx, cb, self, cargs, 3);
  }
  gc_pop_scope();
  return js_undef();
}
static js_value map_keys(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_coll_iter(ctx, t, 0);
}
static js_value map_values(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_coll_iter(ctx, t, 1);
}
static js_value map_entries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_coll_iter(ctx, t, 2);
}
static js_value set_values(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_coll_iter(ctx, t, 0);
}
static js_value set_entries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return make_coll_iter(ctx, t, 3);
}

static js_value set_add(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t);
  js_value v = ARG(0);
  (void)n;
  if (!kk)
    return js_throw_type(ctx, "Set.prototype.add on non-set");
  if (coll_find(kk, v) < 0) {
    gc_push_scope();
    KEEP(t);
    KEEP(v);
    js_array_push(ctx, kk, v);
    gc_pop_scope();
  }
  return t;
}
static js_value set_delete(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t);
  int i;
  (void)n;
  if (!kk)
    return js_bool(0);
  i = coll_find(kk, ARG(0));
  if (i < 0)
    return js_bool(0);
  arr_remove(kk, i);
  return js_bool(1);
}
static js_value set_foreach(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *kk = coll_keys(ctx, t);
  js_value cb = ARG(0), self = ARG(1);
  unsigned i;
  (void)n;
  if (!kk || !js_is_callable(cb))
    return js_undef();
  gc_push_scope();
  KEEP(t);
  KEEP(cb);
  KEEP(self);
  for (i = 0; i < kk->elen && !ctx->has_exc; i++) {
    js_value cargs[3];
    cargs[0] = kk->elems[i];
    cargs[1] = kk->elems[i];
    cargs[2] = t;
    js_call(ctx, cb, self, cargs, 3);
  }
  gc_pop_scope();
  return js_undef();
}

static js_value map_constructor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *m;
  (void)t;
  gc_push_scope();
  m = (js_object *)gc_keep(make_coll(ctx, ctx->map_proto, 1));
  if (n >= 1 && ARG(0).type == JV_OBJ && ARG(0).u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    js_object *src = ARG(0).u.obj;
    for (i = 0; i < src->elen && !ctx->has_exc; i++) {
      js_value pair = src->elems[i], cargs[2];
      cargs[0] = js_get_index(ctx, pair, 0);
      cargs[1] = js_get_index(ctx, pair, 1);
      map_set(ctx, js_obj_value(m), cargs, 2);
    }
  }
  gc_pop_scope();
  return js_obj_value(m);
}
static js_value set_constructor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *s;
  (void)t;
  gc_push_scope();
  s = (js_object *)gc_keep(make_coll(ctx, ctx->set_proto, 0));
  if (n >= 1 && ARG(0).type == JV_OBJ && ARG(0).u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    js_object *src = ARG(0).u.obj;
    for (i = 0; i < src->elen && !ctx->has_exc; i++) {
      js_value e = src->elems[i];
      set_add(ctx, js_obj_value(s), &e, 1);
    }
  }
  gc_pop_scope();
  return js_obj_value(s);
}
static js_value weakmap_constructor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_obj_value(make_coll(ctx, ctx->weakmap_proto, 1));
}
static js_value weakset_constructor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_obj_value(make_coll(ctx, ctx->weakset_proto, 0));
}
static js_value weakmap_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  if (ARG(0).type != JV_OBJ && ARG(0).type != JV_SYMBOL)
    return js_throw_type(ctx, "Invalid value used as weak map key");
  return map_set(ctx, t, a, n);
}
static js_value weakset_add(js_ctx *ctx, js_value t, js_value *a, int n)
{
  if (ARG(0).type != JV_OBJ && ARG(0).type != JV_SYMBOL)
    return js_throw_type(ctx, "Invalid value used in weak set");
  return set_add(ctx, t, a, n);
}

/* ------------------------------------------------------- setup ------------ */

/* ------------------------------------------------------- Date -------------- *
 * Time is epoch-millis in a hidden @t property; the wall clock comes from the
 * embedder's ctx->now_ms hook. UTC only (no timezone — getUTC* alias get*). The
 * days<->civil conversions are Howard Hinnant's branch-free algorithms. */
static double date_t(js_ctx *ctx, js_value t)
{
  js_value v = js_get(ctx, t, "@t");
  return v.type == JV_NUM ? v.u.num : 0;
}
static void date_civil(double ms, int *Y, int *Mo, int *D, int *h, int *mi, int *s, int *mil, int *dow)
{
  long long t;
  long long secs;
  if (!(ms > -8.64e15 && ms < 8.64e15))
    ms = 0;
  t = (long long)ms;
  secs = t / 1000;
  int millis = (int)(t - secs * 1000);
  if (millis < 0) {
    millis += 1000;
    secs -= 1;
  }
  long long days = secs / 86400;
  int rem = (int)(secs - days * 86400);
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  *h = rem / 3600;
  *mi = (rem % 3600) / 60;
  *s = rem % 60;
  *mil = millis;
  {
    int w = (int)(((days % 7) + 4) % 7);
    if (w < 0)
      w += 7;
    *dow = w;
  }
  days += 719468;
  long long era = (days >= 0 ? days : days - 146096) / 146097;
  long long doe = days - era * 146097;
  long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long y = yoe + era * 400;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long long mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1);
  int m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y += (m <= 2);
  *Y = (int)y;
  *Mo = m - 1;
  *D = d;
}
static double date_from_civil(int Y, int Mo, int D, int h, int mi, int s, int ms)
{
  int m = Mo + 1;
  long long yy = Y - (m <= 2);
  long long era = (yy >= 0 ? yy : yy - 399) / 400;
  long long yoe = yy - era * 400;
  long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + D - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = era * 146097 + doe - 719468;
  return (double)(days * 86400LL + (long long)h * 3600 + mi * 60 + s) * 1000.0 + ms;
}
static int date_pad(char *out, int v, int w)
{
  int i;
  for (i = w - 1; i >= 0; i--) {
    out[i] = (char)('0' + v % 10);
    v /= 10;
  }
  return w;
}

static js_value date_now(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_number(ctx->now_ms ? (double)ctx->now_ms() : 0);
}
static js_value date_ctor(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  double ms;
  js_object *o;
  (void)thisv;
  if (n == 0)
    ms = ctx->now_ms ? (double)ctx->now_ms() : 0;
  else if (n == 1)
    ms = js_to_number(ctx, a[0]); /* new Date(ms) */
  else {                          /* new Date(y, mo, d, ...) — UTC */
    int y = js_to_int_sat(ctx, a[0]);
    int mo = js_to_int_sat(ctx, a[1]);
    int d = n > 2 ? js_to_int_sat(ctx, a[2]) : 1;
    int h = n > 3 ? js_to_int_sat(ctx, a[3]) : 0;
    int mi = n > 4 ? js_to_int_sat(ctx, a[4]) : 0;
    int s = n > 5 ? js_to_int_sat(ctx, a[5]) : 0;
    int milc = n > 6 ? js_to_int_sat(ctx, a[6]) : 0;
    ms = date_from_civil(y, mo, d, h, mi, s, milc);
  }
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->date_proto));
  js_def(o, "@t", js_number(ms), 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value date_getTime(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return js_number(date_t(ctx, t));
}
#define DATE_GET(fn, idx)                                                                                              \
  static js_value fn(js_ctx *ctx, js_value t, js_value *a, int n)                                                      \
  {                                                                                                                    \
    int c[8];                                                                                                          \
    (void)a;                                                                                                           \
    (void)n;                                                                                                           \
    date_civil(date_t(ctx, t), &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]);                                \
    return js_number(c[idx]);                                                                                          \
  }
DATE_GET(date_getFullYear, 0)
DATE_GET(date_getMonth, 1)
DATE_GET(date_getDate, 2)
DATE_GET(date_getHours, 3)
DATE_GET(date_getMinutes, 4)
DATE_GET(date_getSeconds, 5)
DATE_GET(date_getMilliseconds, 6)
DATE_GET(date_getDay, 7)
#undef DATE_GET
static js_value date_tzoff(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)a;
  (void)n;
  return js_number(0);
}
static js_value date_iso(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int Y, Mo, D, h, mi, s, ms, dw;
  char b[40];
  int i = 0;
  (void)a;
  (void)n;
  date_civil(date_t(ctx, t), &Y, &Mo, &D, &h, &mi, &s, &ms, &dw);
  i += date_pad(b + i, Y, 4);
  b[i++] = '-';
  i += date_pad(b + i, Mo + 1, 2);
  b[i++] = '-';
  i += date_pad(b + i, D, 2);
  b[i++] = 'T';
  i += date_pad(b + i, h, 2);
  b[i++] = ':';
  i += date_pad(b + i, mi, 2);
  b[i++] = ':';
  i += date_pad(b + i, s, 2);
  b[i++] = '.';
  i += date_pad(b + i, ms, 3);
  b[i++] = 'Z';
  b[i] = '\0';
  return js_mk_string(ctx, b, i);
}

/* ------------------------------------------------------- base64 ----------- *
 * btoa/atob over the shared base64 lib. Operate on the string's raw bytes (a
 * true "binary string"): ASCII round-trips exactly; bytes 128–255 ride through
 * as their stored UTF-8 (we don't enforce the latin1 codepoint range). atob
 * throws on an invalid character (spec forgiving-base64), tolerating embedded
 * whitespace. */
static js_value g_btoa(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  const unsigned char *d;
  int len, cap;
  char *buf;
  js_value r;
  (void)t;
  (void)n;
  gc_push_scope();
  KEEP(s);
  d = (const unsigned char *)s.u.str->data;
  len = s.u.str->len;
  cap = base64_encoded_len(len);
  buf = malloc((unsigned)cap + 1);
  if (!buf) {
    gc_pop_scope();
    return js_throw_type(ctx, "btoa: out of memory");
  }
  base64_encode(d, len, buf);
  gc_pop_scope();
  r = str_of(ctx, buf, cap);
  free(buf);
  return r;
}
static js_value g_atob(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value s = js_to_string(ctx, ARG(0));
  const char *d;
  int len, cap, m;
  unsigned char *buf;
  js_value r;
  (void)t;
  (void)n;
  gc_push_scope();
  KEEP(s);
  d = s.u.str->data;
  len = s.u.str->len;
  cap = base64_decoded_cap(len);
  buf = malloc((unsigned)(cap ? cap : 1));
  if (!buf) {
    gc_pop_scope();
    return js_throw_type(ctx, "atob: out of memory");
  }
  m = base64_decode(d, len, buf);
  gc_pop_scope();
  if (m < 0) {
    free(buf);
    return js_throw_type(ctx, "atob: invalid base64");
  }
  r = str_of(ctx, (const char *)buf, m);
  free(buf);
  return r;
}

/* ------------------------------------------------------- Function --------- *
 * call/apply invoke `this` (the target function) with an explicit receiver;
 * bind returns a trampoline that prepends a fixed receiver + partial args. The
 * trampoline carries its binding in the native's fn->data as the array
 * [target, boundThis, ...boundArgs], read back via ctx->native_data. */
static js_value fn_call(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value thisarg = n > 0 ? a[0] : js_undef();
  return js_call(ctx, t, thisarg, n > 0 ? a + 1 : a, n > 0 ? n - 1 : 0);
}
static js_value fn_apply(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value thisarg = n > 0 ? a[0] : js_undef();
  js_value arr = n > 1 ? a[1] : js_undef();
  js_value *argv, r;
  int len, i;
  if (arr.type == JV_UNDEF || arr.type == JV_NULL)
    return js_call(ctx, t, thisarg, 0, 0);
  len = arr.type == JV_OBJ && arr.u.obj->kind == OBJ_ARRAY ? (int)arr.u.obj->elen
                                                           : (int)js_to_uint32(ctx, js_get(ctx, arr, "length"));
  if (len < 0)
    len = 0;
  argv = len ? (js_value *)malloc((unsigned)len * sizeof *argv) : 0;
  if (len && !argv)
    return js_throw_range(ctx, "apply: out of memory");
  gc_push_scope();
  KEEP(t);
  KEEP(thisarg);
  KEEP(arr);
  for (i = 0; i < len; i++) {
    argv[i] = js_get_index(ctx, arr, (unsigned)i);
    KEEP(argv[i]);
  }
  r = js_call(ctx, t, thisarg, argv, len);
  gc_pop_scope();
  if (argv)
    free(argv);
  return r;
}
/* the bound trampoline: data = [target, boundThis, ...boundArgs] */
static js_value fn_bound_call(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value rec = ctx->native_data, target, boundThis, *argv, r;
  int nbound, total, i;
  (void)t;
  if (rec.type != JV_OBJ || rec.u.obj->kind != OBJ_ARRAY)
    return js_undef();
  target = js_get_index(ctx, rec, 0);
  boundThis = js_get_index(ctx, rec, 1);
  nbound = (int)rec.u.obj->elen - 2;
  if (nbound < 0)
    nbound = 0;
  total = nbound + n;
  argv = total ? (js_value *)malloc((unsigned)total * sizeof *argv) : 0;
  if (total && !argv)
    return js_throw_range(ctx, "bound call: out of memory");
  gc_push_scope();
  KEEP(rec);
  KEEP(target);
  KEEP(boundThis);
  for (i = 0; i < nbound; i++) {
    argv[i] = js_get_index(ctx, rec, (unsigned)(2 + i));
    KEEP(argv[i]);
  }
  for (i = 0; i < n; i++) {
    argv[nbound + i] = a[i];
    KEEP(a[i]);
  }
  r = js_call(ctx, target, boundThis, argv, total);
  gc_pop_scope();
  if (argv)
    free(argv);
  return r;
}
static js_value fn_bind(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *rec, *bf;
  js_value rv;
  int i;
  gc_push_scope();
  KEEP(t);
  rec = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, rec, t);
  js_array_push(ctx, rec, n > 0 ? a[0] : js_undef());
  for (i = 1; i < n; i++)
    js_array_push(ctx, rec, a[i]);
  bf = (js_object *)gc_keep(js_new_native(ctx, fn_bound_call, "bound", 0));
  bf->fn->data = js_obj_value(rec);
  gc_write_barrier(bf);
  js_def(bf, "@bound", js_obj_value(rec), 0); /* lets `new boundFn()` unwrap to the target */
  rv = js_obj_value(bf);
  gc_pop_scope();
  return rv;
}

/* ------------------------------------------------------- typed arrays ----- *
 * ArrayBuffer + the nine TypedArray views + DataView. A view object (kind
 * OBJ_TYPEDARRAY) carries a js_typedarray host {buffer, type, offset, length};
 * element get/set goes through ta_get/ta_set (value.c) so `view[i]` hits the
 * backing bytes. The buffer's bytes are freed by the GC finalizer; the view
 * keeps its ArrayBuffer alive via the tracer. */
static js_object *make_ctor(js_ctx *ctx, const char *name, js_native fn, js_object *proto); /* fwd */

/* a * b clamped to INT_MAX; the allocation then fails instead of overflowing */
static int mul_sat(int a, int b)
{
  if (a < 0 || b < 0)
    return 0;
  return b && a > 2147483647 / b ? 2147483647 : a * b;
}
static js_object *new_arraybuffer(js_ctx *ctx, int n)
{
  js_object *o;
  js_arraybuffer *ab;
  if (n < 0)
    n = 0;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->arraybuffer_proto));
  o->kind = OBJ_ARRAYBUFFER;
  ab = (js_arraybuffer *)malloc(sizeof *ab);
  if (!ab)
    gc_oom();
  ab->data = (unsigned char *)calloc((unsigned)(n > 0 ? n : 1), 1);
  ab->byteLength = n;
  if (!ab->data) { /* too big for the heap: an empty buffer and a RangeError */
    ab->byteLength = 0;
    js_throw_range(ctx, "Array buffer allocation failed");
  }
  o->host = ab;
  gc_pop_scope();
  return o;
}
static js_value new_view(js_ctx *ctx, int type, js_object *buf, int offset, int length)
{
  js_object *o, *proto = type >= 0 ? ctx->typed_protos[type] : ctx->dataview_proto;
  js_typedarray *ta;
  js_value v;
  gc_push_scope();
  gc_keep(buf);
  o = (js_object *)gc_keep(js_new_object(ctx, proto));
  o->kind = OBJ_TYPEDARRAY;
  ta = (js_typedarray *)malloc(sizeof *ta);
  if (!ta)
    gc_oom();
  {
    js_arraybuffer *ab = (js_arraybuffer *)buf->host;
    int es = type >= 0 ? ta_elem_size(type) : 1, bl = ab ? ab->byteLength : 0;
    if (offset < 0 || offset > bl)
      offset = bl;
    if (length < 0 || length > (bl - offset) / es)
      length = (bl - offset) / es;
  }
  ta->buffer = buf;
  ta->type = type;
  ta->offset = offset;
  ta->length = length;
  o->host = ta;
  v = js_obj_value(o);
  gc_pop_scope();
  return v;
}
js_value js_new_typedarray(js_ctx *ctx, int type, int length)
{
  js_object *buf;
  js_value v;
  gc_push_scope();
  buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(length, ta_elem_size(type))));
  v = new_view(ctx, type, buf, 0, length);
  gc_pop_scope();
  return v;
}
static js_typedarray *as_ta(js_value t)
{
  return (t.type == JV_OBJ && t.u.obj->kind == OBJ_TYPEDARRAY) ? (js_typedarray *)t.u.obj->host : 0;
}
static int clamp_idx(int i, int len)
{
  if (i < 0)
    i += len;
  if (i < 0)
    i = 0;
  if (i > len)
    i = len;
  return i;
}

/* length of an array-like / typed-array source (for the copy constructors) */
static int ta_src_len(js_ctx *ctx, js_value src)
{
  if (src.type == JV_OBJ) {
    js_object *o = src.u.obj;
    if (o->kind == OBJ_TYPEDARRAY) {
      js_typedarray *t = (js_typedarray *)o->host;
      return t ? t->length : 0;
    }
    if (o->kind == OBJ_ARRAY)
      return (int)o->elen;
  }
  return (int)js_to_uint32(ctx, js_get(ctx, src, "length"));
}

/* new TypedArray(len) | (buffer[,off[,len]]) | (typedArray|arrayLike) */
static js_value ta_make(js_ctx *ctx, int type, js_value *a, int n)
{
  int es = ta_elem_size(type);
  js_value arg = n > 0 ? a[0] : js_undef();
  if (arg.type == JV_OBJ && arg.u.obj->kind == OBJ_ARRAYBUFFER) {
    js_arraybuffer *ab = (js_arraybuffer *)arg.u.obj->host;
    int off = (n > 1 && a[1].type != JV_UNDEF) ? js_to_int_sat(ctx, a[1]) : 0;
    int len;
    if (off < 0 || off > ab->byteLength || (off % es))
      return js_throw_range(ctx, "invalid typed array byte offset");
    if (n > 2 && a[2].type != JV_UNDEF) {
      len = js_to_int_sat(ctx, a[2]);
      if (len < 0 || off + len * es > ab->byteLength)
        return js_throw_range(ctx, "invalid typed array length");
    } else {
      if ((ab->byteLength - off) % es)
        return js_throw_range(ctx, "byte length not aligned");
      len = (ab->byteLength - off) / es;
    }
    return new_view(ctx, type, arg.u.obj, off, len);
  }
  if (arg.type == JV_OBJ) { /* typed array or array-like: copy */
    int len = ta_src_len(ctx, arg), i;
    js_value view;
    gc_push_scope();
    {
      js_object *buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(len, es)));
      view = new_view(ctx, type, buf, 0, len);
      if (view.type == JV_OBJ) {
        gc_keep(view.u.obj);
        for (i = 0; i < len; i++)
          ta_set(ctx, view.u.obj, (unsigned)i, js_get_index(ctx, arg, (unsigned)i));
      }
    }
    gc_pop_scope();
    return view;
  }
  {
    int len = arg.type == JV_UNDEF ? 0 : js_to_int_sat(ctx, arg);
    if (len < 0)
      return js_throw_range(ctx, "invalid typed array length");
    return new_view(ctx, type, new_arraybuffer(ctx, mul_sat(len, es)), 0, len);
  }
}
#define TA_CTOR(fn, TYPE)                                                                                              \
  static js_value fn(js_ctx *ctx, js_value t, js_value *a, int n)                                                      \
  {                                                                                                                    \
    (void)t;                                                                                                           \
    return ta_make(ctx, TYPE, a, n);                                                                                   \
  }
TA_CTOR(int8a_ctor, TA_I8)
TA_CTOR(uint8a_ctor, TA_U8)
TA_CTOR(uint8ca_ctor, TA_U8C)
TA_CTOR(int16a_ctor, TA_I16)
TA_CTOR(uint16a_ctor, TA_U16)
TA_CTOR(int32a_ctor, TA_I32)
TA_CTOR(uint32a_ctor, TA_U32)
TA_CTOR(float32a_ctor, TA_F32)
TA_CTOR(float64a_ctor, TA_F64)
#undef TA_CTOR

/* %TypedArray%.prototype accessors + methods (this = a view) */
static js_value tap_length(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  (void)ctx;
  (void)a;
  (void)n;
  return js_number(ta ? ta->length : 0);
}
static js_value tap_bytelength(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  (void)ctx;
  (void)a;
  (void)n;
  return js_number(ta ? ta->length * ta_elem_size(ta->type) : 0);
}
static js_value tap_byteoffset(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  (void)ctx;
  (void)a;
  (void)n;
  return js_number(ta ? ta->offset : 0);
}
static js_value tap_buffer(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  (void)ctx;
  (void)a;
  (void)n;
  return ta ? js_obj_value(ta->buffer) : js_undef();
}

static js_value tap_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value src = n > 0 ? a[0] : js_undef();
  int off = n > 1 ? js_to_int_sat(ctx, a[1]) : 0, len, i;
  if (!ta)
    return js_undef();
  len = ta_src_len(ctx, src);
  if (off < 0)
    off = 0;
  for (i = 0; i < len; i++)
    ta_set(ctx, t.u.obj, (unsigned)(off + i), js_get_index(ctx, src, (unsigned)i));
  return js_undef();
}
static js_value tap_subarray(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  int len, b, e;
  if (!ta)
    return js_undef();
  len = ta->length;
  b = (n > 0 && a[0].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[0]), len) : 0;
  e = (n > 1 && a[1].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[1]), len) : len;
  if (e < b)
    e = b;
  return new_view(ctx, ta->type, ta->buffer, ta->offset + b * ta_elem_size(ta->type), e - b);
}
static js_value tap_slice(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  int len, es, b, e, m, i;
  js_value view;
  if (!ta)
    return js_undef();
  len = ta->length;
  es = ta_elem_size(ta->type);
  b = (n > 0 && a[0].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[0]), len) : 0;
  e = (n > 1 && a[1].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[1]), len) : len;
  m = e - b;
  if (m < 0)
    m = 0;
  gc_push_scope();
  {
    js_object *buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(m, es)));
    view = new_view(ctx, ta->type, buf, 0, m);
    if (view.type == JV_OBJ) {
      gc_keep(view.u.obj);
      for (i = 0; i < m; i++)
        ta_set(ctx, view.u.obj, (unsigned)i, ta_get(t.u.obj, (unsigned)(b + i)));
    }
  }
  gc_pop_scope();
  return view;
}
static js_value tap_fill(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value v = n > 0 ? a[0] : js_undef();
  int len, s, e;
  if (!ta)
    return t;
  len = ta->length;
  s = (n > 1 && a[1].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[1]), len) : 0;
  e = (n > 2 && a[2].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[2]), len) : len;
  for (; s < e; s++)
    ta_set(ctx, t.u.obj, (unsigned)s, v);
  return t;
}
static js_value tap_indexof(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value v = n > 0 ? a[0] : js_undef();
  int i, from;
  if (!ta)
    return js_number(-1);
  from = n > 1 ? clamp_idx(js_to_int_sat(ctx, a[1]), ta->length) : 0;
  for (i = from; i < ta->length; i++)
    if (js_strict_eq(ta_get(t.u.obj, (unsigned)i), v))
      return js_number(i);
  return js_number(-1);
}
static js_value tap_includes(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value r = tap_indexof(ctx, t, a, n);
  return js_bool(r.u.num >= 0);
}
static js_value tap_join(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  sbuf sb;
  js_value r;
  int i;
  const char *sep = ",";
  int seplen = 1;
  js_value sv = js_undef();
  if (n > 0 && a[0].type != JV_UNDEF) {
    sv = js_to_string(ctx, a[0]);
    sep = sv.u.str->data;
    seplen = sv.u.str->len;
  }
  sb.b = NULL;
  sb.n = 0;
  sb.cap = 0;
  gc_push_scope();
  if (sv.type == JV_STR)
    KEEP(sv);
  if (ta)
    for (i = 0; i < ta->length; i++) {
      js_value e;
      if (i)
        sb_put(&sb, sep, seplen);
      e = js_to_string(ctx, ta_get(t.u.obj, (unsigned)i));
      if (e.type == JV_STR)
        sb_put(&sb, e.u.str->data, e.u.str->len);
    }
  gc_pop_scope();
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value tap_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return tap_join(ctx, t, 0, 0);
}
static js_value tap_foreach(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value cb = n > 0 ? a[0] : js_undef();
  int i;
  if (!ta || !js_is_callable(cb))
    return js_undef();
  for (i = 0; i < ta->length; i++) {
    js_value ar[3];
    ar[0] = ta_get(t.u.obj, (unsigned)i);
    ar[1] = js_number(i);
    ar[2] = t;
    js_call(ctx, cb, js_undef(), ar, 3);
    if (ctx->has_exc)
      break;
  }
  return js_undef();
}
static js_value tap_map(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value cb = n > 0 ? a[0] : js_undef(), view;
  int i;
  if (!ta)
    return js_undef();
  gc_push_scope();
  {
    js_object *buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(ta->length, ta_elem_size(ta->type))));
    view = new_view(ctx, ta->type, buf, 0, ta->length);
    if (view.type == JV_OBJ) {
      gc_keep(view.u.obj);
      for (i = 0; i < ta->length; i++) {
        js_value ar[3], r;
        ar[0] = ta_get(t.u.obj, (unsigned)i);
        ar[1] = js_number(i);
        ar[2] = t;
        r = js_is_callable(cb) ? js_call(ctx, cb, js_undef(), ar, 3) : ar[0];
        if (ctx->has_exc)
          break;
        ta_set(ctx, view.u.obj, (unsigned)i, r);
      }
    }
  }
  gc_pop_scope();
  return view;
}
static js_value tap_reduce(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  js_value cb = n > 0 ? a[0] : js_undef(), acc;
  int i = 0;
  if (!ta)
    return js_undef();
  if (n > 1)
    acc = a[1];
  else {
    if (ta->length == 0)
      return js_throw_type(ctx, "Reduce of empty array with no initial value");
    acc = ta_get(t.u.obj, 0);
    i = 1;
  }
  for (; i < ta->length; i++) {
    js_value ar[4];
    ar[0] = acc;
    ar[1] = ta_get(t.u.obj, (unsigned)i);
    ar[2] = js_number(i);
    ar[3] = t;
    acc = js_call(ctx, cb, js_undef(), ar, 4);
    if (ctx->has_exc)
      break;
  }
  return acc;
}
/* TypedArray.of / .from read their element kind from the native's bound data */
static js_value ta_of(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int type = (int)ctx->native_data.u.num, i;
  js_value view;
  (void)t;
  gc_push_scope();
  {
    js_object *buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(n, ta_elem_size(type))));
    view = new_view(ctx, type, buf, 0, n);
    if (view.type == JV_OBJ) {
      gc_keep(view.u.obj);
      for (i = 0; i < n; i++)
        ta_set(ctx, view.u.obj, (unsigned)i, a[i]);
    }
  }
  gc_pop_scope();
  return view;
}
static js_value ta_from(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int type = (int)ctx->native_data.u.num, i, len;
  js_value src = n > 0 ? a[0] : js_undef(), mapfn = n > 1 ? a[1] : js_undef(), view;
  (void)t;
  len = ta_src_len(ctx, src);
  gc_push_scope();
  {
    js_object *buf = (js_object *)gc_keep(new_arraybuffer(ctx, mul_sat(len, ta_elem_size(type))));
    view = new_view(ctx, type, buf, 0, len);
    if (view.type == JV_OBJ) {
      gc_keep(view.u.obj);
      for (i = 0; i < len; i++) {
        js_value e = js_get_index(ctx, src, (unsigned)i);
        if (js_is_callable(mapfn)) {
          js_value ar[2];
          ar[0] = e;
          ar[1] = js_number(i);
          e = js_call(ctx, mapfn, js_undef(), ar, 2);
          if (ctx->has_exc)
            break;
        }
        ta_set(ctx, view.u.obj, (unsigned)i, e);
      }
    }
  }
  gc_pop_scope();
  return view;
}

/* ArrayBuffer */
static js_value ab_bytelength(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)a;
  (void)n;
  if (t.type == JV_OBJ && t.u.obj->kind == OBJ_ARRAYBUFFER && t.u.obj->host)
    return js_number(((js_arraybuffer *)t.u.obj->host)->byteLength);
  return js_number(0);
}
static js_value ab_isview(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)n;
  return js_bool(n > 0 && a[0].type == JV_OBJ && a[0].u.obj->kind == OBJ_TYPEDARRAY);
}
static js_value ab_slice(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_arraybuffer *ab;
  int len, b, e, m;
  js_object *o;
  if (t.type != JV_OBJ || t.u.obj->kind != OBJ_ARRAYBUFFER)
    return js_undef();
  ab = (js_arraybuffer *)t.u.obj->host;
  len = ab->byteLength;
  b = (n > 0 && a[0].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[0]), len) : 0;
  e = (n > 1 && a[1].type != JV_UNDEF) ? clamp_idx(js_to_int_sat(ctx, a[1]), len) : len;
  m = e - b;
  if (m < 0)
    m = 0;
  o = new_arraybuffer(ctx, m);
  if (o->host)
    memcpy(((js_arraybuffer *)o->host)->data, ab->data + b, (unsigned)m);
  return js_obj_value(o);
}
static js_value arraybuffer_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  int len = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  (void)t;
  if (len < 0)
    return js_throw_range(ctx, "Invalid array buffer length");
  return js_obj_value(new_arraybuffer(ctx, len));
}

/* DataView: type TA_DATAVIEW, host->length = byteLength */
static void rev_bytes(unsigned char *b, int n)
{
  int i;
  for (i = 0; i < n / 2; i++) {
    unsigned char x = b[i];
    b[i] = b[n - 1 - i];
    b[n - 1 - i] = x;
  }
}
static js_value dv_decode(int type, unsigned char *p)
{ /* p is little-endian */
  switch (type) {
  case TA_I8:
    return js_number((double)*(signed char *)p);
  case TA_U8:
    return js_number((double)*p);
  case TA_I16: {
    short x;
    memcpy(&x, p, 2);
    return js_number((double)x);
  }
  case TA_U16: {
    unsigned short x;
    memcpy(&x, p, 2);
    return js_number((double)x);
  }
  case TA_I32: {
    int x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_U32: {
    unsigned x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_F32: {
    float x;
    memcpy(&x, p, 4);
    return js_number((double)x);
  }
  case TA_F64: {
    double x;
    memcpy(&x, p, 8);
    return js_number(x);
  }
  }
  return js_number(0);
}
static void dv_encode(js_ctx *ctx, int type, js_value v, unsigned char *p)
{
  if (type == TA_F32) {
    float f = (float)js_to_number(ctx, v);
    memcpy(p, &f, 4);
    return;
  }
  if (type == TA_F64) {
    double d = js_to_number(ctx, v);
    memcpy(p, &d, 8);
    return;
  }
  {
    unsigned u = js_to_uint32(ctx, v);
    memcpy(p, &u, (unsigned)ta_elem_size(type));
  }
}
static js_value dv_get(js_ctx *ctx, js_value t, js_value *a, int n, int type)
{
  js_typedarray *ta = as_ta(t);
  int es = ta_elem_size(type), off, le;
  js_arraybuffer *ab;
  unsigned char tmp[8];
  if (!ta || ta->type != TA_DATAVIEW)
    return js_undef();
  off = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  le = (es > 1) && (n > 1) && js_to_boolean(a[1]);
  if (off < 0 || off + es > ta->length)
    return js_throw_range(ctx, "Offset is outside the bounds of the DataView");
  ab = (js_arraybuffer *)ta->buffer->host;
  memcpy(tmp, ab->data + ta->offset + off, (unsigned)es);
  if (!le)
    rev_bytes(tmp, es); /* DataView defaults to big-endian */
  return dv_decode(type, tmp);
}
static js_value dv_set(js_ctx *ctx, js_value t, js_value *a, int n, int type)
{
  js_typedarray *ta = as_ta(t);
  int es = ta_elem_size(type), off, le;
  js_arraybuffer *ab;
  unsigned char tmp[8];
  if (!ta || ta->type != TA_DATAVIEW)
    return js_undef();
  off = n > 0 ? js_to_int_sat(ctx, a[0]) : 0;
  le = (es > 1) && (n > 2) && js_to_boolean(a[2]);
  if (off < 0 || off + es > ta->length)
    return js_throw_range(ctx, "Offset is outside the bounds of the DataView");
  ab = (js_arraybuffer *)ta->buffer->host;
  dv_encode(ctx, type, n > 1 ? a[1] : js_undef(), tmp);
  if (!le)
    rev_bytes(tmp, es);
  memcpy(ab->data + ta->offset + off, tmp, (unsigned)es);
  return js_undef();
}
#define DV_GET(fn, TYPE)                                                                                               \
  static js_value fn(js_ctx *ctx, js_value t, js_value *a, int n) { return dv_get(ctx, t, a, n, TYPE); }
#define DV_SET(fn, TYPE)                                                                                               \
  static js_value fn(js_ctx *ctx, js_value t, js_value *a, int n) { return dv_set(ctx, t, a, n, TYPE); }
DV_GET(dv_get_i8, TA_I8)
DV_SET(dv_set_i8, TA_I8)
DV_GET(dv_get_u8, TA_U8)
DV_SET(dv_set_u8, TA_U8)
DV_GET(dv_get_i16, TA_I16)
DV_SET(dv_set_i16, TA_I16)
DV_GET(dv_get_u16, TA_U16)
DV_SET(dv_set_u16, TA_U16)
DV_GET(dv_get_i32, TA_I32)
DV_SET(dv_set_i32, TA_I32)
DV_GET(dv_get_u32, TA_U32)
DV_SET(dv_set_u32, TA_U32)
DV_GET(dv_get_f32, TA_F32)
DV_SET(dv_set_f32, TA_F32)
DV_GET(dv_get_f64, TA_F64)
DV_SET(dv_set_f64, TA_F64)
#undef DV_GET
#undef DV_SET
static js_value dv_bytelength(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_typedarray *ta = as_ta(t);
  (void)ctx;
  (void)a;
  (void)n;
  return js_number(ta ? ta->length : 0);
}
static js_value dataview_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_arraybuffer *ab;
  int off, len;
  (void)t;
  if (!(n > 0 && a[0].type == JV_OBJ && a[0].u.obj->kind == OBJ_ARRAYBUFFER))
    return js_throw_type(ctx, "First argument to DataView constructor must be an ArrayBuffer");
  ab = (js_arraybuffer *)a[0].u.obj->host;
  off = (n > 1 && a[1].type != JV_UNDEF) ? js_to_int_sat(ctx, a[1]) : 0;
  if (off < 0 || off > ab->byteLength)
    return js_throw_range(ctx, "Invalid DataView offset");
  if (n > 2 && a[2].type != JV_UNDEF) {
    len = js_to_int_sat(ctx, a[2]);
    if (len < 0 || off + len > ab->byteLength)
      return js_throw_range(ctx, "Invalid DataView length");
  } else
    len = ab->byteLength - off;
  return new_view(ctx, TA_DATAVIEW, a[0].u.obj, off, len);
}

/* a configurable+enumerable accessor (get only) on `proto` */
static void ta_def_acc(js_ctx *ctx, js_object *proto, const char *name, js_native get)
{
  gc_push_scope();
  {
    js_object *g = (js_object *)gc_keep(js_new_native(ctx, get, name, 0));
    js_def_accessor(proto, name, g, 0, P_CONFIGURABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
}

static void setup_typed_arrays(js_ctx *ctx)
{
  static const struct {
    const char *name;
    js_native ctor;
    int type;
  } views[9] = {{"Int8Array", int8a_ctor, TA_I8},
                {"Uint8Array", uint8a_ctor, TA_U8},
                {"Uint8ClampedArray", uint8ca_ctor, TA_U8C},
                {"Int16Array", int16a_ctor, TA_I16},
                {"Uint16Array", uint16a_ctor, TA_U16},
                {"Int32Array", int32a_ctor, TA_I32},
                {"Uint32Array", uint32a_ctor, TA_U32},
                {"Float32Array", float32a_ctor, TA_F32},
                {"Float64Array", float64a_ctor, TA_F64}};
  int i;

  /* ArrayBuffer */
  ta_def_acc(ctx, ctx->arraybuffer_proto, "byteLength", ab_bytelength);
  js_def_native(ctx, ctx->arraybuffer_proto, "slice", ab_slice, 2);
  {
    js_object *c = make_ctor(ctx, "ArrayBuffer", arraybuffer_ctor, ctx->arraybuffer_proto);
    js_def_native(ctx, c, "isView", ab_isview, 1);
  }

  /* %TypedArray%.prototype: shared accessors + methods */
  ta_def_acc(ctx, ctx->typedarray_proto, "length", tap_length);
  ta_def_acc(ctx, ctx->typedarray_proto, "byteLength", tap_bytelength);
  ta_def_acc(ctx, ctx->typedarray_proto, "byteOffset", tap_byteoffset);
  ta_def_acc(ctx, ctx->typedarray_proto, "buffer", tap_buffer);
  js_def_native(ctx, ctx->typedarray_proto, "set", tap_set, 2);
  js_def_native(ctx, ctx->typedarray_proto, "subarray", tap_subarray, 2);
  js_def_native(ctx, ctx->typedarray_proto, "slice", tap_slice, 2);
  js_def_native(ctx, ctx->typedarray_proto, "fill", tap_fill, 3);
  js_def_native(ctx, ctx->typedarray_proto, "indexOf", tap_indexof, 1);
  js_def_native(ctx, ctx->typedarray_proto, "includes", tap_includes, 1);
  js_def_native(ctx, ctx->typedarray_proto, "join", tap_join, 1);
  js_def_native(ctx, ctx->typedarray_proto, "toString", tap_tostring, 0);
  js_def_native(ctx, ctx->typedarray_proto, "forEach", tap_foreach, 1);
  js_def_native(ctx, ctx->typedarray_proto, "map", tap_map, 1);
  js_def_native(ctx, ctx->typedarray_proto, "reduce", tap_reduce, 2);

  /* the nine views: a constructor + a per-view prototype chained to the shared
   * one */
  for (i = 0; i < 9; i++) {
    js_object *proto = ctx->typed_protos[i];
    js_object *c, *of_fn, *from_fn;
    gc_push_scope();
    c = (js_object *)gc_keep(js_new_native(ctx, views[i].ctor, views[i].name, 3));
    js_def(c, "prototype", js_obj_value(proto), 0);
    js_def(proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
    js_def(proto, "BYTES_PER_ELEMENT", js_number(ta_elem_size(views[i].type)), 0);
    js_def(c, "BYTES_PER_ELEMENT", js_number(ta_elem_size(views[i].type)), 0);
    of_fn = (js_object *)gc_keep(js_new_native(ctx, ta_of, "of", 0));
    of_fn->fn->data = js_number(views[i].type);
    js_def(c, "of", js_obj_value(of_fn), P_WRITABLE | P_CONFIGURABLE);
    from_fn = (js_object *)gc_keep(js_new_native(ctx, ta_from, "from", 1));
    from_fn->fn->data = js_number(views[i].type);
    js_def(c, "from", js_obj_value(from_fn), P_WRITABLE | P_CONFIGURABLE);
    js_def(ctx->global, views[i].name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  /* DataView */
  ta_def_acc(ctx, ctx->dataview_proto, "byteLength", dv_bytelength);
  ta_def_acc(ctx, ctx->dataview_proto, "byteOffset", tap_byteoffset);
  ta_def_acc(ctx, ctx->dataview_proto, "buffer", tap_buffer);
  js_def_native(ctx, ctx->dataview_proto, "getInt8", dv_get_i8, 1);
  js_def_native(ctx, ctx->dataview_proto, "setInt8", dv_set_i8, 2);
  js_def_native(ctx, ctx->dataview_proto, "getUint8", dv_get_u8, 1);
  js_def_native(ctx, ctx->dataview_proto, "setUint8", dv_set_u8, 2);
  js_def_native(ctx, ctx->dataview_proto, "getInt16", dv_get_i16, 1);
  js_def_native(ctx, ctx->dataview_proto, "setInt16", dv_set_i16, 2);
  js_def_native(ctx, ctx->dataview_proto, "getUint16", dv_get_u16, 1);
  js_def_native(ctx, ctx->dataview_proto, "setUint16", dv_set_u16, 2);
  js_def_native(ctx, ctx->dataview_proto, "getInt32", dv_get_i32, 1);
  js_def_native(ctx, ctx->dataview_proto, "setInt32", dv_set_i32, 2);
  js_def_native(ctx, ctx->dataview_proto, "getUint32", dv_get_u32, 1);
  js_def_native(ctx, ctx->dataview_proto, "setUint32", dv_set_u32, 2);
  js_def_native(ctx, ctx->dataview_proto, "getFloat32", dv_get_f32, 1);
  js_def_native(ctx, ctx->dataview_proto, "setFloat32", dv_set_f32, 2);
  js_def_native(ctx, ctx->dataview_proto, "getFloat64", dv_get_f64, 1);
  js_def_native(ctx, ctx->dataview_proto, "setFloat64", dv_set_f64, 2);
  make_ctor(ctx, "DataView", dataview_ctor, ctx->dataview_proto);
}

static js_object *make_ctor(js_ctx *ctx, const char *name, js_native fn, js_object *proto)
{
  js_object *c = js_new_native(ctx, fn, name, 1);
  gc_push_scope();
  gc_keep(c);
  gc_keep(proto);
  js_def(c, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->global, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return c;
}
/* ------------------------------------------------------- Intl ------------- *
 * ECMA-402 without ICU/CLDR data: en-US-style formatting, good enough that
 * sites which feature-detect and call Intl succeed and dates/numbers render
 * reasonably. Each sub-constructor stores its locale (@loc) and options object
 * (@opts) on the instance and carries its prototype in the native fn's `data`
 * (delivered as ctx->native_data), so both `Intl.X()` and `new Intl.X()` yield
 * a configured instance whose [[Prototype]] is X.prototype. */

static const char *const INTL_MONTH[12] = {"January", "February", "March",     "April",   "May",      "June",
                                           "July",    "August",   "September", "October", "November", "December"};
static const char *const INTL_MON3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const INTL_WDAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const INTL_WD3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void intl_puts(sbuf *sb, const char *s) { sb_put(sb, s, (int)strlen(s)); }
static void intl_app_int(sbuf *sb, int v)
{
  char b[16];
  int l;
  if (v < 0) {
    sb_putc(sb, '-');
    v = -v;
  }
  l = u64toa((unsigned long long)v, b, 10);
  sb_put(sb, b, l);
}
static void intl_app_pad2(sbuf *sb, int v)
{
  v = ((v % 100) + 100) % 100;
  sb_putc(sb, (char)('0' + v / 10));
  sb_putc(sb, (char)('0' + v % 10));
}

static js_object *intl_proto_of(js_ctx *ctx)
{
  return ctx->native_data.type == JV_OBJ ? ctx->native_data.u.obj : ctx->object_proto;
}

/* build a configured instance from (locales, options), storing both hidden */
static js_object *intl_new(js_ctx *ctx, js_value *a, int n)
{
  js_object *o;
  js_value loc, opts;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, intl_proto_of(ctx)));
  loc = js_mk_cstring(ctx, "en-US");
  if (n > 0) {
    if (a[0].type == JV_STR)
      loc = a[0];
    else if (a[0].type == JV_OBJ && a[0].u.obj->kind == OBJ_ARRAY && a[0].u.obj->elen > 0 &&
             a[0].u.obj->elems[0].type == JV_STR)
      loc = a[0].u.obj->elems[0];
  }
  opts = (n > 1 && a[1].type == JV_OBJ) ? a[1] : js_obj_value(js_new_object(ctx, ctx->object_proto));
  js_def(o, "@loc", loc, 0);
  js_def(o, "@opts", opts, 0);
  gc_pop_scope();
  return o;
}
static js_value intl_opt(js_ctx *ctx, js_value self, const char *f)
{
  js_value o = js_get(ctx, self, "@opts");
  return o.type == JV_OBJ ? js_get(ctx, o, f) : js_undef();
}
static const char *intl_opt_s(js_ctx *ctx, js_value self, const char *f, const char *dflt)
{
  js_value v = intl_opt(ctx, self, f);
  return v.type == JV_STR ? v.u.str->data : dflt;
}

/* generic .resolvedOptions() — locale + gregory/latn/UTC + a copy of the opts
 */
static js_value intl_resolved(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *o;
  js_value opts;
  unsigned char pf = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  (void)a;
  (void)n;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "locale", js_get(ctx, t, "@loc"), pf);
  js_def(o, "numberingSystem", js_mk_cstring(ctx, "latn"), pf);
  js_def(o, "calendar", js_mk_cstring(ctx, "gregory"), pf);
  js_def(o, "timeZone", js_mk_cstring(ctx, "UTC"), pf);
  opts = js_get(ctx, t, "@opts");
  if (opts.type == JV_OBJ) {
    int i;
    for (i = 0; i < opts.u.obj->props.n; i++) {
      pentry *pe = &opts.u.obj->props.e[i];
      if (pe->flags & P_ENUMERABLE)
        js_def(o, pe->key, pe->value, pf);
    }
  }
  gc_pop_scope();
  return js_obj_value(o);
}
/* wrap a formatted string as a single-part list, so callers that iterate
 * formatToParts() and read .value/.type still reassemble the whole string. */
static js_value intl_one_part(js_ctx *ctx, js_value str)
{
  js_object *arr, *part;
  unsigned char pf = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  gc_push_scope();
  KEEP(str);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  part = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(part, "type", js_mk_cstring(ctx, "literal"), pf);
  js_def(part, "value", str, pf);
  js_array_push(ctx, arr, js_obj_value(part));
  gc_pop_scope();
  return js_obj_value(arr);
}

/* --- DateTimeFormat --- */
static double intl_arg_ms(js_ctx *ctx, js_value v)
{
  if (v.type == JV_OBJ && v.u.obj->proto == ctx->date_proto)
    return date_t(ctx, v);
  if (v.type == JV_UNDEF)
    return ctx->now_ms ? (double)ctx->now_ms() : 0;
  return js_to_number(ctx, v);
}
static void dtf_build(js_ctx *ctx, js_value self, double ms, sbuf *sb)
{
  int Y, Mo, D, h, mi, s, mil, dow, hadDate = 0;
  const char *ds = intl_opt_s(ctx, self, "dateStyle", 0);
  const char *ts = intl_opt_s(ctx, self, "timeStyle", 0);
  const char *wd = intl_opt_s(ctx, self, "weekday", 0);
  const char *yr = intl_opt_s(ctx, self, "year", 0);
  const char *mo = intl_opt_s(ctx, self, "month", 0);
  const char *dy = intl_opt_s(ctx, self, "day", 0);
  const char *hr = intl_opt_s(ctx, self, "hour", 0);
  const char *mn = intl_opt_s(ctx, self, "minute", 0);
  const char *sc = intl_opt_s(ctx, self, "second", 0);
  js_value h12v = intl_opt(ctx, self, "hour12");
  int haveDate = ds || wd || yr || mo || dy;
  int haveTime = ts || hr || mn || sc;
  date_civil(ms, &Y, &Mo, &D, &h, &mi, &s, &mil, &dow);
  if (!haveDate && !haveTime) {
    haveDate = 1;
    yr = mo = dy = "numeric";
  } /* default: M/D/Y */

  if (haveDate) {
    if (ds && streq(ds, "short")) {
      intl_app_int(sb, Mo + 1);
      sb_putc(sb, '/');
      intl_app_int(sb, D);
      sb_putc(sb, '/');
      intl_app_int(sb, Y);
    } else if (ds) { /* full/long/medium */
      if (streq(ds, "full")) {
        intl_puts(sb, INTL_WDAY[dow]);
        intl_puts(sb, ", ");
      }
      intl_puts(sb, streq(ds, "medium") ? INTL_MON3[Mo] : INTL_MONTH[Mo]);
      sb_putc(sb, ' ');
      intl_app_int(sb, D);
      intl_puts(sb, ", ");
      intl_app_int(sb, Y);
    } else {
      int named = mo && (streq(mo, "long") || streq(mo, "short") || streq(mo, "narrow"));
      if (wd) {
        intl_puts(sb, streq(wd, "short") ? INTL_WD3[dow] : INTL_WDAY[dow]);
        if (streq(wd, "narrow")) {
          sb->n -= (int)strlen(INTL_WDAY[dow]);
          sb_putc(sb, INTL_WDAY[dow][0]);
        }
        if (mo || dy || yr)
          intl_puts(sb, ", ");
      }
      if (named) {
        if (streq(mo, "narrow"))
          sb_putc(sb, INTL_MONTH[Mo][0]);
        else
          intl_puts(sb, streq(mo, "short") ? INTL_MON3[Mo] : INTL_MONTH[Mo]);
        if (dy) {
          sb_putc(sb, ' ');
          if (streq(dy, "2-digit"))
            intl_app_pad2(sb, D);
          else
            intl_app_int(sb, D);
        }
        if (yr) {
          intl_puts(sb, ", ");
          if (streq(yr, "2-digit"))
            intl_app_pad2(sb, Y);
          else
            intl_app_int(sb, Y);
        }
      } else { /* numeric parts joined by '/' */
        int wrote = 0;
        if (mo) {
          if (streq(mo, "2-digit"))
            intl_app_pad2(sb, Mo + 1);
          else
            intl_app_int(sb, Mo + 1);
          wrote = 1;
        }
        if (dy) {
          if (wrote)
            sb_putc(sb, '/');
          if (streq(dy, "2-digit"))
            intl_app_pad2(sb, D);
          else
            intl_app_int(sb, D);
          wrote = 1;
        }
        if (yr) {
          if (wrote)
            sb_putc(sb, '/');
          if (streq(yr, "2-digit"))
            intl_app_pad2(sb, Y);
          else
            intl_app_int(sb, Y);
        }
      }
    }
    hadDate = 1;
  }
  if (haveTime) {
    int h12 = h % 12;
    const char *ampm = h < 12 ? "AM" : "PM";
    int wantSec;
    int use12 = h12v.type == JV_BOOL ? (h12v.u.num != 0) : 1;
    if (h12 == 0)
      h12 = 12;
    if (hadDate)
      intl_puts(sb, ", ");
    wantSec = ts ? (streq(ts, "medium") || streq(ts, "long") || streq(ts, "full")) : (sc != 0);
    if (use12)
      intl_app_int(sb, h12);
    else
      intl_app_pad2(sb, h);
    sb_putc(sb, ':');
    intl_app_pad2(sb, mi);
    if (wantSec) {
      sb_putc(sb, ':');
      intl_app_pad2(sb, s);
    }
    if (use12) {
      sb_putc(sb, ' ');
      intl_puts(sb, ampm);
    }
  }
}
static js_value dtf_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value dtf_format(js_ctx *ctx, js_value t, js_value *a, int n)
{
  sbuf sb;
  js_value r;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  dtf_build(ctx, t, intl_arg_ms(ctx, ARG(0)), &sb);
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value dtf_ftp(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return intl_one_part(ctx, dtf_format(ctx, t, a, n));
}
static js_value dtf_range(js_ctx *ctx, js_value t, js_value *a, int n)
{
  sbuf sb;
  js_value r;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  dtf_build(ctx, t, intl_arg_ms(ctx, ARG(0)), &sb);
  intl_puts(&sb, " – ");
  dtf_build(ctx, t, intl_arg_ms(ctx, ARG(1)), &sb);
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}

/* --- NumberFormat --- */
/* append x with [minFrac,maxFrac] fraction digits and optional 3-grouping */
static void intl_num(sbuf *sb, double x, int minFrac, int maxFrac, int group)
{
  char ds[48];
  int dl, i, neg = 0, intlen, fraclen;
  double scale = 1, scaled;
  if (x != x) {
    intl_puts(sb, "NaN");
    return;
  }
  if (x == 1.0 / 0.0) {
    intl_puts(sb, "∞");
    return;
  }
  if (x == -1.0 / 0.0) {
    intl_puts(sb, "-∞");
    return;
  }
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  if (maxFrac < 0)
    maxFrac = 0;
  if (maxFrac > 20)
    maxFrac = 20;
  if (minFrac < 0)
    minFrac = 0;
  if (minFrac > maxFrac)
    minFrac = maxFrac;
  for (i = 0; i < maxFrac; i++)
    scale *= 10;
  scaled = floor(x * scale + 0.5);
  if (scaled < 1.8e19)
    dl = u64toa((unsigned long long)scaled, ds, 10);
  else
    dl = dtoa(scaled, ds);
  while (dl <= maxFrac) {
    for (i = dl; i >= 0; i--)
      ds[i + 1] = ds[i];
    ds[0] = '0';
    dl++;
  } /* pad to >= 1 int digit */
  intlen = dl - maxFrac;
  fraclen = maxFrac;
  while (fraclen > minFrac && ds[intlen + fraclen - 1] == '0')
    fraclen--;
  if (neg)
    sb_putc(sb, '-');
  if (group) {
    for (i = 0; i < intlen; i++) {
      if (i && (intlen - i) % 3 == 0)
        sb_putc(sb, ',');
      sb_putc(sb, ds[i]);
    }
  } else
    sb_put(sb, ds, intlen);
  if (fraclen > 0) {
    sb_putc(sb, '.');
    sb_put(sb, ds + intlen, fraclen);
  }
}
static js_value nf_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value nf_format(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  const char *style = intl_opt_s(ctx, t, "style", "decimal");
  const char *cur = intl_opt_s(ctx, t, "currency", "USD");
  js_value minv = intl_opt(ctx, t, "minimumFractionDigits");
  js_value maxv = intl_opt(ctx, t, "maximumFractionDigits");
  js_value grpv = intl_opt(ctx, t, "useGrouping");
  int group = grpv.type == JV_BOOL ? (grpv.u.num != 0) : 1;
  int minF, maxF;
  sbuf sb;
  js_value r;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  if (streq(style, "currency")) {
    minF = (streq(cur, "JPY") || streq(cur, "KRW")) ? 0 : 2;
    maxF = minF;
  } else if (streq(style, "percent")) {
    minF = 0;
    maxF = 0;
    x = x * 100;
  } else {
    minF = 0;
    maxF = 3;
  }
  if (minv.type == JV_NUM)
    minF = (int)minv.u.num;
  if (maxv.type == JV_NUM)
    maxF = (int)maxv.u.num;
  else if (maxF < minF)
    maxF = minF;
  if (streq(style, "currency")) {
    int neg = x < 0;
    const char *sym = streq(cur, "USD")   ? "$"
                      : streq(cur, "EUR") ? "€"
                      : streq(cur, "GBP") ? "£"
                      : streq(cur, "JPY") ? "¥"
                                          : 0;
    if (neg)
      sb_putc(&sb, '-');
    if (sym)
      intl_puts(&sb, sym);
    else {
      intl_puts(&sb, cur);
      sb_putc(&sb, ' ');
    }
    intl_num(&sb, neg ? -x : x, minF, maxF, group);
  } else {
    intl_num(&sb, x, minF, maxF, group);
    if (streq(style, "percent"))
      sb_putc(&sb, '%');
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value nf_ftp(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return intl_one_part(ctx, nf_format(ctx, t, a, n));
}

/* --- Collator --- */
static js_value col_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value col_compare(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value sa = js_to_string(ctx, ARG(0)), sbv = js_to_string(ctx, ARG(1));
  const char *x = sa.type == JV_STR ? sa.u.str->data : "", *y = sbv.type == JV_STR ? sbv.u.str->data : "";
  const char *sens = intl_opt_s(ctx, t, "sensitivity", 0);
  int ci = sens && (streq(sens, "base") || streq(sens, "accent")), c = 0;
  (void)n;
  if (ci) {
    while (*x && *y) {
      char cx = lower(*x), cy = lower(*y);
      if (cx != cy) {
        c = (unsigned char)cx < (unsigned char)cy ? -1 : 1;
        break;
      }
      x++;
      y++;
    }
    if (!c)
      c = *x ? 1 : *y ? -1 : 0;
  } else {
    int d = strcmp(x, y);
    c = d < 0 ? -1 : d > 0 ? 1 : 0;
  }
  return js_number(c);
}

/* --- PluralRules --- */
static js_value pr_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value pr_select(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, ARG(0));
  long v = (long)x;
  (void)n;
  if (streq(intl_opt_s(ctx, t, "type", "cardinal"), "ordinal")) {
    long m10 = ((v % 10) + 10) % 10, m100 = ((v % 100) + 100) % 100;
    if (m10 == 1 && m100 != 11)
      return js_mk_cstring(ctx, "one");
    if (m10 == 2 && m100 != 12)
      return js_mk_cstring(ctx, "two");
    if (m10 == 3 && m100 != 13)
      return js_mk_cstring(ctx, "few");
    return js_mk_cstring(ctx, "other");
  }
  return js_mk_cstring(ctx, x == 1 ? "one" : "other");
}

/* --- RelativeTimeFormat --- */
static js_value rtf_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value rtf_format(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double v = js_to_number(ctx, ARG(0));
  js_value uv = js_to_string(ctx, ARG(1));
  const char *unit = uv.type == JV_STR ? uv.u.str->data : "";
  const char *num = intl_opt_s(ctx, t, "numeric", "always");
  long iv = (long)v, av = iv < 0 ? -iv : iv;
  int past = v < 0;
  char base[32];
  int bl = 0;
  sbuf sb;
  js_value r;
  (void)n;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  while (unit[bl] && bl < 31) {
    base[bl] = unit[bl];
    bl++;
  }
  base[bl] = 0;
  if (bl > 0 && base[bl - 1] == 's')
    base[bl - 1] = 0; /* singularize */
  if (streq(num, "auto") && streq(base, "day") && (iv == 0 || iv == -1 || iv == 1)) {
    intl_puts(&sb, iv == 0 ? "today" : iv == -1 ? "yesterday" : "tomorrow");
  } else if (past) {
    intl_app_int(&sb, (int)av);
    sb_putc(&sb, ' ');
    intl_puts(&sb, base);
    if (av != 1)
      sb_putc(&sb, 's');
    intl_puts(&sb, " ago");
  } else {
    intl_puts(&sb, "in ");
    intl_app_int(&sb, (int)av);
    sb_putc(&sb, ' ');
    intl_puts(&sb, base);
    if (av != 1)
      sb_putc(&sb, 's');
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value rtf_ftp(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return intl_one_part(ctx, rtf_format(ctx, t, a, n));
}

/* --- ListFormat --- */
static js_value lf_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
static js_value lf_format(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value arr = ARG(0);
  const char *conj = streq(intl_opt_s(ctx, t, "type", "conjunction"), "disjunction") ? "or" : "and";
  sbuf sb;
  js_value r;
  (void)n;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  if (arr.type == JV_OBJ && arr.u.obj->kind == OBJ_ARRAY) {
    unsigned i, len = arr.u.obj->elen;
    for (i = 0; i < len; i++) {
      js_value e;
      if (i > 0) {
        if (i == len - 1) {
          if (len > 2)
            sb_putc(&sb, ',');
          sb_putc(&sb, ' ');
          intl_puts(&sb, conj);
          sb_putc(&sb, ' ');
        } else {
          sb_putc(&sb, ',');
          sb_putc(&sb, ' ');
        }
      }
      e = js_to_string(ctx, arr.u.obj->elems[i]);
      if (e.type == JV_STR)
        sb_put(&sb, e.u.str->data, e.u.str->len);
    }
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value lf_ftp(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return intl_one_part(ctx, lf_format(ctx, t, a, n));
}

/* --- Locale --- */
static js_value locale_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tagv = n > 0 ? js_to_string(ctx, a[0]) : js_mk_cstring(ctx, "en-US");
  const char *tag = tagv.type == JV_STR ? tagv.u.str->data : "en-US";
  char lang[16] = {0}, script[16] = {0}, region[16] = {0};
  int i = 0, k = 0;
  unsigned char pf = P_ENUMERABLE | P_CONFIGURABLE;
  js_object *o;
  (void)t;
  while (tag[i] && tag[i] != '-' && k < 15)
    lang[k++] = tag[i++]; /* language */
  while (tag[i] == '-') {
    char part[16];
    int pl = 0;
    i++;
    while (tag[i] && tag[i] != '-' && pl < 15)
      part[pl++] = tag[i++];
    part[pl] = 0;
    if (pl == 4 && !script[0]) {
      int j;
      for (j = 0; j < pl; j++)
        script[j] = part[j];
    } else if ((pl == 2 || pl == 3) && !region[0]) {
      int j;
      for (j = 0; j < pl; j++)
        region[j] = part[j];
    }
  }
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, intl_proto_of(ctx)));
  js_def(o, "baseName", tagv, pf);
  js_def(o, "language", js_mk_cstring(ctx, lang), pf);
  js_def(o, "script", script[0] ? js_mk_cstring(ctx, script) : js_undef(), pf);
  js_def(o, "region", region[0] ? js_mk_cstring(ctx, region) : js_undef(), pf);
  js_def(o, "@loc", tagv, 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value locale_tostring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return js_get(ctx, t, "baseName");
}

/* --- Segmenter --- */
static js_value seg_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  return js_obj_value(intl_new(ctx, a, n));
}
/* returns an array (iterable) of { segment, index, input, isWordLike } */
static js_value seg_segment(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value sv = js_to_string(ctx, ARG(0));
  const char *s = sv.type == JV_STR ? sv.u.str->data : "";
  int len = sv.type == JV_STR ? sv.u.str->len : 0;
  const char *gran = intl_opt_s(ctx, t, "granularity", "grapheme");
  int word = streq(gran, "word"), i = 0;
  js_object *out;
  unsigned char pf = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  (void)n;
  gc_push_scope();
  KEEP(sv);
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  while (i < len) {
    int start = i, wl;
    if (word) {
      int isw = re_word((unsigned char)s[i]);
      while (i < len && (re_word((unsigned char)s[i]) != 0) == (isw != 0))
        i++;
      wl = isw;
    } else {
      i++;
      wl = re_word((unsigned char)s[start]);
    }
    {
      js_object *seg = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
      js_def(seg, "segment", str_of(ctx, s + start, i - start), pf);
      js_def(seg, "index", js_number(start), pf);
      js_def(seg, "input", sv, pf);
      js_def(seg, "isWordLike", js_bool(wl), pf);
      js_array_push(ctx, out, js_obj_value(seg));
    }
  }
  gc_pop_scope();
  return js_obj_value(out);
}

/* --- Intl statics --- */
static js_value intl_canonical(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *out;
  (void)t;
  (void)n;
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (ARG(0).type == JV_STR)
    js_array_push(ctx, out, ARG(0));
  else if (ARG(0).type == JV_OBJ && ARG(0).u.obj->kind == OBJ_ARRAY) {
    unsigned i;
    for (i = 0; i < ARG(0).u.obj->elen; i++)
      js_array_push(ctx, out, js_to_string(ctx, ARG(0).u.obj->elems[i]));
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value intl_supported(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *k = ARG(0).type == JV_STR ? ARG(0).u.str->data : "";
  js_object *out;
  int i;
  (void)t;
  (void)n;
  static const char *cal[] = {"gregory", 0}, *cur[] = {"USD", "EUR", "GBP", "JPY", 0}, *ns[] = {"latn", 0},
                    *tz[] = {"UTC", 0}, *unit[] = {"day", "hour", "minute", "second", 0};
  const char **src = streq(k, "calendar")          ? cal
                     : streq(k, "currency")        ? cur
                     : streq(k, "numberingSystem") ? ns
                     : streq(k, "timeZone")        ? tz
                     : streq(k, "unit")            ? unit
                                                   : cal;
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; src[i]; i++)
    js_array_push(ctx, out, js_mk_cstring(ctx, src[i]));
  gc_pop_scope();
  return js_obj_value(out);
}

/* toLocale* on Number/Date, delegating to the same formatting */
static js_value num_tolocale(js_ctx *ctx, js_value t, js_value *a, int n)
{
  double x = js_to_number(ctx, t);
  sbuf sb;
  js_value r;
  (void)a;
  (void)n;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  intl_num(&sb, x, 0, 3, 1);
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value date_tolocale(js_ctx *ctx, js_value t, js_value *a, int n, int mode)
{
  int Y, Mo, D, h, mi, s, mil, dow;
  sbuf sb;
  js_value r;
  int h12;
  const char *ampm;
  (void)a;
  (void)n;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  date_civil(date_t(ctx, t), &Y, &Mo, &D, &h, &mi, &s, &mil, &dow);
  if (mode != 2) {
    intl_app_int(&sb, Mo + 1);
    sb_putc(&sb, '/');
    intl_app_int(&sb, D);
    sb_putc(&sb, '/');
    intl_app_int(&sb, Y);
  }
  if (mode == 0)
    intl_puts(&sb, ", ");
  if (mode != 1) {
    h12 = h % 12;
    ampm = h < 12 ? "AM" : "PM";
    if (h12 == 0)
      h12 = 12;
    intl_app_int(&sb, h12);
    sb_putc(&sb, ':');
    intl_app_pad2(&sb, mi);
    sb_putc(&sb, ':');
    intl_app_pad2(&sb, s);
    sb_putc(&sb, ' ');
    intl_puts(&sb, ampm);
  }
  r = str_of(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}
static js_value date_tolocalestring(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return date_tolocale(ctx, t, a, n, 0);
}
static js_value date_tolocaledate(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return date_tolocale(ctx, t, a, n, 1);
}
static js_value date_tolocaletime(js_ctx *ctx, js_value t, js_value *a, int n)
{
  return date_tolocale(ctx, t, a, n, 2);
}

/* --- WeakRef / FinalizationRegistry --- *
 * No true weak semantics here (the target is held strongly, never reclaimed):
 * enough for feature-detection and deref()-based caches to work. */
static js_value weakref_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *o;
  (void)t;
  (void)n;
  if (ARG(0).type != JV_OBJ && ARG(0).type != JV_SYMBOL)
    return js_throw_type(ctx, "WeakRef: target must be an object or symbol");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, intl_proto_of(ctx)));
  js_def(o, "@t", ARG(0), 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value weakref_deref(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return js_get(ctx, t, "@t");
}
static js_value finreg_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *o;
  (void)t;
  (void)n;
  if (!js_is_callable(ARG(0)))
    return js_throw_type(ctx, "FinalizationRegistry: cleanup callback must be callable");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, intl_proto_of(ctx)));
  js_def(o, "@cb", ARG(0), 0);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value finreg_register(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)a;
  (void)n;
  return js_undef();
}
static js_value finreg_unregister(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)a;
  (void)n;
  return js_bool(0);
}

/* create an Intl sub-constructor: proto lives in the fn's data (→
 * ctx->native_data). */
static js_object *intl_sub(js_ctx *ctx, js_object *intl, const char *name, js_native fn, js_object **proto_out)
{
  js_object *proto, *c;
  gc_push_scope();
  proto = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  c = (js_object *)gc_keep(js_new_native(ctx, fn, name, 0));
  c->fn->data = js_obj_value(proto);
  gc_write_barrier(c);
  js_def(c, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(intl, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  if (proto_out)
    *proto_out = proto;
  return c;
}
static void setup_intl(js_ctx *ctx)
{
  js_object *intl, *p, *wc, *wrp, *fc, *frp;
  gc_push_scope();
  intl = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));

  intl_sub(ctx, intl, "DateTimeFormat", dtf_ctor, &p);
  js_def_native(ctx, p, "format", dtf_format, 1);
  js_def_native(ctx, p, "formatToParts", dtf_ftp, 1);
  js_def_native(ctx, p, "formatRange", dtf_range, 2);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "NumberFormat", nf_ctor, &p);
  js_def_native(ctx, p, "format", nf_format, 1);
  js_def_native(ctx, p, "formatToParts", nf_ftp, 1);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "Collator", col_ctor, &p);
  js_def_native(ctx, p, "compare", col_compare, 2);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "PluralRules", pr_ctor, &p);
  js_def_native(ctx, p, "select", pr_select, 1);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "RelativeTimeFormat", rtf_ctor, &p);
  js_def_native(ctx, p, "format", rtf_format, 2);
  js_def_native(ctx, p, "formatToParts", rtf_ftp, 2);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "ListFormat", lf_ctor, &p);
  js_def_native(ctx, p, "format", lf_format, 1);
  js_def_native(ctx, p, "formatToParts", lf_ftp, 1);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  intl_sub(ctx, intl, "Locale", locale_ctor, &p);
  js_def_native(ctx, p, "toString", locale_tostring, 0);

  intl_sub(ctx, intl, "Segmenter", seg_ctor, &p);
  js_def_native(ctx, p, "segment", seg_segment, 1);
  js_def_native(ctx, p, "resolvedOptions", intl_resolved, 0);

  js_def_native(ctx, intl, "getCanonicalLocales", intl_canonical, 1);
  js_def_native(ctx, intl, "supportedValuesOf", intl_supported, 1);
  js_def(ctx->global, "Intl", js_obj_value(intl), P_WRITABLE | P_CONFIGURABLE);

  js_def_native(ctx, ctx->number_proto, "toLocaleString", num_tolocale, 0);
  js_def_native(ctx, ctx->date_proto, "toLocaleString", date_tolocalestring, 0);
  js_def_native(ctx, ctx->date_proto, "toLocaleDateString", date_tolocaledate, 0);
  js_def_native(ctx, ctx->date_proto, "toLocaleTimeString", date_tolocaletime, 0);

  wrp = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  wc = make_ctor(ctx, "WeakRef", weakref_ctor, wrp);
  wc->fn->data = js_obj_value(wrp);
  gc_write_barrier(wc);
  js_def_native(ctx, wrp, "deref", weakref_deref, 0);

  frp = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  fc = make_ctor(ctx, "FinalizationRegistry", finreg_ctor, frp);
  fc->fn->data = js_obj_value(frp);
  gc_write_barrier(fc);
  js_def_native(ctx, frp, "register", finreg_register, 2);
  js_def_native(ctx, frp, "unregister", finreg_unregister, 1);
  gc_pop_scope();
}

static void def_global(js_ctx *ctx, const char *name, js_value v)
{
  js_def(ctx->global, name, v, P_WRITABLE | P_CONFIGURABLE);
}
static void def_global_fn(js_ctx *ctx, const char *name, js_native fn, int arity)
{
  js_def_native(ctx, ctx->global, name, fn, arity);
}

void js_setup_builtins(js_ctx *ctx)
{
  gc_push_scope();

  /* Object.prototype methods + the Object constructor */
  js_def_native(ctx, ctx->object_proto, "hasOwnProperty", op_hasown, 1);
  js_def_native(ctx, ctx->object_proto, "toString", op_tostring, 0);
  js_def_native(ctx, ctx->object_proto, "valueOf", op_valueof, 0);
  js_def_native(ctx, ctx->object_proto, "isPrototypeOf", op_isproto, 1);
  {
    js_object *octor = make_ctor(ctx, "Object", native_object, ctx->object_proto);
    js_def_native(ctx, octor, "keys", object_keys, 1);
    js_def_native(ctx, octor, "values", object_values, 1);
    js_def_native(ctx, octor, "entries", object_entries, 1);
    js_def_native(ctx, octor, "assign", object_assign, 2);
    js_def_native(ctx, octor, "create", object_create, 2);
    js_def_native(ctx, octor, "getPrototypeOf", object_getproto, 1);
    js_def_native(ctx, octor, "setPrototypeOf", object_setproto, 2);
    js_def_native(ctx, octor, "freeze", object_freeze, 1);
    js_def_native(ctx, octor, "isFrozen", object_isfrozen, 1);
    js_def_native(ctx, octor, "defineProperty", object_defprop, 3);
    js_def_native(ctx, octor, "getOwnPropertyNames", object_ownnames, 1);
    js_def_native(ctx, octor, "getOwnPropertySymbols", object_getownsymbols, 1);
    js_def_native(ctx, octor, "fromEntries", object_fromentries, 1);
    js_def_native(ctx, octor, "defineProperties", object_defineprops, 2);
    js_def_native(ctx, octor, "getOwnPropertyDescriptor", object_getownpropdesc, 2);
    js_def_native(ctx, octor, "getOwnPropertyDescriptors", object_getownpropdescs, 1);
    js_def_native(ctx, octor, "isExtensible", object_isextensible, 1);
    js_def_native(ctx, octor, "preventExtensions", object_preventext, 1);
    js_def_native(ctx, octor, "seal", object_seal, 1);
    js_def_native(ctx, octor, "isSealed", object_issealed, 1);
    js_def_native(ctx, octor, "is", object_is, 2);
    js_def_native(ctx, octor, "hasOwn", object_hasown, 2);
    js_def_native(ctx, octor, "groupBy", object_groupby, 2);
    js_def_native(ctx, ctx->object_proto, "propertyIsEnumerable", proto_propisenum, 1);
  }

  /* Reflect */
  {
    js_object *refl = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(refl);
    js_def(ctx->global, "Reflect", js_obj_value(refl), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(ctx, refl, "get", reflect_get, 2);
    js_def_native(ctx, refl, "set", reflect_set, 3);
    js_def_native(ctx, refl, "has", reflect_has, 2);
    js_def_native(ctx, refl, "deleteProperty", reflect_deleteprop, 2);
    js_def_native(ctx, refl, "ownKeys", reflect_ownkeys, 1);
    js_def_native(ctx, refl, "defineProperty", reflect_defineprop, 3);
    js_def_native(ctx, refl, "getOwnPropertyDescriptor", reflect_getownpropdesc, 2);
    js_def_native(ctx, refl, "getPrototypeOf", reflect_getproto, 1);
    js_def_native(ctx, refl, "setPrototypeOf", reflect_setproto, 2);
    js_def_native(ctx, refl, "isExtensible", reflect_isext, 1);
    js_def_native(ctx, refl, "preventExtensions", reflect_preventext, 1);
    js_def_native(ctx, refl, "apply", reflect_apply, 3);
    js_def_native(ctx, refl, "construct", reflect_construct, 2);
    gc_pop_scope();
  }
  { /* Proxy — real trap dispatch (value.c js_proxy_*); Proxy.revocable too */
    js_object *pc = make_ctor(ctx, "Proxy", proxy_ctor, ctx->object_proto);
    js_def_native(ctx, pc, "revocable", proxy_revocable, 2);
  }

  /* Function.prototype (minimal) */
  js_def_native(ctx, ctx->function_proto, "toString", function_tostring, 0);
  js_def_native(ctx, ctx->function_proto, "call", fn_call, 1);
  js_def_native(ctx, ctx->function_proto, "apply", fn_apply, 2);
  js_def_native(ctx, ctx->function_proto, "bind", fn_bind, 1);

  /* Array */
  js_def_native(ctx, ctx->array_proto, "push", array_push, 1);
  js_def_native(ctx, ctx->array_proto, "pop", array_pop, 0);
  js_def_native(ctx, ctx->array_proto, "shift", array_shift, 0);
  js_def_native(ctx, ctx->array_proto, "unshift", array_unshift, 1);
  js_def_native(ctx, ctx->array_proto, "slice", array_slice, 2);
  js_def_native(ctx, ctx->array_proto, "splice", array_splice, 2);
  js_def_native(ctx, ctx->array_proto, "concat", array_concat, 1);
  js_def_native(ctx, ctx->array_proto, "join", array_join, 1);
  js_def_native(ctx, ctx->array_proto, "toString", array_tostring, 0);
  js_def_native(ctx, ctx->array_proto, "indexOf", array_indexof, 1);
  js_def_native(ctx, ctx->array_proto, "lastIndexOf", array_lastindexof, 1);
  js_def_native(ctx, ctx->array_proto, "includes", array_includes, 1);
  js_def_native(ctx, ctx->array_proto, "forEach", array_foreach, 1);
  js_def_native(ctx, ctx->array_proto, "map", array_map, 1);
  js_def_native(ctx, ctx->array_proto, "filter", array_filter, 1);
  js_def_native(ctx, ctx->array_proto, "some", array_some, 1);
  js_def_native(ctx, ctx->array_proto, "every", array_every, 1);
  js_def_native(ctx, ctx->array_proto, "find", array_find, 1);
  js_def_native(ctx, ctx->array_proto, "findIndex", array_findindex, 1);
  js_def_native(ctx, ctx->array_proto, "reduce", array_reduce, 1);
  js_def_native(ctx, ctx->array_proto, "reverse", array_reverse, 0);
  js_def_native(ctx, ctx->array_proto, "fill", array_fill, 1);
  js_def_native(ctx, ctx->array_proto, "flat", array_flat, 0);
  js_def_native(ctx, ctx->array_proto, "sort", array_sort, 1);
  js_def_native(ctx, ctx->array_proto, "at", array_at, 1);
  js_def_native(ctx, ctx->array_proto, "flatMap", array_flatmap, 1);
  js_def_native(ctx, ctx->array_proto, "findLast", array_findlast, 1);
  js_def_native(ctx, ctx->array_proto, "findLastIndex", array_findlastindex, 1);
  js_def_native(ctx, ctx->array_proto, "reduceRight", array_reduceright, 1);
  js_def_native(ctx, ctx->array_proto, "copyWithin", array_copywithin, 2);
  js_def_native(ctx, ctx->array_proto, "values", array_values_m, 0);
  js_def_native(ctx, ctx->array_proto, "keys", array_keys_m, 0);
  js_def_native(ctx, ctx->array_proto, "entries", array_entries_m, 0);
  {
    js_object *actor = make_ctor(ctx, "Array", native_array, ctx->array_proto);
    js_def_native(ctx, actor, "isArray", array_isarray, 1);
    js_def_native(ctx, actor, "from", array_from, 1);
    js_def_native(ctx, actor, "of", array_of, 0);
  }

  /* String */
  js_def_native(ctx, ctx->string_proto, "charAt", str_charat, 1);
  js_def_native(ctx, ctx->string_proto, "charCodeAt", str_charcode, 1);
  js_def_native(ctx, ctx->string_proto, "codePointAt", str_charcode, 1);
  js_def_native(ctx, ctx->string_proto, "at", str_at, 1);
  js_def_native(ctx, ctx->string_proto, "indexOf", str_indexof, 1);
  js_def_native(ctx, ctx->string_proto, "localeCompare", str_localecompare, 1);
  js_def_native(ctx, ctx->string_proto, "lastIndexOf", str_lastindexof, 1);
  js_def_native(ctx, ctx->string_proto, "includes", str_includes, 1);
  js_def_native(ctx, ctx->string_proto, "startsWith", str_startswith, 1);
  js_def_native(ctx, ctx->string_proto, "endsWith", str_endswith, 1);
  js_def_native(ctx, ctx->string_proto, "slice", str_slice, 2);
  js_def_native(ctx, ctx->string_proto, "substring", str_substring, 2);
  js_def_native(ctx, ctx->string_proto, "substr", str_substr, 2);
  js_def_native(ctx, ctx->string_proto, "toUpperCase", str_upper, 0);
  js_def_native(ctx, ctx->string_proto, "toLowerCase", str_lower, 0);
  js_def_native(ctx, ctx->string_proto, "toLocaleUpperCase", str_upper, 0);
  js_def_native(ctx, ctx->string_proto, "toLocaleLowerCase", str_lower, 0);
  js_def_native(ctx, ctx->string_proto, "normalize", str_normalize, 0);
  js_def_native(ctx, ctx->string_proto, "trim", strm_trim, 0);
  js_def_native(ctx, ctx->string_proto, "trimStart", strm_trimstart, 0);
  js_def_native(ctx, ctx->string_proto, "trimEnd", strm_trimend, 0);
  js_def_native(ctx, ctx->string_proto, "split", str_split, 2);
  js_def_native(ctx, ctx->string_proto, "replace", str_replace, 2);
  js_def_native(ctx, ctx->string_proto, "replaceAll", str_replaceall, 2);
  js_def_native(ctx, ctx->string_proto, "match", str_match, 1);
  js_def_native(ctx, ctx->string_proto, "matchAll", str_matchall, 1);
  js_def_native(ctx, ctx->string_proto, "search", str_search, 1);
  js_def_native(ctx, ctx->string_proto, "repeat", str_repeat, 1);
  js_def_native(ctx, ctx->string_proto, "padStart", str_padstart, 1);
  js_def_native(ctx, ctx->string_proto, "padEnd", str_padend, 1);
  js_def_native(ctx, ctx->string_proto, "concat", str_concat, 1);
  js_def_native(ctx, ctx->string_proto, "toString", str_tostring, 0);
  js_def_native(ctx, ctx->string_proto, "valueOf", str_tostring, 0);
  {
    js_object *sctor = make_ctor(ctx, "String", native_string, ctx->string_proto);
    js_def_native(ctx, sctor, "fromCharCode", string_fromcharcode, 1);
    js_def_native(ctx, sctor, "fromCodePoint", string_fromcodepoint, 1);
    js_def_native(ctx, sctor, "raw", string_raw, 1);
  }

  /* Number */
  js_def_native(ctx, ctx->number_proto, "toFixed", num_tofixed, 1);
  js_def_native(ctx, ctx->number_proto, "toExponential", num_toexp, 1);
  js_def_native(ctx, ctx->number_proto, "toPrecision", num_toprecision, 1);
  js_def_native(ctx, ctx->number_proto, "toString", num_tostring, 1);
  js_def_native(ctx, ctx->number_proto, "valueOf", prim_valueof, 0);
  {
    js_object *nctor = make_ctor(ctx, "Number", native_number, ctx->number_proto);
    js_def_native(ctx, nctor, "isInteger", num_isinteger, 1);
    js_def_native(ctx, nctor, "isFinite", num_isfinite, 1);
    js_def_native(ctx, nctor, "isNaN", num_isnan, 1);
    js_def_native(ctx, nctor, "isSafeInteger", num_issafe, 1);
    js_def_native(ctx, nctor, "parseFloat", g_parsefloat, 1);
    js_def_native(ctx, nctor, "parseInt", g_parseint, 2);
    js_def(nctor, "MAX_SAFE_INTEGER", js_number(9007199254740991.0), 0);
    js_def(nctor, "MIN_SAFE_INTEGER", js_number(-9007199254740991.0), 0);
    js_def(nctor, "MAX_VALUE", js_number(1.7976931348623157e308), 0);
    js_def(nctor, "MIN_VALUE", js_number(5e-324), 0);
    js_def(nctor, "EPSILON", js_number(2.220446049250313e-16), 0);
    js_def(nctor, "POSITIVE_INFINITY", js_number(1.0 / 0.0), 0);
    js_def(nctor, "NEGATIVE_INFINITY", js_number(-1.0 / 0.0), 0);
    js_def(nctor, "NaN", js_number(0.0 / 0.0), 0);
  }

  /* Boolean */
  js_def_native(ctx, ctx->boolean_proto, "toString", bool_tostring, 0);
  js_def_native(ctx, ctx->boolean_proto, "valueOf", prim_valueof, 0);
  make_ctor(ctx, "Boolean", native_boolean, ctx->boolean_proto);

  /* Error family */
  js_def(ctx->error_proto, "name", js_mk_cstring(ctx, "Error"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->error_proto, "message", js_mk_cstring(ctx, ""), P_WRITABLE | P_CONFIGURABLE);
  js_def_native(ctx, ctx->error_proto, "toString", err_tostring, 0);
  js_def(ctx->type_error_proto, "name", js_mk_cstring(ctx, "TypeError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->range_error_proto, "name", js_mk_cstring(ctx, "RangeError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->syntax_error_proto, "name", js_mk_cstring(ctx, "SyntaxError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->ref_error_proto, "name", js_mk_cstring(ctx, "ReferenceError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->eval_error_proto, "name", js_mk_cstring(ctx, "EvalError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->uri_error_proto, "name", js_mk_cstring(ctx, "URIError"), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->aggregate_error_proto, "name", js_mk_cstring(ctx, "AggregateError"), P_WRITABLE | P_CONFIGURABLE);
  make_ctor(ctx, "Error", err_error, ctx->error_proto);
  make_ctor(ctx, "TypeError", err_type, ctx->type_error_proto);
  make_ctor(ctx, "RangeError", err_range, ctx->range_error_proto);
  make_ctor(ctx, "SyntaxError", err_syntax, ctx->syntax_error_proto);
  make_ctor(ctx, "ReferenceError", err_ref, ctx->ref_error_proto);
  make_ctor(ctx, "EvalError", err_evalerr, ctx->eval_error_proto);
  make_ctor(ctx, "URIError", err_uri, ctx->uri_error_proto);
  make_ctor(ctx, "AggregateError", err_aggregate, ctx->aggregate_error_proto);

  /* Math */
  {
    js_object *math = js_new_object(ctx, ctx->object_proto);
    gc_keep(math);
    js_def(ctx->global, "Math", js_obj_value(math), P_WRITABLE | P_CONFIGURABLE);
    js_def(math, "PI", js_number(3.141592653589793), 0);
    js_def(math, "E", js_number(2.718281828459045), 0);
    js_def(math, "LN2", js_number(0.6931471805599453), 0);
    js_def(math, "LN10", js_number(2.302585092994046), 0);
    js_def(math, "LOG2E", js_number(1.4426950408889634), 0);
    js_def(math, "LOG10E", js_number(0.4342944819032518), 0);
    js_def(math, "SQRT2", js_number(1.4142135623730951), 0);
    js_def(math, "SQRT1_2", js_number(0.7071067811865476), 0);
    js_def_native(ctx, math, "abs", m_abs, 1);
    js_def_native(ctx, math, "floor", m_floor, 1);
    js_def_native(ctx, math, "ceil", m_ceil, 1);
    js_def_native(ctx, math, "round", m_round, 1);
    js_def_native(ctx, math, "trunc", m_trunc, 1);
    js_def_native(ctx, math, "sign", m_sign, 1);
    js_def_native(ctx, math, "sqrt", m_sqrt, 1);
    js_def_native(ctx, math, "cbrt", m_cbrt, 1);
    js_def_native(ctx, math, "pow", m_pow, 2);
    js_def_native(ctx, math, "exp", m_exp, 1);
    js_def_native(ctx, math, "log", m_log, 1);
    js_def_native(ctx, math, "log2", m_log2, 1);
    js_def_native(ctx, math, "log10", m_log10, 1);
    js_def_native(ctx, math, "sin", m_sin, 1);
    js_def_native(ctx, math, "cos", m_cos, 1);
    js_def_native(ctx, math, "tan", m_tan, 1);
    js_def_native(ctx, math, "asin", m_asin, 1);
    js_def_native(ctx, math, "acos", m_acos, 1);
    js_def_native(ctx, math, "atan", m_atan, 1);
    js_def_native(ctx, math, "atan2", m_atan2, 2);
    js_def_native(ctx, math, "sinh", m_sinh, 1);
    js_def_native(ctx, math, "cosh", m_cosh, 1);
    js_def_native(ctx, math, "tanh", m_tanh, 1);
    js_def_native(ctx, math, "hypot", m_hypot, 2);
    js_def_native(ctx, math, "asinh", m_asinh, 1);
    js_def_native(ctx, math, "acosh", m_acosh, 1);
    js_def_native(ctx, math, "atanh", m_atanh, 1);
    js_def_native(ctx, math, "expm1", m_expm1, 1);
    js_def_native(ctx, math, "log1p", m_log1p, 1);
    js_def_native(ctx, math, "fround", m_fround, 1);
    js_def_native(ctx, math, "clz32", m_clz32, 1);
    js_def_native(ctx, math, "imul", m_imul, 2);
    js_def_native(ctx, math, "max", m_max, 2);
    js_def_native(ctx, math, "min", m_min, 2);
    js_def_native(ctx, math, "random", m_random, 0);
    (void)m_round_;
  }

  /* Promise */
  js_def_native(ctx, ctx->promise_proto, "then", promise_then, 2);
  js_def_native(ctx, ctx->promise_proto, "catch", promise_catch, 1);
  js_def_native(ctx, ctx->promise_proto, "finally", promise_finally, 1);
  {
    js_object *pctor = make_ctor(ctx, "Promise", promise_ctor, ctx->promise_proto);
    js_def_native(ctx, pctor, "resolve", promise_resolve_s, 1);
    js_def_native(ctx, pctor, "reject", promise_reject_s, 1);
    js_def_native(ctx, pctor, "all", promise_all_s, 1);
    js_def_native(ctx, pctor, "race", promise_all_s, 1); /* simplified: first-settled-ish */
    js_def_native(ctx, pctor, "allSettled", promise_allsettled_s, 1);
    js_def_native(ctx, pctor, "any", promise_any_s, 1);
  }

  /* Symbol (+ well-known symbols). Created before Map/Set so [Symbol.iterator]
   * keys are available. */
  js_def_native(ctx, ctx->symbol_proto, "toString", symbol_tostring, 0);
  js_def_native(ctx, ctx->symbol_proto, "valueOf", symbol_valueof, 0);
  {
    js_object *g = js_new_native(ctx, symbol_desc_get, "description", 0);
    js_def_accessor(ctx->symbol_proto, "description", g, 0, P_CONFIGURABLE);
  }
  {
    js_object *sctor = make_ctor(ctx, "Symbol", symbol_ctor, ctx->symbol_proto);
    gc_push_scope();
    gc_keep(sctor);
    ctx->sym_for_reg = js_new_object(ctx, ctx->object_proto);
    js_def(sctor, "@reg", js_obj_value(ctx->sym_for_reg), 0);
    js_def_native(ctx, sctor, "for", symbol_for, 1);
    js_def_native(ctx, sctor, "keyFor", symbol_keyfor, 1);
    ctx->sym_iterator = js_mk_symbol(ctx, "Symbol.iterator");
    js_def(sctor, "iterator", ctx->sym_iterator, 0);
    js_def(sctor, "asyncIterator", js_mk_symbol(ctx, "Symbol.asyncIterator"), 0);
    js_def(sctor, "hasInstance", js_mk_symbol(ctx, "Symbol.hasInstance"), 0);
    js_def(sctor, "toPrimitive", js_mk_symbol(ctx, "Symbol.toPrimitive"), 0);
    ctx->sym_tostringtag = js_mk_symbol(ctx, "Symbol.toStringTag");
    js_def(sctor, "toStringTag", ctx->sym_tostringtag, 0);
    gc_pop_scope();
  }
  {
    js_object *f = js_new_native(ctx, set_values, "[Symbol.iterator]", 0);
    js_def(ctx->array_proto, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  {
    js_object *f = js_new_native(ctx, string_values_m, "[Symbol.iterator]", 0);
    js_def(ctx->string_proto, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  js_def_native(ctx, ctx->string_proto, "values", string_values_m, 0);

  /* BigInt (after Symbol so Symbol.toStringTag exists) */
  js_def_native(ctx, ctx->bigint_proto, "toString", bigint_tostring, 0);
  js_def_native(ctx, ctx->bigint_proto, "toLocaleString", bigint_tostring, 0);
  js_def_native(ctx, ctx->bigint_proto, "valueOf", bigint_valueof, 0);
  if (ctx->sym_tostringtag.type == JV_SYMBOL)
    js_def(ctx->bigint_proto, js_symbol_key(ctx->sym_tostringtag), js_mk_cstring(ctx, "BigInt"), 0);
  {
    js_object *bctor = make_ctor(ctx, "BigInt", bigint_ctor, ctx->bigint_proto);
    js_def_native(ctx, bctor, "asUintN", bigint_asuintn, 2);
    js_def_native(ctx, bctor, "asIntN", bigint_asintn, 2);
  }

  /* Map */
  js_def_native(ctx, ctx->map_proto, "set", map_set, 2);
  js_def_native(ctx, ctx->map_proto, "get", map_get, 1);
  js_def_native(ctx, ctx->map_proto, "has", coll_has, 1);
  js_def_native(ctx, ctx->map_proto, "delete", map_delete, 1);
  js_def_native(ctx, ctx->map_proto, "clear", coll_clear, 0);
  js_def_native(ctx, ctx->map_proto, "forEach", map_foreach, 1);
  js_def_native(ctx, ctx->map_proto, "keys", map_keys, 0);
  js_def_native(ctx, ctx->map_proto, "values", map_values, 0);
  js_def_native(ctx, ctx->map_proto, "entries", map_entries, 0);
  {
    js_object *g = js_new_native(ctx, coll_size, "size", 0);
    js_def_accessor(ctx->map_proto, "size", g, 0, P_CONFIGURABLE);
  }
  {
    js_object *f = js_new_native(ctx, map_entries, "[Symbol.iterator]", 0);
    js_def(ctx->map_proto, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  make_ctor(ctx, "Map", map_constructor, ctx->map_proto);

  /* Set */
  js_def_native(ctx, ctx->set_proto, "add", set_add, 1);
  js_def_native(ctx, ctx->set_proto, "has", coll_has, 1);
  js_def_native(ctx, ctx->set_proto, "delete", set_delete, 1);
  js_def_native(ctx, ctx->set_proto, "clear", coll_clear, 0);
  js_def_native(ctx, ctx->set_proto, "forEach", set_foreach, 1);
  js_def_native(ctx, ctx->set_proto, "values", set_values, 0);
  js_def_native(ctx, ctx->set_proto, "keys", set_values, 0);
  js_def_native(ctx, ctx->set_proto, "entries", set_entries, 0);
  {
    js_object *g = js_new_native(ctx, coll_size, "size", 0);
    js_def_accessor(ctx->set_proto, "size", g, 0, P_CONFIGURABLE);
  }
  {
    js_object *f = js_new_native(ctx, set_values, "[Symbol.iterator]", 0);
    js_def(ctx->set_proto, js_symbol_key(ctx->sym_iterator), js_obj_value(f), 0);
  }
  make_ctor(ctx, "Set", set_constructor, ctx->set_proto);

  /* WeakMap / WeakSet (non-weak: keys held strongly; object/symbol keys only)
   */
  js_def_native(ctx, ctx->weakmap_proto, "set", weakmap_set, 2);
  js_def_native(ctx, ctx->weakmap_proto, "get", map_get, 1);
  js_def_native(ctx, ctx->weakmap_proto, "has", coll_has, 1);
  js_def_native(ctx, ctx->weakmap_proto, "delete", map_delete, 1);
  make_ctor(ctx, "WeakMap", weakmap_constructor, ctx->weakmap_proto);
  js_def_native(ctx, ctx->weakset_proto, "add", weakset_add, 1);
  js_def_native(ctx, ctx->weakset_proto, "has", coll_has, 1);
  js_def_native(ctx, ctx->weakset_proto, "delete", set_delete, 1);
  make_ctor(ctx, "WeakSet", weakset_constructor, ctx->weakset_proto);

  /* Date — time stored as epoch-millis in a hidden "@t" property; all fields
   * are UTC (no local timezone, getTimezoneOffset is 0). */
  js_def_native(ctx, ctx->date_proto, "getTime", date_getTime, 0);
  js_def_native(ctx, ctx->date_proto, "valueOf", date_getTime, 0);
  js_def_native(ctx, ctx->date_proto, "getFullYear", date_getFullYear, 0);
  js_def_native(ctx, ctx->date_proto, "getMonth", date_getMonth, 0);
  js_def_native(ctx, ctx->date_proto, "getDate", date_getDate, 0);
  js_def_native(ctx, ctx->date_proto, "getDay", date_getDay, 0);
  js_def_native(ctx, ctx->date_proto, "getHours", date_getHours, 0);
  js_def_native(ctx, ctx->date_proto, "getMinutes", date_getMinutes, 0);
  js_def_native(ctx, ctx->date_proto, "getSeconds", date_getSeconds, 0);
  js_def_native(ctx, ctx->date_proto, "getMilliseconds", date_getMilliseconds, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCFullYear", date_getFullYear, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCMonth", date_getMonth, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCDate", date_getDate, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCDay", date_getDay, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCHours", date_getHours, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCMinutes", date_getMinutes, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCSeconds", date_getSeconds, 0);
  js_def_native(ctx, ctx->date_proto, "getUTCMilliseconds", date_getMilliseconds, 0);
  js_def_native(ctx, ctx->date_proto, "getTimezoneOffset", date_tzoff, 0);
  js_def_native(ctx, ctx->date_proto, "toISOString", date_iso, 0);
  js_def_native(ctx, ctx->date_proto, "toJSON", date_iso, 0);
  js_def_native(ctx, ctx->date_proto, "toString", date_iso, 0);
  {
    js_object *dc = make_ctor(ctx, "Date", date_ctor, ctx->date_proto);
    js_def_native(ctx, dc, "now", date_now, 0);
  }

  /* RegExp: prototype methods + the constructor (also reachable as the type of
   * a `/.../ ` literal, which interp.c builds via js_make_regexp). */
  js_def_native(ctx, ctx->regexp_proto, "test", re_test, 1);
  js_def_native(ctx, ctx->regexp_proto, "exec", re_proto_exec, 1);
  js_def_native(ctx, ctx->regexp_proto, "toString", re_tostring, 0);
  make_ctor(ctx, "RegExp", regexp_ctor, ctx->regexp_proto);

  /* ArrayBuffer / TypedArrays / DataView */
  setup_typed_arrays(ctx);

  /* JSON */
  {
    js_object *json = js_new_object(ctx, ctx->object_proto);
    gc_keep(json);
    js_def(ctx->global, "JSON", js_obj_value(json), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(ctx, json, "stringify", json_stringify, 3);
    js_def_native(ctx, json, "parse", json_parse, 2);
  }

  /* console */
  {
    js_object *con = js_new_object(ctx, ctx->object_proto);
    gc_keep(con);
    js_def(ctx->global, "console", js_obj_value(con), P_WRITABLE | P_CONFIGURABLE);
    js_def_native(ctx, con, "log", native_console_log, 0);
    js_def_native(ctx, con, "error", native_console_error, 0);
    js_def_native(ctx, con, "warn", native_console_warn, 0);
    js_def_native(ctx, con, "info", native_console_info, 0);
    js_def_native(ctx, con, "debug", native_console_debug, 0);
  }

  /* global functions and values */
  def_global_fn(ctx, "parseInt", g_parseint, 2);
  def_global_fn(ctx, "parseFloat", g_parsefloat, 1);
  def_global_fn(ctx, "isNaN", g_isnan, 1);
  def_global_fn(ctx, "structuredClone", g_structured_clone, 1);
  def_global_fn(ctx, "isFinite", g_isfinite, 1);
  def_global_fn(ctx, "encodeURIComponent", g_encode_uri_component, 1);
  def_global_fn(ctx, "encodeURI", g_encode_uri, 1);
  def_global_fn(ctx, "decodeURIComponent", g_decode_uri_component, 1);
  def_global_fn(ctx, "escape", g_escape, 1);
  def_global_fn(ctx, "unescape", g_unescape, 1);
  def_global_fn(ctx, "decodeURI", g_decode_uri, 1);
  def_global_fn(ctx, "btoa", g_btoa, 1);
  def_global_fn(ctx, "atob", g_atob, 1);
  def_global_fn(ctx, "eval", js_builtin_eval, 1);
  {
    js_object *fctor = js_new_native(ctx, js_builtin_function, "Function", 1);
    gc_push_scope();
    gc_keep(fctor);
    js_def(fctor, "prototype", js_obj_value(ctx->function_proto), 0);
    js_def(ctx->function_proto, "constructor", js_obj_value(fctor), P_WRITABLE | P_CONFIGURABLE);
    js_def(ctx->global, "Function", js_obj_value(fctor), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_global(ctx, "undefined", js_undef());
  def_global(ctx, "NaN", js_number(0.0 / 0.0));
  def_global(ctx, "Infinity", js_number(1.0 / 0.0));
  js_def(ctx->global, "globalThis", js_obj_value(ctx->global), P_WRITABLE | P_CONFIGURABLE);

  setup_intl(ctx);
  gc_pop_scope();
}
