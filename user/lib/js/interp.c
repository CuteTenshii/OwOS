/* interp.c — the tree-walk interpreter. See interp.h for the contract and the
 * GC rooting strategy. The evaluator pins every value it holds across an
 * allocation with the JS_KEEP discipline (gc_push_scope/gc_keep), since a
 * collection can only happen inside the js_new_* allocators. */
#include "interp.h"
#include "ast.h"
#include "fmt.h"
#include "gc.h"
#include "lexer.h" /* jtok kinds used as operator ids on AST nodes */
#include "math.h"
#include "owos.h"
#include "parser.h" /* js_parse / js_parse_error for eval + the Function ctor */
#include "value.h"

#define MAX_DEPTH 800

static void *refp(js_value v)
{
  if (v.type == JV_OBJ || v.type == JV_SYMBOL || v.type == JV_BIGINT)
    return v.u.obj;
  if (v.type == JV_STR)
    return v.u.str;
  return NULL;
}
#define KEEP(v)                                                                                                        \
  do {                                                                                                                 \
    void *_p = refp(v);                                                                                                \
    if (_p)                                                                                                            \
      gc_keep(_p);                                                                                                     \
  } while (0)

/* A computed key value -> property-key string. Symbols map to their internal
 * key; everything else goes through ToString. *hold receives a value to gc_keep
 * so the returned pointer stays valid across the subsequent get/set. */
static const char *to_prop_key(js_ctx *ctx, js_value k, js_value *hold)
{
  if (k.type == JV_SYMBOL) {
    *hold = k;
    return js_symbol_key(k);
  }
  *hold = js_to_string(ctx, k);
  return ctx->has_exc ? "" : hold->u.str->data;
}

/* ------------------------------------------------------- completion ------- */

enum { C_NORMAL, C_RETURN, C_BREAK, C_CONTINUE };
typedef struct {
  int kind;
  js_value value;
  const char *label;
} completion;
static completion cnorm(void)
{
  completion c;
  c.kind = C_NORMAL;
  c.value = js_undef();
  c.label = NULL;
  return c;
}

/* ------------------------------------------------------- forward decls ---- */

static js_value evaluate(js_ctx *ctx, js_node *n);
static completion exec_stmt(js_ctx *ctx, js_node *n, const char *label);
static completion exec_list(js_ctx *ctx, js_node **items, int count);
static void bind_pattern(js_ctx *ctx, js_node *pat, js_value val, int declare, unsigned char flags);
static js_value call_function(js_ctx *ctx, js_value fnval, js_value thisv, js_value *args, int argc);
static js_object *super_home_proto(js_ctx *ctx);
static int get_iterator(js_ctx *ctx, js_value v, js_value *iter);
static int iterator_step(js_ctx *ctx, js_value iter, js_value *value);

/* a small public shim so builtins.c can read own props without the static map
 */
pentry *pmap_find_pub(js_object *o, const char *key);
int pmap_find_in_env(js_env *e, const char *name);

/* ------------------------------------------------------- exceptions ------- */

void js_throw(js_ctx *ctx, js_value err)
{
  ctx->has_exc = 1;
  ctx->exc = err;
  ctx->exc_root = refp(err);
#ifdef JS_TRACE_THROW
  /* Debug build only: log EVERY thrown exception (even ones that get caught)
   * with its captured stack, so a swallowed failure — e.g. a module whose init
   * aborts, leaving a later access to throw a misleading downstream error — is
   * visible at the point it actually happened. Off in release (no
   * JS_TRACE_THROW). */
  if (ctx->print && err.type == JV_OBJ) {
    pentry *st = pmap_find_pub(err.u.obj, "stack");
    if (st && st->value.type == JV_STR) {
      js_string *s = st->value.u.str;
      char *buf = (char *)malloc((unsigned)s->len + 10);
      if (buf) {
        int k = 0;
        const char *p;
        for (p = "[throw] "; *p; p++)
          buf[k++] = *p;
        memcpy(buf + k, s->data, (unsigned)s->len);
        k += s->len;
        ctx->print(ctx, JS_LOG_ERROR, buf, k);
        free(buf);
      }
    }
  }
#endif
}

/* Build "Name: message\n    at fn (line:col)\n    at fn2 (line:col)\n..." from
 * the active call frames (innermost first) and store it as the error's `.stack`
 * — captured at creation, like a real engine. `o` must already be GC-rooted. */
static char *put_uint(char *p, unsigned v)
{
  char tmp[12];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *p++ = tmp[--n];
  return p;
}

/* Upper bound on the bytes emit_frames writes for the current frames plus the
 * trailing script-origin line. */
static int frames_bytes(js_ctx *ctx)
{
  int i, total = 0;
  /* per frame: "    at " + name + " (line:col)\n" — 32 bytes fixed/number slack
   */
  for (i = 0; i < ctx->nframes; i++)
    total += 32 + (int)strlen(ctx->frames[i]);
  /* the outermost frame is the script/async entry itself: "    at <label>\n" */
  if (ctx->cur_script && ctx->cur_script[0])
    total += 8 + (int)strlen(ctx->cur_script);
  return total;
}

/* Write the "    at fn (line:col)\n" lines for the active frames (innermost
 * last), then the "    at <script>\n" origin line, to `p` and return the new
 * write cursor. The origin line makes every captured segment bottom out at the
 * script (or async entry) that produced it, even when no JS function is on the
 * stack. */
static char *emit_frames(js_ctx *ctx, char *p)
{
  int i;
  const char *s;
  for (i = ctx->nframes - 1; i >= 0; i--) {
    /* the innermost frame sits at the throw site (cur_line/col); every outer
     * frame sits at the call that entered the frame below it. */
    int line = (i == ctx->nframes - 1) ? ctx->cur_line : ctx->frame_line[i];
    int col = (i == ctx->nframes - 1) ? ctx->cur_col : ctx->frame_col[i];
    for (s = "    at "; *s; s++)
      *p++ = *s;
    for (s = ctx->frames[i]; *s; s++)
      *p++ = *s;
    if (line > 0) {
      *p++ = ' ';
      *p++ = '(';
      p = put_uint(p, (unsigned)line);
      *p++ = ':';
      p = put_uint(p, (unsigned)(col > 0 ? col : 1));
      *p++ = ')';
    }
    *p++ = '\n';
  }
  if (ctx->cur_script && ctx->cur_script[0]) {
    for (s = "    at "; *s; s++)
      *p++ = *s;
    for (s = ctx->cur_script; *s; s++)
      *p++ = *s;
    *p++ = '\n';
  }
  return p;
}

/* Separator that precedes the frames of an async (deferred) parent in a stack.
 */
static const char ASYNC_SEP[] = "    --- scheduled from ---\n";

static void capture_stack(js_ctx *ctx, js_object *o, const char *name, const char *msg)
{
  int total;
  char *buf, *p;
  const char *s;
  const char *async = ctx->async_stack;
  total = (int)strlen(name ? name : "Error") + 2 + (int)strlen(msg ? msg : "") + 2;
  total += frames_bytes(ctx);
  if (async)
    total += (int)sizeof ASYNC_SEP + (int)strlen(async);
  buf = (char *)malloc((unsigned)total + 1);
  if (!buf)
    return;
  p = buf;
  for (s = name ? name : "Error"; *s; s++)
    *p++ = *s;
  if (msg && *msg) {
    *p++ = ':';
    *p++ = ' ';
    for (s = msg; *s; s++)
      *p++ = *s;
  }
  *p++ = '\n';
  p = emit_frames(ctx, p);
  if (async) { /* graft the scheduling site's frames */
    for (s = ASYNC_SEP; *s; s++)
      *p++ = *s;
    for (s = async; *s; s++)
      *p++ = *s; /* already newline-terminated */
  }
  *p = '\0';
  js_def(o, "stack", js_mk_string(ctx, buf, (int)(p - buf)), P_WRITABLE | P_CONFIGURABLE);
  free(buf);
}

char *js_async_capture(js_ctx *ctx)
{
  int total;
  char *buf, *p;
  const char *s;
  const char *prev = ctx->async_stack;
  total = frames_bytes(ctx);
  if (prev)
    total += (int)sizeof ASYNC_SEP + (int)strlen(prev);
  buf = (char *)malloc((unsigned)total + 1);
  if (!buf)
    return 0;
  p = emit_frames(ctx, buf);
  if (prev) { /* chain the already-installed parent */
    for (s = ASYNC_SEP; *s; s++)
      *p++ = *s;
    for (s = prev; *s; s++)
      *p++ = *s;
  }
  *p = '\0';
  return buf;
}

char *js_async_enter(js_ctx *ctx, char *captured)
{
  char *prev = ctx->async_stack;
  ctx->async_stack = captured;
  return prev;
}

void js_async_leave(js_ctx *ctx, char *prev) { ctx->async_stack = prev; }

void js_set_script(js_ctx *ctx, const char *label) { ctx->cur_script = label; }

js_value js_make_error(js_ctx *ctx, js_object *proto, const char *name, const char *msg)
{
  js_object *o;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, proto ? proto : ctx->error_proto));
  o->kind = OBJ_ERROR;
  js_def(o, "message", js_mk_cstring(ctx, msg ? msg : ""), P_WRITABLE | P_CONFIGURABLE);
  if (name)
    js_def(o, "name", js_mk_cstring(ctx, name), P_WRITABLE | P_CONFIGURABLE);
  capture_stack(ctx, o, name, msg);
  gc_pop_scope();
  return js_obj_value(o);
}

js_value js_throw_error(js_ctx *ctx, js_object *proto, const char *name, const char *msg)
{
  js_throw(ctx, js_make_error(ctx, proto, name, msg));
  return js_undef();
}
js_value js_throw_type(js_ctx *ctx, const char *msg)
{
  return js_throw_error(ctx, ctx->type_error_proto, "TypeError", msg);
}
js_value js_throw_range(js_ctx *ctx, const char *msg)
{
  return js_throw_error(ctx, ctx->range_error_proto, "RangeError", msg);
}
static js_value throw_ref(js_ctx *ctx, const char *msg)
{
  return js_throw_error(ctx, ctx->ref_error_proto, "ReferenceError", msg);
}

void js_set_stack_limit(js_ctx *ctx, void *low_limit) { ctx->stack_limit = (char *)low_limit; }

/* Native-stack overflow guard. The evaluator and native callbacks recurse on
 * the C stack, so deep recursion (or a deeply nested AST) would smash past a
 * bounded thread stack. `sp` is the caller's own frame address; the limit is
 * the floor the embedder reserved headroom above. A fixed call-count cap can't
 * catch this: per-frame stack cost varies, and expression recursion never bumps
 * the count. */
static int stack_overflow(js_ctx *ctx, void *sp) { return ctx->stack_limit && (char *)sp < ctx->stack_limit; }

void js_def_native(js_ctx *ctx, js_object *o, const char *name, js_native fn, int arity)
{
  gc_push_scope();
  {
    js_object *f = (js_object *)gc_keep(js_new_native(ctx, fn, name, arity));
    js_def(o, name, js_obj_value(f), P_WRITABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
}

/* ------------------------------------------------------- identifiers ------ */

static int nullish(js_value v) { return v.type == JV_UNDEF || v.type == JV_NULL; }

static int has_global(js_ctx *ctx, const char *name)
{
  js_object *o;
  for (o = ctx->global; o; o = o->proto)
    if (js_has_own(o, name))
      return 1;
  return 0;
}

/* Resolve an identifier reference to its binding, using the node's inline
 * cache. The cache records how far up the scope chain the binding sits (cdepth)
 * and its slot in that scope's variable map (cslot). It is only ever a hint: a
 * hit walks to the cached scope and revalidates by comparing the key at the
 * slot, so a stale entry (delete/rehash, or a scope that no longer nests the
 * same way) simply falls through to a full walk and re-records. Direct eval,
 * which can inject a binding into an existing scope, bumps ctx->ident_cache_gen
 * to drop every cache at once. */
static pentry *resolve_ident(js_ctx *ctx, js_node *node, js_env **owner)
{
  const char *name = node->u.str.s;
  js_env *e;
  int depth;

  if (node->u.str.cslot && node->u.str.cgen == ctx->ident_cache_gen) {
    js_env *t = ctx->cur_env;
    int d = node->u.str.cdepth;
    while (d > 0 && t) {
      t = t->parent;
      d--;
    }
    if (t && d == 0) {
      int idx = node->u.str.cslot - 1;
      if (idx < t->vars.n && strcmp(t->vars.e[idx].key, name) == 0) {
        if (owner)
          *owner = t;
        return &t->vars.e[idx];
      }
    }
  }
  for (e = ctx->cur_env, depth = 0; e; e = e->parent, depth++) {
    int idx = pmap_index(&e->vars, name);
    if (idx >= 0) {
      node->u.str.cdepth = depth;
      node->u.str.cslot = idx + 1;
      node->u.str.cgen = ctx->ident_cache_gen;
      if (owner)
        *owner = e;
      return &e->vars.e[idx];
    }
  }
  return NULL;
}

static js_value read_ident(js_ctx *ctx, js_node *node)
{
  const char *name = node->u.str.s;
  pentry *pe = resolve_ident(ctx, node, NULL);
  if (pe)
    return pe->value;
  if (has_global(ctx, name))
    return js_get(ctx, js_obj_value(ctx->global), name);
  {
    char buf[160];
    int n = strlen(name);
    if (n > 140)
      n = 140;
    memcpy(buf, name, (unsigned)n);
    memcpy(buf + n, " is not defined", 16);
    return throw_ref(ctx, buf);
  }
}

static void assign_ident(js_ctx *ctx, js_node *node, js_value val)
{
  js_env *owner = NULL;
  pentry *pe = resolve_ident(ctx, node, &owner);
  if (pe) {
    if (!(pe->flags & P_WRITABLE)) {
      js_throw_type(ctx, "assignment to constant variable");
      return;
    }
    pe->value = val;
    gc_write_barrier(owner); /* an old scope may now hold a young value */
    return;
  }
  js_set(ctx, js_obj_value(ctx->global), node->u.str.s, val);
}

/* ------------------------------------------------------- operators -------- */

static int str_cmp_bytes(js_string *a, js_string *b)
{
  int n = a->len < b->len ? a->len : b->len;
  int i;
  for (i = 0; i < n; i++) {
    unsigned char x = (unsigned char)a->data[i], y = (unsigned char)b->data[i];
    if (x != y)
      return x < y ? -1 : 1;
  }
  if (a->len != b->len)
    return a->len < b->len ? -1 : 1;
  return 0;
}

/* -1/0/1 ordering, or 2 when unordered (a NaN was involved) */
static int js_compare(js_ctx *ctx, js_value l, js_value r)
{
  js_value lp = js_to_primitive(ctx, l, 1);
  js_value rp = js_to_primitive(ctx, r, 1);
  if (ctx->has_exc)
    return 2;
  if (lp.type == JV_STR && rp.type == JV_STR)
    return str_cmp_bytes(lp.u.str, rp.u.str);
  {
    double a = js_to_number(ctx, lp), b = js_to_number(ctx, rp);
    if (a != a || b != b)
      return 2;
    if (a < b)
      return -1;
    if (a > b)
      return 1;
    return 0;
  }
}

static int has_property(js_ctx *ctx, js_object *o, const char *key)
{
  for (; o; o = o->proto) {
    if (o->kind == OBJ_PROXY)
      return js_proxy_has(ctx, o, key); /* has trap (+ proto walk) */
    if (js_has_own(o, key))
      return 1;
  }
  return 0;
}

/* Compare a BigInt against a Number/String/BigInt operand for the relational
 * operators: -1/0/1 ordering, or 2 when unordered (a NaN / non-numeric string).
 */
static int cmp_bigint_double(const bigint *b, double d)
{
  if (d != d)
    return 2;
  if (floor(d) == d && d != 1.0 / 0.0 && d != -1.0 / 0.0) {
    int ok;
    bigint *bd = bi_from_double(d, &ok);
    if (ok && bd) {
      int c = bi_cmp(b, bd);
      bi_free(bd);
      return c;
    }
    if (bd)
      bi_free(bd);
  }
  {
    double bv = bi_to_double(b);
    return bv < d ? -1 : (bv > d ? 1 : 0);
  }
}
static int bigint_compare(js_ctx *ctx, js_value l, js_value r)
{
  bigint *lb = js_bigint_of(l), *rb = js_bigint_of(r);
  if (lb && rb)
    return bi_cmp(lb, rb);
  if (lb) {
    if (r.type == JV_STR) {
      int ok;
      bigint *t = bi_from_str(r.u.str->data, r.u.str->len, &ok);
      int c;
      if (!ok || !t) {
        if (t)
          bi_free(t);
        return 2;
      }
      c = bi_cmp(lb, t);
      bi_free(t);
      return c;
    }
    return cmp_bigint_double(lb, js_to_number(ctx, r));
  }
  if (l.type == JV_STR) {
    int ok;
    bigint *t = bi_from_str(l.u.str->data, l.u.str->len, &ok);
    int c;
    if (!ok || !t) {
      if (t)
        bi_free(t);
      return 2;
    }
    c = bi_cmp(t, rb);
    bi_free(t);
    return c;
  }
  {
    int c = cmp_bigint_double(rb, js_to_number(ctx, l));
    return c == 2 ? 2 : -c;
  }
}

/* BigInt operators. Returns 1 if it handled `op` (setting *out, which may be a
 * thrown value), 0 for ops the generic path owns (==, ===, in, instanceof, and
 * `+` with a string operand, which coerces to string concatenation). */
static int bigint_binop(js_ctx *ctx, int op, js_value l, js_value r, js_value *out)
{
  bigint *lb, *rb, *res;

  /* `+` is BigInt addition only when both sides are BigInt; otherwise the
   * generic path runs ToPrimitive and either concatenates (a string/stringy
   * object operand) or throws the mix TypeError via ToNumber. */
  if (op == T_PLUS && !(js_bigint_of(l) && js_bigint_of(r)))
    return 0;
  if (op == T_EQEQ || op == T_NE || op == T_EQEQEQ || op == T_NEEQ || op == T_IN || op == T_INSTANCEOF)
    return 0;

  if (op == T_LT || op == T_GT || op == T_LE || op == T_GE) {
    int c = bigint_compare(ctx, l, r), b;
    if (ctx->has_exc) {
      *out = js_undef();
      return 1;
    }
    switch (op) {
    case T_LT:
      b = (c == -1);
      break;
    case T_GT:
      b = (c == 1);
      break;
    case T_LE:
      b = (c == -1 || c == 0);
      break;
    default:
      b = (c == 1 || c == 0);
      break;
    }
    if (c == 2)
      b = 0;
    *out = js_bool(b);
    return 1;
  }

  lb = js_bigint_of(l);
  rb = js_bigint_of(r);
  if (!lb || !rb) {
    *out = js_throw_type(ctx, "Cannot mix BigInt and other types, use explicit conversions");
    return 1;
  }

  res = NULL;
  switch (op) {
  case T_PLUS:
    res = bi_add(lb, rb);
    break;
  case T_MINUS:
    res = bi_sub(lb, rb);
    break;
  case T_STAR:
    res = bi_mul(lb, rb);
    break;
  case T_SLASH:
    if (bi_is_zero(rb)) {
      *out = js_throw_range(ctx, "Division by zero");
      return 1;
    }
    bi_divmod(lb, rb, &res, NULL);
    break;
  case T_PERCENT:
    if (bi_is_zero(rb)) {
      *out = js_throw_range(ctx, "Division by zero");
      return 1;
    }
    bi_divmod(lb, rb, NULL, &res);
    break;
  case T_STARSTAR: {
    int rc;
    res = bi_pow(lb, rb, &rc);
    if (rc == -1) {
      *out = js_throw_range(ctx, "Exponent must be non-negative");
      return 1;
    }
    break;
  }
  case T_AMP:
    res = bi_and(lb, rb);
    break;
  case T_BAR:
    res = bi_or(lb, rb);
    break;
  case T_CARET:
    res = bi_xor(lb, rb);
    break;
  case T_LTLT: {
    int ok;
    long long k = bi_to_i64(rb, &ok);
    res = ok ? bi_shl(lb, k) : NULL;
    break;
  }
  case T_GTGT: {
    int ok;
    long long k = bi_to_i64(rb, &ok);
    res = ok ? bi_shr(lb, k) : NULL;
    break;
  }
  case T_GTGTGT:
    *out = js_throw_type(ctx, "BigInts have no unsigned right shift");
    return 1;
  default:
    return 0;
  }
  if (!res) {
    *out = js_throw_range(ctx, "Maximum BigInt size exceeded");
    return 1;
  }
  *out = js_mk_bigint(ctx, res);
  return 1;
}

static js_value apply_binop(js_ctx *ctx, int op, js_value l, js_value r)
{
  if (l.type == JV_BIGINT || r.type == JV_BIGINT) {
    js_value out;
    if (bigint_binop(ctx, op, l, r, &out))
      return out;
  }
  switch (op) {
  case T_PLUS: {
    js_value lp, rp;
    gc_push_scope();
    KEEP(l);
    KEEP(r);
    lp = js_to_primitive(ctx, l, 1);
    KEEP(lp);
    rp = js_to_primitive(ctx, r, 1);
    KEEP(rp);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    if (lp.type == JV_STR || rp.type == JV_STR) {
      js_value ls = js_to_string(ctx, lp);
      KEEP(ls);
      js_value rs = js_to_string(ctx, rp);
      KEEP(rs);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      {
        js_value out;
        out.type = JV_STR;
        out.u.str = js_str_concat(ctx, ls.u.str, rs.u.str);
        gc_pop_scope();
        return out;
      }
    }
    {
      double v = js_to_number(ctx, lp) + js_to_number(ctx, rp);
      gc_pop_scope();
      return js_number(v);
    }
  }
  case T_MINUS:
    return js_number(js_to_number(ctx, l) - js_to_number(ctx, r));
  case T_STAR:
    return js_number(js_to_number(ctx, l) * js_to_number(ctx, r));
  case T_SLASH:
    return js_number(js_to_number(ctx, l) / js_to_number(ctx, r));
  case T_PERCENT:
    return js_number(fmod(js_to_number(ctx, l), js_to_number(ctx, r)));
  case T_STARSTAR:
    return js_number(pow(js_to_number(ctx, l), js_to_number(ctx, r)));
  case T_LT: {
    int c = js_compare(ctx, l, r);
    return js_bool(c == -1);
  }
  case T_GT: {
    int c = js_compare(ctx, l, r);
    return js_bool(c == 1);
  }
  case T_LE: {
    int c = js_compare(ctx, l, r);
    return js_bool(c == -1 || c == 0);
  }
  case T_GE: {
    int c = js_compare(ctx, l, r);
    return js_bool(c == 1 || c == 0);
  }
  case T_EQEQ:
    return js_bool(js_loose_eq(ctx, l, r));
  case T_NE:
    return js_bool(!js_loose_eq(ctx, l, r));
  case T_EQEQEQ:
    return js_bool(js_strict_eq(l, r));
  case T_NEEQ:
    return js_bool(!js_strict_eq(l, r));
  case T_AMP:
    return js_number(js_to_int32(ctx, l) & js_to_int32(ctx, r));
  case T_BAR:
    return js_number(js_to_int32(ctx, l) | js_to_int32(ctx, r));
  case T_CARET:
    return js_number(js_to_int32(ctx, l) ^ js_to_int32(ctx, r));
  case T_LTLT:
    return js_number(js_to_int32(ctx, l) << (js_to_uint32(ctx, r) & 31));
  case T_GTGT:
    return js_number(js_to_int32(ctx, l) >> (js_to_uint32(ctx, r) & 31));
  case T_GTGTGT:
    return js_number((double)(js_to_uint32(ctx, l) >> (js_to_uint32(ctx, r) & 31)));
  case T_IN:
    if (r.type != JV_OBJ)
      return js_throw_type(ctx, "'in' requires an object");
    {
      js_value hold;
      const char *key = to_prop_key(ctx, l, &hold);
      if (ctx->has_exc)
        return js_undef();
      return js_bool(has_property(ctx, r.u.obj, key));
    }
  case T_INSTANCEOF: {
    js_value proto;
    js_object *o;
    if (!js_is_callable(r))
      return js_throw_type(ctx, "right-hand side of 'instanceof' is not callable");
    proto = js_get(ctx, r, "prototype");
    if (proto.type != JV_OBJ)
      return js_throw_type(ctx, "prototype is not an object");
    if (l.type != JV_OBJ)
      return js_bool(0);
    for (o = l.u.obj->proto; o; o = o->proto)
      if (o == proto.u.obj)
        return js_bool(1);
    return js_bool(0);
  }
  default:
    return js_undef();
  }
}

/* ------------------------------------------------------- member access ---- */

/* Access m off objv. When keyout != NULL (and cap > 0) the resolved property
 * key is copied there — used to render "obj[\"key\"] is not a function" for a
 * computed callee, where the static AST no longer carries the runtime name. */
static js_value access_ex(js_ctx *ctx, js_value objv, js_node *m, char *keyout, int cap)
{
  if (m->u.member.computed) {
    js_value k = evaluate(ctx, m->u.member.prop);
    if (ctx->has_exc)
      return js_undef();
    if (k.type == JV_NUM) {
      double d = k.u.num;
      if (d >= 0 && d == floor(d) && d < 4294967296.0) {
        if (keyout && cap > 66)
          u64toa((unsigned)d, keyout, 10);
        else if (keyout && cap > 0)
          keyout[0] = '\0';
        return js_get_index(ctx, objv, (unsigned)d);
      }
    }
    gc_push_scope();
    KEEP(objv);
    KEEP(k);
    {
      js_value hold;
      const char *key = to_prop_key(ctx, k, &hold);
      js_value out;
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      KEEP(hold);
      if (keyout && cap > 0)
        strlcpy(keyout, key, (unsigned)cap);
      out = js_get(ctx, objv, key);
      gc_pop_scope();
      return out;
    }
  }
  return js_get(ctx, objv, m->u.member.prop->u.str.s);
}

static js_value access(js_ctx *ctx, js_value objv, js_node *m) { return access_ex(ctx, objv, m, NULL, 0); }

/* evaluate a member/call chain, threading optional-chaining short-circuit */
static js_value eval_chain(js_ctx *ctx, js_node *n, int *shorted);

static js_value eval_chain_obj(js_ctx *ctx, js_node *n, int *shorted)
{
  if (n->type == JS_MEMBER || n->type == JS_CALL)
    return eval_chain(ctx, n, shorted);
  return evaluate(ctx, n);
}

static js_value *eval_args(js_ctx *ctx, js_node **anodes, int an, int *out_argc)
{
  js_value *argv;
  int cap = an > 0 ? an : 1, i;
  int argc = 0;
  argv = (js_value *)malloc((unsigned)cap * sizeof(js_value));
  for (i = 0; i < an; i++) {
    js_node *a = anodes[i];
    if (a->type == JS_SPREAD) {
      js_value sv = evaluate(ctx, a->u.one.arg);
      if (ctx->has_exc) {
        free(argv);
        return NULL;
      }
      KEEP(sv);
      if ((sv.type == JV_OBJ && sv.u.obj->kind == OBJ_ARRAY) || sv.type == JV_STR) {
        unsigned j, len = sv.type == JV_STR ? (unsigned)sv.u.str->len : sv.u.obj->elen;
        for (j = 0; j < len; j++) {
          js_value e = js_get_index(ctx, sv, j);
          if (argc == cap) {
            cap *= 2;
            argv = (js_value *)realloc(argv, (unsigned)cap * sizeof(js_value));
          }
          argv[argc++] = e;
          KEEP(e);
        }
      } else {
        js_value iter, e;
        if (!get_iterator(ctx, sv, &iter)) {
          free(argv);
          if (!ctx->has_exc)
            js_throw_type(ctx, "spread value is not iterable");
          return NULL;
        }
        KEEP(iter);
        while (iterator_step(ctx, iter, &e)) {
          if (argc == cap) {
            cap *= 2;
            argv = (js_value *)realloc(argv, (unsigned)cap * sizeof(js_value));
          }
          argv[argc++] = e;
          KEEP(e);
          if (ctx->has_exc)
            break;
        }
        if (ctx->has_exc) {
          free(argv);
          return NULL;
        }
      }
    } else {
      js_value v = evaluate(ctx, a);
      if (ctx->has_exc) {
        free(argv);
        return NULL;
      }
      if (argc == cap) {
        cap *= 2;
        argv = (js_value *)realloc(argv, (unsigned)cap * sizeof(js_value));
      }
      argv[argc++] = v;
      KEEP(v);
    }
  }
  *out_argc = argc;
  return argv;
}

/* Append the source-ish name of a callee AST to `buf` for a "<x> is not a
 * function" TypeError, so a failing call names the culprit instead of the
 * opaque "value". Bounded and best-effort: idents and dotted/indexed members
 * render, the rest degrade to a placeholder. */
static void callee_desc(js_node *callee, char *buf, int cap)
{
  int n = 0;
  buf[0] = '\0';
  if (!callee) {
    strlcpy(buf, "value", (unsigned)cap);
    return;
  }
  switch (callee->type) {
  case JS_IDENT:
  case JS_STR: {
    int l = callee->u.str.len;
    if (l > cap - 1)
      l = cap - 1;
    for (int i = 0; i < l; i++)
      buf[n++] = callee->u.str.s[i];
    buf[n] = '\0';
    break;
  }
  case JS_THIS:
    strlcpy(buf, "this", (unsigned)cap);
    break;
  case JS_MEMBER: {
    callee_desc(callee->u.member.obj, buf, cap);
    n = (int)strlen(buf);
    if (callee->u.member.computed) {
      if (n < cap - 3) {
        buf[n++] = '[';
        buf[n++] = ']';
        buf[n] = '\0';
      }
    } else if (n < cap - 2) {
      buf[n++] = '.';
      js_node *p = callee->u.member.prop;
      if (p && (p->type == JS_IDENT || p->type == JS_STR)) {
        int l = p->u.str.len;
        if (l > cap - 1 - n)
          l = cap - 1 - n;
        for (int i = 0; i < l; i++)
          buf[n++] = p->u.str.s[i];
      }
      buf[n] = '\0';
    }
    break;
  }
  case JS_CALL:
    callee_desc(callee->u.call.callee, buf, cap);
    n = (int)strlen(buf);
    if (n < cap - 3) {
      buf[n++] = '(';
      buf[n++] = ')';
      buf[n] = '\0';
    }
    break;
  default:
    strlcpy(buf, "value", (unsigned)cap);
    break;
  }
  if (!buf[0])
    strlcpy(buf, "value", (unsigned)cap);
}

static js_value eval_chain(js_ctx *ctx, js_node *n, int *shorted)
{
  if (n->type == JS_MEMBER) {
    js_value obj = eval_chain_obj(ctx, n->u.member.obj, shorted);
    if (ctx->has_exc || *shorted)
      return js_undef();
    if (n->u.member.optional && nullish(obj)) {
      *shorted = 1;
      return js_undef();
    }
    return access(ctx, obj, n);
  }
  /* JS_CALL */
  {
    js_node *callee = n->u.call.callee;
    js_value thisv = js_undef(), fnv;
    js_value *argv;
    int argc = 0;
    js_value result;
    char keybuf[64];
    keybuf[0] = '\0';

    if (callee->type == JS_MEMBER && callee->u.member.obj->type == JS_SUPER) {
      js_object *hp = super_home_proto(ctx); /* super.method() — this stays current this */
      if (!hp)
        return js_throw_type(ctx, "'super' keyword is unexpected here");
      thisv = js_env_get_this(ctx->cur_env);
      gc_push_scope();
      KEEP(thisv);
      fnv = access(ctx, js_obj_value(hp), callee);
      gc_pop_scope();
    } else if (callee->type == JS_MEMBER) {
      js_value obj = eval_chain_obj(ctx, callee->u.member.obj, shorted);
      if (ctx->has_exc || *shorted)
        return js_undef();
      if (callee->u.member.optional && nullish(obj)) {
        *shorted = 1;
        return js_undef();
      }
      thisv = obj;
      gc_push_scope();
      KEEP(obj);
      fnv = access_ex(ctx, obj, callee, keybuf, sizeof keybuf);
      gc_pop_scope();
    } else if (callee->type == JS_SUPER) { /* super(...) — call parent ctor on this */
      js_object *home = js_env_get_home(ctx->cur_env);
      if (!home || !home->proto)
        return js_throw_type(ctx, "'super' call is unexpected here");
      thisv = js_env_get_this(ctx->cur_env);
      fnv = js_get(ctx, js_obj_value(home->proto), "constructor");
    } else {
      fnv = eval_chain_obj(ctx, callee, shorted);
    }
    if (ctx->has_exc || *shorted)
      return js_undef();
    if (n->u.call.optional && nullish(fnv)) {
      *shorted = 1;
      return js_undef();
    }

    gc_push_scope();
    KEEP(thisv);
    KEEP(fnv);
    argv = eval_args(ctx, n->u.call.args, n->u.call.nargs, &argc);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    {
      int i;
      for (i = 0; i < argc; i++)
        KEEP(argv[i]);
    }
    if (!js_is_callable(fnv)) {
      char desc[96], msg[192];
      int m = 0;
      free(argv);
      gc_pop_scope();
      if (callee->type == JS_MEMBER && callee->u.member.computed && keybuf[0]) {
        /* runtime key recovered: render obj["key"] instead of obj[] */
        callee_desc(callee->u.member.obj, desc, sizeof desc);
        for (const char *q = desc; *q && m < (int)sizeof msg - 40; q++)
          msg[m++] = *q;
        msg[m++] = '[';
        msg[m++] = '"';
        for (const char *q = keybuf; *q && m < (int)sizeof msg - 24; q++)
          msg[m++] = *q;
        msg[m++] = '"';
        msg[m++] = ']';
      } else {
        callee_desc(callee, desc, sizeof desc);
        for (const char *q = desc; *q && m < (int)sizeof msg - 20; q++)
          msg[m++] = *q;
      }
      for (const char *q = " is not a function"; *q; q++)
        msg[m++] = *q;
      msg[m] = '\0';
      return js_throw_type(ctx, msg);
    }
    if (ctx->nframes > 0 && n->line > 0) { /* pin the caller frame to this call site */
      ctx->frame_line[ctx->nframes - 1] = n->line;
      ctx->frame_col[ctx->nframes - 1] = n->col;
    }
    result = call_function(ctx, fnv, thisv, argv, argc);
    free(argv);
    gc_pop_scope();
    return result;
  }
}

/* ------------------------------------------------------- assignment ------- */

static int compound_base_op(int op)
{
  switch (op) {
  case T_PLUS_EQ:
    return T_PLUS;
  case T_MINUS_EQ:
    return T_MINUS;
  case T_STAR_EQ:
    return T_STAR;
  case T_SLASH_EQ:
    return T_SLASH;
  case T_PERCENT_EQ:
    return T_PERCENT;
  case T_STARSTAR_EQ:
    return T_STARSTAR;
  case T_LTLT_EQ:
    return T_LTLT;
  case T_GTGT_EQ:
    return T_GTGT;
  case T_GTGTGT_EQ:
    return T_GTGTGT;
  case T_AMP_EQ:
    return T_AMP;
  case T_BAR_EQ:
    return T_BAR;
  case T_CARET_EQ:
    return T_CARET;
  default:
    return 0;
  }
}

static js_value eval_assign(js_ctx *ctx, js_node *n)
{
  int op = n->op;
  js_node *target = n->u.bin.left;

  if (op == T_EQ) {
    js_value rhs;
    if (target->type == JS_ARRAY_PATTERN || target->type == JS_OBJECT_PATTERN) {
      rhs = evaluate(ctx, n->u.bin.right);
      if (ctx->has_exc)
        return js_undef();
      gc_push_scope();
      KEEP(rhs);
      bind_pattern(ctx, target, rhs, 0, 0);
      gc_pop_scope();
      return rhs;
    }
    if (target->type == JS_IDENT) {
      rhs = evaluate(ctx, n->u.bin.right);
      if (ctx->has_exc)
        return js_undef();
      assign_ident(ctx, target, rhs);
      return rhs;
    }
    if (target->type == JS_MEMBER) {
      js_value obj = evaluate(ctx, target->u.member.obj);
      if (ctx->has_exc)
        return js_undef();
      gc_push_scope();
      KEEP(obj);
      if (target->u.member.computed) {
        js_value k = evaluate(ctx, target->u.member.prop);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        KEEP(k);
        rhs = evaluate(ctx, n->u.bin.right);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        KEEP(rhs);
        if (k.type == JV_NUM && k.u.num >= 0 && k.u.num == floor(k.u.num) && k.u.num < 4294967296.0)
          js_set_index(ctx, obj, (unsigned)k.u.num, rhs);
        else {
          js_value hold;
          const char *key = to_prop_key(ctx, k, &hold);
          if (!ctx->has_exc) {
            KEEP(hold);
            js_set(ctx, obj, key, rhs);
          }
        }
      } else {
        rhs = evaluate(ctx, n->u.bin.right);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        KEEP(rhs);
        js_set(ctx, obj, target->u.member.prop->u.str.s, rhs);
      }
      gc_pop_scope();
      return rhs;
    }
    return js_throw_type(ctx, "invalid assignment target");
  }

  /* logical assignment: &&=  ||=  ??= */
  if (op == T_AMPAMP_EQ || op == T_BARBAR_EQ || op == T_QQ_EQ) {
    js_value cur;
    int doit;
    if (target->type != JS_IDENT && target->type != JS_MEMBER)
      return js_throw_type(ctx, "invalid assignment target");
    if (target->type == JS_IDENT)
      cur = read_ident(ctx, target);
    else
      cur = access(ctx, evaluate(ctx, target->u.member.obj), target);
    if (ctx->has_exc)
      return js_undef();
    doit = op == T_AMPAMP_EQ ? js_to_boolean(cur) : op == T_BARBAR_EQ ? !js_to_boolean(cur) : nullish(cur);
    if (!doit)
      return cur;
    {
      js_value rhs = evaluate(ctx, n->u.bin.right);
      if (ctx->has_exc)
        return js_undef();
      if (target->type == JS_IDENT)
        assign_ident(ctx, target, rhs);
      else {
        js_value obj = evaluate(ctx, target->u.member.obj);
        if (ctx->has_exc)
          return js_undef();
        if (!target->u.member.computed)
          js_set(ctx, obj, target->u.member.prop->u.str.s, rhs);
        else {
          js_value kv = evaluate(ctx, target->u.member.prop), hold;
          if (!ctx->has_exc) {
            const char *key = to_prop_key(ctx, kv, &hold);
            if (!ctx->has_exc)
              js_set(ctx, obj, key, rhs);
          }
        }
      }
      return rhs;
    }
  }

  /* arithmetic compound assignment */
  {
    int base = compound_base_op(op);
    if (target->type == JS_IDENT) {
      js_value cur = read_ident(ctx, target);
      js_value rhs, res;
      if (ctx->has_exc)
        return js_undef();
      gc_push_scope();
      KEEP(cur);
      rhs = evaluate(ctx, n->u.bin.right);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      KEEP(rhs);
      res = apply_binop(ctx, base, cur, rhs);
      gc_pop_scope();
      if (ctx->has_exc)
        return js_undef();
      assign_ident(ctx, target, res);
      return res;
    }
    if (target->type == JS_MEMBER) {
      js_value obj = evaluate(ctx, target->u.member.obj);
      js_value res;
      const char *key;
      js_value cur, rhs;
      if (ctx->has_exc)
        return js_undef();
      gc_push_scope();
      KEEP(obj);
      if (target->u.member.computed) {
        js_value k = evaluate(ctx, target->u.member.prop), hold;
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        key = to_prop_key(ctx, k, &hold);
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        KEEP(hold);
      } else
        key = target->u.member.prop->u.str.s;
      cur = js_get(ctx, obj, key);
      KEEP(cur);
      rhs = evaluate(ctx, n->u.bin.right);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      KEEP(rhs);
      res = apply_binop(ctx, base, cur, rhs);
      if (!ctx->has_exc)
        js_set(ctx, obj, key, res);
      gc_pop_scope();
      return res;
    }
    return js_throw_type(ctx, "invalid assignment target");
  }
}

/* ------------------------------------------------------- update (++/--) --- */

/* Step a value by ±1 for ++/--. `*oldout` receives the ToNumeric'd operand (a
 * BigInt stays a BigInt, everything else becomes a Number), the return is the
 * stepped value. Sets ctx->has_exc on a BigInt size-cap / conversion failure.
 */
static js_value inc_dec(js_ctx *ctx, js_value cur, int inc, js_value *oldout)
{
  if (cur.type == JV_BIGINT) {
    bigint *one = bi_from_i64(1), *nv;
    *oldout = cur;
    if (!one) {
      js_throw_range(ctx, "out of memory");
      return js_undef();
    }
    nv = inc ? bi_add(js_bigint_of(cur), one) : bi_sub(js_bigint_of(cur), one);
    bi_free(one);
    return js_mk_bigint(ctx, nv); /* NULL nv -> throws inside */
  }
  {
    double o = js_to_number(ctx, cur);
    *oldout = js_number(o);
    return js_number(inc ? o + 1 : o - 1);
  }
}

static js_value eval_update(js_ctx *ctx, js_node *n)
{
  js_node *t = n->u.unary.operand;
  js_value oldv, newv;
  int inc = n->op == T_PLUSPLUS;
  if (t->type == JS_IDENT) {
    js_value cur = read_ident(ctx, t);
    if (ctx->has_exc)
      return js_undef();
    gc_push_scope();
    KEEP(cur);
    newv = inc_dec(ctx, cur, inc, &oldv);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(newv);
    KEEP(oldv);
    assign_ident(ctx, t, newv);
    gc_pop_scope();
    return n->u.unary.prefix ? newv : oldv;
  }
  if (t->type == JS_MEMBER) {
    js_value obj = evaluate(ctx, t->u.member.obj);
    const char *key;
    char keybuf[DTOA_MAX];
    js_value cur;
    if (ctx->has_exc)
      return js_undef();
    gc_push_scope();
    KEEP(obj);
    if (t->u.member.computed) {
      js_value ks = js_to_string(ctx, evaluate(ctx, t->u.member.prop));
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      {
        int n2 = ks.u.str->len < DTOA_MAX - 1 ? ks.u.str->len : DTOA_MAX - 1;
        memcpy(keybuf, ks.u.str->data, (unsigned)n2);
        keybuf[n2] = 0;
        key = keybuf;
      }
    } else
      key = t->u.member.prop->u.str.s;
    cur = js_get(ctx, obj, key);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(cur);
    newv = inc_dec(ctx, cur, inc, &oldv);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(newv);
    KEEP(oldv);
    js_set(ctx, obj, key, newv);
    gc_pop_scope();
    return n->u.unary.prefix ? newv : oldv;
  }
  return js_throw_type(ctx, "invalid update target");
}

/* ------------------------------------------------------- literals --------- */

static js_value build_array(js_ctx *ctx, js_node *n)
{
  js_object *arr;
  int i;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < n->u.list.count; i++) {
    js_node *el = n->u.list.items[i];
    if (!el) {
      js_array_set_len(ctx, arr, arr->elen + 1);
      continue;
    } /* hole */
    if (el->type == JS_SPREAD) {
      js_value sv = evaluate(ctx, el->u.one.arg);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      KEEP(sv);
      if ((sv.type == JV_OBJ && sv.u.obj->kind == OBJ_ARRAY) || sv.type == JV_STR) {
        unsigned j, len = sv.type == JV_STR ? (unsigned)sv.u.str->len : sv.u.obj->elen;
        for (j = 0; j < len; j++)
          js_array_push(ctx, arr, js_get_index(ctx, sv, j));
      } else {
        js_value iter, e;
        if (!get_iterator(ctx, sv, &iter)) {
          gc_pop_scope();
          if (!ctx->has_exc)
            js_throw_type(ctx, "spread value is not iterable");
          return js_undef();
        }
        KEEP(iter);
        while (iterator_step(ctx, iter, &e)) {
          js_array_push(ctx, arr, e);
          if (ctx->has_exc)
            break;
        }
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
      }
    } else {
      js_value v = evaluate(ctx, el);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      js_array_push(ctx, arr, v);
    }
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

static js_value prop_key_value(js_ctx *ctx, js_node *keynode, int computed, char *buf, int bufsz, const char **out)
{
  if (computed) {
    js_value k = evaluate(ctx, keynode);
    js_value ks;
    if (ctx->has_exc) {
      *out = "";
      return js_undef();
    }
    if (k.type == JV_SYMBOL) {
      *out = js_symbol_key(k);
      return k;
    }
    ks = js_to_string(ctx, k);
    if (ctx->has_exc) {
      *out = "";
      return js_undef();
    }
    *out = ks.u.str->data;
    return ks;
  }
  (void)bufsz;
  if (keynode->type == JS_NUM) {
    dtoa(keynode->u.num, buf);
    *out = buf;
    return js_undef();
  }
  if (keynode->type == JS_STR) {
    *out = keynode->u.str.s;
    return js_undef();
  }
  *out = keynode->u.str.s; /* JS_IDENT name */
  return js_undef();
}

static js_value build_object(js_ctx *ctx, js_node *n)
{
  js_object *obj;
  int i;
  gc_push_scope();
  obj = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  for (i = 0; i < n->u.object.count; i++) {
    js_prop *pr = &n->u.object.props[i];
    char buf[DTOA_MAX];
    const char *key;
    js_value keyhold;
    if (pr->kind == JS_PROP_SPREAD) {
      js_value src = evaluate(ctx, pr->value);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      if (src.type == JV_OBJ) {
        js_object *s = src.u.obj;
        int k;
        if (s->kind == OBJ_ARRAY) {
          unsigned e;
          for (e = 0; e < s->elen; e++)
            js_set_index(ctx, js_obj_value(obj), e, s->elems[e]);
        }
        for (k = 0; k < s->props.n; k++)
          if (s->props.e[k].flags & P_ENUMERABLE)
            js_set(ctx, js_obj_value(obj), s->props.e[k].key, js_get(ctx, src, s->props.e[k].key));
      }
      continue;
    }
    keyhold = prop_key_value(ctx, pr->key, pr->computed, buf, (int)sizeof buf, &key);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(keyhold);
    if (pr->kind == JS_PROP_GET || pr->kind == JS_PROP_SET) {
      js_value fn = evaluate(ctx, pr->value);
      pentry *pe;
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      pe = pmap_find_pub(obj, key);
      if (pe && (pe->flags & P_ACCESSOR)) {
        if (pr->kind == JS_PROP_GET)
          pe->getter = fn.u.obj;
        else
          pe->setter = fn.u.obj;
        gc_write_barrier(obj);
      } else {
        js_def_accessor(obj, key, pr->kind == JS_PROP_GET ? fn.u.obj : NULL, pr->kind == JS_PROP_SET ? fn.u.obj : NULL,
                        P_ENUMERABLE | P_CONFIGURABLE);
      }
    } else {
      js_value v = evaluate(ctx, pr->value);
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      js_def(obj, key, v, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    }
  }
  gc_pop_scope();
  return js_obj_value(obj);
}

static js_value build_template(js_ctx *ctx, js_node *n)
{
  js_value acc;
  int i;
  gc_push_scope();
  acc.type = JV_STR;
  acc.u.str = js_mk_string(ctx, n->u.tmpl.quasis[0]->u.str.s, n->u.tmpl.quasis[0]->u.str.len).u.str;
  gc_keep(acc.u.str);
  for (i = 0; i < n->u.tmpl.nexprs; i++) {
    js_value ev = evaluate(ctx, n->u.tmpl.exprs[i]);
    js_value es, q;
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(ev);
    es = js_to_string(ctx, ev);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(es);
    acc.u.str = js_str_concat(ctx, acc.u.str, es.u.str);
    gc_keep(acc.u.str);
    q = js_mk_string(ctx, n->u.tmpl.quasis[i + 1]->u.str.s, n->u.tmpl.quasis[i + 1]->u.str.len);
    gc_keep(q.u.str);
    acc.u.str = js_str_concat(ctx, acc.u.str, q.u.str);
    gc_keep(acc.u.str);
  }
  gc_pop_scope();
  return acc;
}

static js_value build_tagged(js_ctx *ctx, js_node *n)
{
  js_node *tmpl = n->u.bin.right;
  js_value tag, strings, result;
  js_value *argv;
  int nargs = 1 + tmpl->u.tmpl.nexprs, i;
  gc_push_scope();
  tag = evaluate(ctx, n->u.bin.left);
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  KEEP(tag);
  strings = js_obj_value(js_new_array(ctx, 0));
  KEEP(strings);
  for (i = 0; i < tmpl->u.tmpl.nquasis; i++)
    js_array_push(ctx, strings.u.obj,
                  js_mk_string(ctx, tmpl->u.tmpl.quasis[i]->u.str.s, tmpl->u.tmpl.quasis[i]->u.str.len));
  js_set(ctx, strings, "raw", strings); /* approximate: raw == cooked     */
  argv = (js_value *)malloc((unsigned)nargs * sizeof(js_value));
  argv[0] = strings;
  for (i = 0; i < tmpl->u.tmpl.nexprs; i++) {
    argv[i + 1] = evaluate(ctx, tmpl->u.tmpl.exprs[i]);
    if (ctx->has_exc) {
      free(argv);
      gc_pop_scope();
      return js_undef();
    }
    KEEP(argv[i + 1]);
  }
  if (!js_is_callable(tag)) {
    free(argv);
    gc_pop_scope();
    return js_throw_type(ctx, "tag is not a function");
  }
  result = call_function(ctx, tag, js_undef(), argv, nargs);
  free(argv);
  gc_pop_scope();
  return result;
}

/* ------------------------------------------------------- classes ---------- */

/* the prototype `super` resolves against in the current method: home->proto */
static js_object *super_home_proto(js_ctx *ctx)
{
  js_object *h = js_env_get_home(ctx->cur_env);
  return h ? h->proto : NULL;
}

static js_value eval_class(js_ctx *ctx, js_node *node)
{
  js_object *superctor = NULL, *superproto = NULL, *proto, *ctor;
  js_node *ctor_node = NULL;
  int i;

  if (node->u.cls.super) {
    js_value sv = evaluate(ctx, node->u.cls.super);
    if (ctx->has_exc)
      return js_undef();
    if (!js_is_callable(sv))
      return js_throw_type(ctx, "class extends value is not a constructor");
    superctor = sv.u.obj;
    {
      js_value sp = js_get(ctx, sv, "prototype");
      superproto = sp.type == JV_OBJ ? sp.u.obj : NULL;
    }
  }

  gc_push_scope();
  if (superctor)
    gc_keep(superctor);
  proto = (js_object *)gc_keep(js_new_object(ctx, superproto ? superproto : ctx->object_proto));

  for (i = 0; i < node->u.cls.nmembers; i++)
    if (node->u.cls.members[i].kind == JS_M_CTOR) {
      ctor_node = node->u.cls.members[i].value;
      break;
    }

  ctor = (js_object *)gc_keep(js_new_function(ctx, ctor_node, ctx->cur_env));
  ctor->fn->class_node = node;
  ctor->fn->home = proto;
  gc_write_barrier(ctor);
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  if (superctor) {
    ctor->proto = superctor;
    gc_write_barrier(ctor);
  } /* static inheritance */
  if (node->u.cls.id)
    js_def(ctor, "name", js_mk_cstring(ctx, node->u.cls.id->u.str.s), P_CONFIGURABLE);

  for (i = 0; i < node->u.cls.nmembers; i++) {
    js_member *m = &node->u.cls.members[i];
    js_object *target = m->is_static ? ctor : proto;
    char buf[DTOA_MAX];
    const char *key;
    js_value kh;
    if (m->kind == JS_M_CTOR)
      continue;
    if (m->kind == JS_M_STATIC_BLOCK) { /* run `static { … }` with this = the class */
      js_object *fn = js_new_function(ctx, m->value, ctx->cur_env);
      gc_push_scope();
      gc_keep(fn);
      fn->fn->home = ctor;
      gc_write_barrier(fn);
      js_call(ctx, js_obj_value(fn), js_obj_value(ctor), 0, 0);
      gc_pop_scope();
      if (ctx->has_exc) {
        gc_pop_scope();
        return js_undef();
      }
      continue;
    }
    kh = prop_key_value(ctx, m->key, m->computed, buf, (int)sizeof buf, &key);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(kh);
    if (m->kind == JS_M_FIELD) {
      if (m->is_static) {
        js_value v = m->value ? evaluate(ctx, m->value) : js_undef();
        if (ctx->has_exc) {
          gc_pop_scope();
          return js_undef();
        }
        js_def(ctor, key, v, P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
      }
      continue; /* instance fields: per-construction */
    }
    {
      js_object *fn = js_new_function(ctx, m->value, ctx->cur_env);
      gc_keep(fn);
      fn->fn->home = target;
      gc_write_barrier(fn);
      if (m->kind == JS_M_GET || m->kind == JS_M_SET) {
        pentry *pe = pmap_find_pub(target, key);
        if (pe && (pe->flags & P_ACCESSOR)) {
          if (m->kind == JS_M_GET)
            pe->getter = fn;
          else
            pe->setter = fn;
          gc_write_barrier(target);
        } else {
          js_def_accessor(target, key, m->kind == JS_M_GET ? fn : NULL, m->kind == JS_M_SET ? fn : NULL,
                          P_CONFIGURABLE);
        }
      } else {
        js_def(target, key, js_obj_value(fn), P_WRITABLE | P_CONFIGURABLE); /* non-enumerable */
      }
    }
  }
  gc_pop_scope();
  return js_obj_value(ctor);
}

/* ------------------------------------------------------- evaluate --------- */

static js_value evaluate(js_ctx *ctx, js_node *n)
{
  char probe;
  if (ctx->has_exc)
    return js_undef();
  if (stack_overflow(ctx, &probe))
    return js_throw_range(ctx, "maximum call stack size exceeded");
  if (n->line > 0) {
    ctx->cur_line = n->line;
    ctx->cur_col = n->col;
  } /* throw-site location for stack traces */
  switch (n->type) {
  case JS_NUM:
    return js_number(n->u.num);
  case JS_BIGINT: {
    int ok = 0;
    bigint *b = bi_from_str(n->u.str.s, n->u.str.len, &ok);
    if (!b || !ok) {
      bi_free(b);
      return js_throw_range(ctx, "invalid BigInt literal");
    }
    return js_mk_bigint(ctx, b);
  }
  case JS_STR:
    return js_mk_string(ctx, n->u.str.s, n->u.str.len);
  case JS_BOOL:
    return js_bool(n->u.num != 0);
  case JS_NULL:
    return js_null();
  case JS_THIS:
    return js_env_get_this(ctx->cur_env);
  case JS_IDENT:
    if (n->u.str.len == 10 && strcmp(n->u.str.s, "new.target") == 0)
      return js_undef();
    return read_ident(ctx, n);
  case JS_TEMPLATE:
    return build_template(ctx, n);
  case JS_TAGGED_TEMPLATE:
    return build_tagged(ctx, n);
  case JS_ARRAY:
    return build_array(ctx, n);
  case JS_OBJECT:
    return build_object(ctx, n);
  case JS_FUNC:
    /* A named function expression binds its own name in a dedicated scope
     * that wraps its closure, so the body can recurse by name (the staple of
     * UMD/browserify bundles) without leaking the name to the enclosing
     * scope. Arrows have no name and skip this. */
    if (n->u.func.name && !n->u.func.is_arrow) {
      js_object *fn;
      js_env *fenv;
      gc_push_scope();
      fenv = (js_env *)gc_keep(js_new_env(ctx, ctx->cur_env, 0));
      fn = (js_object *)gc_keep(js_new_function(ctx, n, fenv));
      js_env_define(fenv, n->u.func.name, js_obj_value(fn), P_WRITABLE);
      gc_pop_scope();
      return js_obj_value(fn);
    }
    return js_obj_value(js_new_function(ctx, n, ctx->cur_env));
  case JS_IMPORT_META: {
    js_object *o = js_new_object(ctx, ctx->object_proto);
    gc_push_scope();
    gc_keep(o);
    js_def(o, "url", js_mk_cstring(ctx, ctx->cur_module_url ? ctx->cur_module_url : ""), P_ENUMERABLE | P_WRITABLE);
    gc_pop_scope();
    return js_obj_value(o);
  }
  case JS_IMPORT_CALL: { /* dynamic import(spec) -> Promise */
    js_value spec = evaluate(ctx, n->u.one.arg), specstr, r;
    js_object *ns;
    if (ctx->has_exc)
      return js_undef();
    gc_push_scope();
    KEEP(spec);
    specstr = js_to_string(ctx, spec);
    ns = (ctx->load_module && specstr.type == JV_STR) ? ctx->load_module(ctx, specstr.u.str->data, ctx->load_module_ud)
                                                      : NULL;
    if (ctx->has_exc) {
      js_value e = ctx->exc;
      ctx->has_exc = 0;
      ctx->exc_root = NULL;
      KEEP(e);
      r = js_promise_rejected(ctx, e);
    } else if (ns) {
      r = js_promise_resolved(ctx, js_obj_value(ns));
    } else {
      r = js_promise_rejected(ctx, js_mk_cstring(ctx, "module load failed"));
    }
    gc_pop_scope();
    return r;
  }
  case JS_SEQUENCE: {
    js_value v = js_undef();
    int i;
    for (i = 0; i < n->u.list.count; i++) {
      v = evaluate(ctx, n->u.list.items[i]);
      if (ctx->has_exc)
        return js_undef();
    }
    return v;
  }
  case JS_COND:
    return js_to_boolean(evaluate(ctx, n->u.iff.test)) ? evaluate(ctx, n->u.iff.cons) : evaluate(ctx, n->u.iff.alt);
  case JS_LOGICAL: {
    js_value l = evaluate(ctx, n->u.bin.left);
    if (ctx->has_exc)
      return js_undef();
    if (n->op == T_AMPAMP)
      return js_to_boolean(l) ? evaluate(ctx, n->u.bin.right) : l;
    if (n->op == T_BARBAR)
      return js_to_boolean(l) ? l : evaluate(ctx, n->u.bin.right);
    return nullish(l) ? evaluate(ctx, n->u.bin.right) : l; /* ?? */
  }
  case JS_BINARY: {
    js_value l = evaluate(ctx, n->u.bin.left), r, res;
    if (ctx->has_exc)
      return js_undef();
    gc_push_scope();
    KEEP(l);
    r = evaluate(ctx, n->u.bin.right);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    KEEP(r);
    res = apply_binop(ctx, n->op, l, r);
    gc_pop_scope();
    return res;
  }
  case JS_UNARY: {
    js_node *opd = n->u.unary.operand;
    if (n->op == T_TYPEOF) {
      if (opd->type == JS_IDENT && !js_env_lookup(ctx->cur_env, opd->u.str.s) && !has_global(ctx, opd->u.str.s))
        return js_mk_cstring(ctx, "undefined");
      {
        js_value v = evaluate(ctx, opd);
        if (ctx->has_exc)
          return js_undef();
        return js_mk_cstring(ctx, js_typeof(v));
      }
    }
    if (n->op == T_DELETE) {
      if (opd->type == JS_MEMBER) {
        js_value obj = evaluate(ctx, opd->u.member.obj);
        if (ctx->has_exc)
          return js_undef();
        if (obj.type == JV_OBJ) {
          int prox = obj.u.obj->kind == OBJ_PROXY;
          if (opd->u.member.computed) {
            js_value kv = evaluate(ctx, opd->u.member.prop), hold;
            if (!ctx->has_exc) {
              const char *key = to_prop_key(ctx, kv, &hold);
              if (!ctx->has_exc) {
                if (prox)
                  js_proxy_delete(ctx, obj.u.obj, key);
                else
                  js_delete(obj.u.obj, key);
              }
            }
          } else if (prox)
            js_proxy_delete(ctx, obj.u.obj, opd->u.member.prop->u.str.s);
          else
            js_delete(obj.u.obj, opd->u.member.prop->u.str.s);
        }
      }
      return js_bool(1);
    }
    if (n->op == T_VOID) {
      evaluate(ctx, opd);
      return js_undef();
    }
    {
      js_value v = evaluate(ctx, opd);
      if (ctx->has_exc)
        return js_undef();
      switch (n->op) {
      case T_BANG:
        return js_bool(!js_to_boolean(v));
      case T_TILDE:
        if (v.type == JV_BIGINT)
          return js_mk_bigint(ctx, bi_not(js_bigint_of(v)));
        return js_number(~js_to_int32(ctx, v));
      case T_PLUS:
        if (v.type == JV_BIGINT)
          return js_throw_type(ctx, "Cannot convert a BigInt value to a number");
        return js_number(js_to_number(ctx, v));
      case T_MINUS:
        if (v.type == JV_BIGINT)
          return js_mk_bigint(ctx, bi_neg(js_bigint_of(v)));
        return js_number(-js_to_number(ctx, v));
      default:
        return js_undef();
      }
    }
  }
  case JS_UPDATE:
    return eval_update(ctx, n);
  case JS_ASSIGN:
    return eval_assign(ctx, n);
  case JS_MEMBER:
  case JS_CALL: {
    int sh = 0;
    return eval_chain(ctx, n, &sh);
  }
  case JS_NEW: {
    js_value fnv = evaluate(ctx, n->u.call.callee);
    js_value *argv;
    int argc = 0;
    js_value result;
    if (ctx->has_exc)
      return js_undef();
    gc_push_scope();
    KEEP(fnv);
    argv = eval_args(ctx, n->u.call.args, n->u.call.nargs, &argc);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
    {
      int i;
      for (i = 0; i < argc; i++)
        KEEP(argv[i]);
    }
    if (ctx->nframes > 0 && n->line > 0) { /* pin the caller frame to this `new` site */
      ctx->frame_line[ctx->nframes - 1] = n->line;
      ctx->frame_col[ctx->nframes - 1] = n->col;
    }
    result = js_construct(ctx, fnv, argv, argc);
    free(argv);
    gc_pop_scope();
    return result;
  }
  case JS_REGEX:
    return js_make_regexp(ctx, n->u.regex.pattern, n->u.regex.flags);
  case JS_AWAIT: {
    js_value v = evaluate(ctx, n->u.one.arg), settled;
    int st;
    if (ctx->has_exc)
      return js_undef();
    st = js_promise_inspect(ctx, v, &settled);
    if (st == -1)
      return v; /* await of a non-promise -> the value */
    if (st == 1)
      return settled; /* fulfilled -> its value              */
    if (st == 2) {
      js_throw(ctx, settled);
      return js_undef();
    } /* rejected -> throw */
    return js_undef(); /* pending (no event loop) -> undefined */
  }
  case JS_YIELD:
    return js_throw_type(ctx, "yield needs generator runtime (not yet implemented)");
  case JS_CLASS:
    return eval_class(ctx, n);
  case JS_SUPER: {
    js_object *hp = super_home_proto(ctx); /* `super.x` reads off home->proto */
    if (!hp)
      return js_throw_type(ctx, "'super' keyword is unexpected here");
    return js_obj_value(hp);
  }
  default:
    return js_throw_type(ctx, "cannot evaluate this expression");
  }
}

/* ------------------------------------------------------- patterns --------- */

static void bind_pattern(js_ctx *ctx, js_node *pat, js_value val, int declare, unsigned char flags)
{
  if (ctx->has_exc)
    return;
  switch (pat->type) {
  case JS_IDENT:
    if (declare)
      js_env_define(ctx->cur_env, pat->u.str.s, val, flags);
    else
      assign_ident(ctx, pat, val);
    return;
  case JS_MEMBER: {
    js_value obj = evaluate(ctx, pat->u.member.obj);
    if (ctx->has_exc)
      return;
    if (pat->u.member.computed) {
      js_value k = js_to_string(ctx, evaluate(ctx, pat->u.member.prop));
      if (!ctx->has_exc)
        js_set(ctx, obj, k.u.str->data, val);
    } else
      js_set(ctx, obj, pat->u.member.prop->u.str.s, val);
    return;
  }
  case JS_ASSIGN_PATTERN: {
    js_value v = val;
    if (v.type == JV_UNDEF) {
      v = evaluate(ctx, pat->u.assignpat.init);
      if (ctx->has_exc)
        return;
    }
    bind_pattern(ctx, pat->u.assignpat.id, v, declare, flags);
    return;
  }
  case JS_ARRAY_PATTERN: {
    int i;
    unsigned idx = 0;
    gc_push_scope();
    KEEP(val);
    for (i = 0; i < pat->u.list.count; i++) {
      js_node *el = pat->u.list.items[i];
      if (!el) {
        idx++;
        continue;
      }
      if (el->type == JS_REST) {
        js_object *rest = js_new_array(ctx, 0);
        gc_keep(rest);
        if (val.type == JV_OBJ && val.u.obj->kind == OBJ_ARRAY)
          for (; idx < val.u.obj->elen; idx++)
            js_array_push(ctx, rest, val.u.obj->elems[idx]);
        else if (val.type == JV_STR)
          for (; (int)idx < val.u.str->len; idx++)
            js_array_push(ctx, rest, js_get_index(ctx, val, idx));
        bind_pattern(ctx, el->u.one.arg, js_obj_value(rest), declare, flags);
        break;
      }
      {
        js_value ev = js_get_index(ctx, val, idx++);
        bind_pattern(ctx, el, ev, declare, flags);
      }
      if (ctx->has_exc)
        break;
    }
    gc_pop_scope();
    return;
  }
  case JS_OBJECT_PATTERN: {
    int i;
    gc_push_scope();
    KEEP(val);
    for (i = 0; i < pat->u.object.count; i++) {
      js_prop *pr = &pat->u.object.props[i];
      if (pr->kind == JS_PROP_SPREAD) {
        js_object *ro = js_new_object(ctx, ctx->object_proto);
        int k;
        gc_keep(ro);
        if (val.type == JV_OBJ) {
          js_object *s = val.u.obj;
          for (k = 0; k < s->props.n; k++) {
            int used = 0, j;
            if (!(s->props.e[k].flags & P_ENUMERABLE))
              continue;
            for (j = 0; j < i; j++)
              if (pat->u.object.props[j].kind != JS_PROP_SPREAD && !pat->u.object.props[j].computed &&
                  strcmp(pat->u.object.props[j].key->u.str.s, s->props.e[k].key) == 0) {
                used = 1;
                break;
              }
            if (!used)
              js_set(ctx, js_obj_value(ro), s->props.e[k].key, js_get(ctx, val, s->props.e[k].key));
          }
        }
        bind_pattern(ctx, pr->value, js_obj_value(ro), declare, flags);
        break;
      }
      {
        char buf[DTOA_MAX];
        const char *key;
        js_value kh = prop_key_value(ctx, pr->key, pr->computed, buf, (int)sizeof buf, &key);
        js_value pv;
        if (ctx->has_exc)
          break;
        KEEP(kh);
        pv = js_get(ctx, val, key);
        bind_pattern(ctx, pr->value, pv, declare, flags);
      }
      if (ctx->has_exc)
        break;
    }
    gc_pop_scope();
    return;
  }
  default:
    js_throw_type(ctx, "invalid binding pattern");
  }
}

/* ------------------------------------------------------- hoisting --------- */

static void hoist_vars(js_ctx *ctx, js_env *fnenv, js_node *n);

static void declare_pattern_vars(js_env *fnenv, js_node *pat)
{
  switch (pat->type) {
  case JS_IDENT:
    if (!pmap_find_in_env(fnenv, pat->u.str.s))
      js_env_define(fnenv, pat->u.str.s, js_undef(), P_WRITABLE | P_ENUMERABLE);
    return;
  case JS_ASSIGN_PATTERN:
    declare_pattern_vars(fnenv, pat->u.assignpat.id);
    return;
  case JS_REST:
    declare_pattern_vars(fnenv, pat->u.one.arg);
    return;
  case JS_ARRAY_PATTERN: {
    int i;
    for (i = 0; i < pat->u.list.count; i++)
      if (pat->u.list.items[i])
        declare_pattern_vars(fnenv, pat->u.list.items[i]);
    return;
  }
  case JS_OBJECT_PATTERN: {
    int i;
    for (i = 0; i < pat->u.object.count; i++)
      declare_pattern_vars(fnenv, pat->u.object.props[i].value);
    return;
  }
  default:
    return;
  }
}

static void hoist_vars(js_ctx *ctx, js_env *fnenv, js_node *n)
{
  if (!n)
    return;
  switch (n->type) {
  case JS_VAR_DECL:
    if (n->u.var.kind == JS_DECL_VAR) {
      int i;
      for (i = 0; i < n->u.var.count; i++)
        declare_pattern_vars(fnenv, n->u.var.decls[i].id);
    }
    return;
  case JS_BLOCK: {
    int i;
    for (i = 0; i < n->u.list.count; i++)
      hoist_vars(ctx, fnenv, n->u.list.items[i]);
    return;
  }
  case JS_IF:
    hoist_vars(ctx, fnenv, n->u.iff.cons);
    hoist_vars(ctx, fnenv, n->u.iff.alt);
    return;
  case JS_FOR:
    hoist_vars(ctx, fnenv, n->u.forc.init);
    hoist_vars(ctx, fnenv, n->u.forc.body);
    return;
  case JS_FOR_IN:
    hoist_vars(ctx, fnenv, n->u.forin.left);
    hoist_vars(ctx, fnenv, n->u.forin.body);
    return;
  case JS_WHILE:
  case JS_DO_WHILE:
    hoist_vars(ctx, fnenv, n->u.loop.body);
    return;
  case JS_TRY:
    hoist_vars(ctx, fnenv, n->u.tc.block);
    hoist_vars(ctx, fnenv, n->u.tc.handler);
    hoist_vars(ctx, fnenv, n->u.tc.finalizer);
    return;
  case JS_SWITCH: {
    int i, j;
    for (i = 0; i < n->u.sw.count; i++)
      for (j = 0; j < n->u.sw.cases[i].nbody; j++)
        hoist_vars(ctx, fnenv, n->u.sw.cases[i].body[j]);
    return;
  }
  case JS_LABELED:
    hoist_vars(ctx, fnenv, n->u.labeled.body);
    return;
  case JS_EXPORT:
    hoist_vars(ctx, fnenv, n->u.exp.decl);
    return; /* `export var x` */
  default:
    return; /* don't descend into nested functions */
  }
}

/* bind function declarations in the current scope (block-level) */
static void hoist_functions(js_ctx *ctx, js_node **items, int count)
{
  int i;
  for (i = 0; i < count; i++) {
    js_node *s = items[i];
    if (s->type == JS_EXPORT && s->u.exp.decl)
      s = s->u.exp.decl; /* `export function f` */
    if (s->type == JS_FUNC && s->u.func.name)
      js_env_define(ctx->cur_env, s->u.func.name, js_obj_value(js_new_function(ctx, s, ctx->cur_env)),
                    P_WRITABLE | P_ENUMERABLE);
  }
}

/* Annex B (sloppy-mode) legacy function hoisting: a function declared inside a
 * block also gets a var-scoped binding of the same name in the enclosing
 * function/program scope. We pre-declare those names (undefined) here; the
 * block-level declaration copies its value up when execution reaches it (see
 * the JS_FUNC case in exec_stmt). Names already bound at function scope (e.g. a
 * `var` or a top-level function of the same name) are left untouched. This
 * engine has no strict mode, so the rule is applied uniformly. */
static void hoist_block_funcs(js_ctx *ctx, js_env *fnenv, js_node *n)
{
  if (!n)
    return;
  switch (n->type) {
  case JS_FUNC:
    if (n->u.func.name && !pmap_find_in_env(fnenv, n->u.func.name))
      js_env_define(fnenv, n->u.func.name, js_undef(), P_WRITABLE | P_ENUMERABLE);
    return;
  case JS_BLOCK: {
    int i;
    for (i = 0; i < n->u.list.count; i++)
      hoist_block_funcs(ctx, fnenv, n->u.list.items[i]);
    return;
  }
  case JS_IF:
    hoist_block_funcs(ctx, fnenv, n->u.iff.cons);
    hoist_block_funcs(ctx, fnenv, n->u.iff.alt);
    return;
  case JS_FOR:
    hoist_block_funcs(ctx, fnenv, n->u.forc.body);
    return;
  case JS_FOR_IN:
    hoist_block_funcs(ctx, fnenv, n->u.forin.body);
    return;
  case JS_WHILE:
  case JS_DO_WHILE:
    hoist_block_funcs(ctx, fnenv, n->u.loop.body);
    return;
  case JS_TRY:
    hoist_block_funcs(ctx, fnenv, n->u.tc.block);
    hoist_block_funcs(ctx, fnenv, n->u.tc.handler);
    hoist_block_funcs(ctx, fnenv, n->u.tc.finalizer);
    return;
  case JS_SWITCH: {
    int i, j;
    for (i = 0; i < n->u.sw.count; i++)
      for (j = 0; j < n->u.sw.cases[i].nbody; j++)
        hoist_block_funcs(ctx, fnenv, n->u.sw.cases[i].body[j]);
    return;
  }
  case JS_LABELED:
    hoist_block_funcs(ctx, fnenv, n->u.labeled.body);
    return;
  case JS_EXPORT:
    hoist_block_funcs(ctx, fnenv, n->u.exp.decl);
    return;
  default:
    return; /* don't descend into nested functions */
  }
}

/* ------------------------------------------------------- statements ------- */

static completion exec_var_decl(js_ctx *ctx, js_node *n)
{
  int i;
  /* `var` names are hoisted to the function scope already, so a `var`
   * initializer must ASSIGN to that hoisted binding (declare=0) — not define a
   * fresh one in the current (possibly block/for) scope, which would leave the
   * function-scope var untouched once the inner scope pops. `let`/`const` are
   * block-scoped, so they declare in the current scope. */
  int is_var = n->u.var.kind == JS_DECL_VAR;
  unsigned char flags = n->u.var.kind == JS_DECL_CONST ? P_ENUMERABLE : (P_WRITABLE | P_ENUMERABLE);
  for (i = 0; i < n->u.var.count; i++) {
    js_declor *d = &n->u.var.decls[i];
    js_value init = js_undef();
    if (d->init) {
      init = evaluate(ctx, d->init);
      if (ctx->has_exc)
        return cnorm();
    } else if (is_var) {
      continue; /* no initializer: keep the hoisted binding's value */
    }
    gc_push_scope();
    KEEP(init);
    bind_pattern(ctx, d->id, init, is_var ? 0 : 1, flags);
    gc_pop_scope();
    if (ctx->has_exc)
      return cnorm();
  }
  return cnorm();
}

static completion exec_block(js_ctx *ctx, js_node **items, int count)
{
  js_env *saved = ctx->cur_env;
  completion c;
  gc_push_scope();
  gc_keep(saved);
  ctx->cur_env = js_new_env(ctx, saved, 0);
  hoist_functions(ctx, items, count);
  c = exec_list(ctx, items, count);
  ctx->cur_env = saved;
  gc_pop_scope();
  return c;
}

static completion exec_loop_body(js_ctx *ctx, js_node *body, const char *label, int *brk)
{
  completion c = exec_stmt(ctx, body, NULL);
  *brk = 0;
  if (ctx->has_exc)
    return c;
  if (c.kind == C_BREAK) {
    if (!c.label || (label && strcmp(c.label, label) == 0)) {
      *brk = 1;
      return cnorm();
    }
    return c;
  }
  if (c.kind == C_CONTINUE) {
    if (!c.label || (label && strcmp(c.label, label) == 0))
      return cnorm();
    return c;
  }
  if (c.kind == C_RETURN)
    return c;
  return cnorm();
}

static completion exec_for(js_ctx *ctx, js_node *n, const char *label)
{
  js_env *saved = ctx->cur_env;
  completion c = cnorm();
  gc_push_scope();
  gc_keep(saved);
  ctx->cur_env = js_new_env(ctx, saved, 0); /* scope for a `let` init */
  if (n->u.forc.init) {
    if (n->u.forc.init->type == JS_VAR_DECL)
      exec_var_decl(ctx, n->u.forc.init);
    else
      evaluate(ctx, n->u.forc.init);
  }
  while (!ctx->has_exc) {
    int brk;
    if (n->u.forc.test) {
      js_value t = evaluate(ctx, n->u.forc.test);
      if (ctx->has_exc)
        break;
      if (!js_to_boolean(t))
        break;
    }
    c = exec_loop_body(ctx, n->u.forc.body, label, &brk);
    if (ctx->has_exc || c.kind != C_NORMAL || brk)
      break;
    if (n->u.forc.update)
      evaluate(ctx, n->u.forc.update);
  }
  ctx->cur_env = saved;
  gc_pop_scope();
  return ctx->has_exc ? cnorm() : c;
}

static void bind_for_target(js_ctx *ctx, js_node *left, js_value item)
{
  if (left->type == JS_VAR_DECL) {
    /* `var` assigns to the hoisted function-scope binding (declare=0) so the
     * loop variable survives the loop; `let`/`const` bind fresh in the
     * per-iteration block scope. */
    int declare = left->u.var.kind != JS_DECL_VAR;
    bind_pattern(ctx, left->u.var.decls[0].id, item, declare, P_WRITABLE | P_ENUMERABLE);
  } else
    bind_pattern(ctx, left, item, 0, 0);
}

/* the iteration protocol: obj[Symbol.iterator]() then repeated .next() */
static int get_iterator(js_ctx *ctx, js_value v, js_value *iter)
{
  js_value itfn;
  if (v.type != JV_OBJ)
    return 0;
  itfn = js_get(ctx, v, js_symbol_key(ctx->sym_iterator));
  if (ctx->has_exc || !js_is_callable(itfn))
    return 0;
  *iter = js_call(ctx, itfn, v, NULL, 0);
  return !ctx->has_exc && iter->type == JV_OBJ;
}
static int iterator_step(js_ctx *ctx, js_value iter, js_value *value)
{
  js_value res, nextfn = js_get(ctx, iter, "next");
  if (!js_is_callable(nextfn)) {
    js_throw_type(ctx, "iterator.next is not a function");
    return 0;
  }
  gc_push_scope();
  KEEP(iter);
  res = js_call(ctx, nextfn, iter, NULL, 0);
  if (ctx->has_exc) {
    gc_pop_scope();
    return 0;
  }
  KEEP(res);
  if (js_to_boolean(js_get(ctx, res, "done"))) {
    gc_pop_scope();
    return 0;
  }
  *value = js_get(ctx, res, "value");
  gc_pop_scope();
  return !ctx->has_exc;
}

static completion exec_for_in(js_ctx *ctx, js_node *n, const char *label)
{
  js_value rhs = evaluate(ctx, n->u.forin.right);
  completion result = cnorm();
  if (ctx->has_exc)
    return cnorm();
  gc_push_scope();
  KEEP(rhs);

  if (n->u.forin.of) { /* for...of */
    if ((rhs.type == JV_OBJ && rhs.u.obj->kind == OBJ_ARRAY) || rhs.type == JV_STR) {
      unsigned i, len = rhs.type == JV_STR ? (unsigned)rhs.u.str->len : rhs.u.obj->elen;
      for (i = 0; i < len && !ctx->has_exc; i++) {
        js_value item = js_get_index(ctx, rhs, i);
        int brk;
        js_env *saved = ctx->cur_env;
        gc_push_scope();
        gc_keep(saved);
        KEEP(item);
        ctx->cur_env = js_new_env(ctx, saved, 0);
        bind_for_target(ctx, n->u.forin.left, item);
        result = ctx->has_exc ? cnorm() : exec_loop_body(ctx, n->u.forin.body, label, &brk);
        ctx->cur_env = saved;
        gc_pop_scope();
        if (ctx->has_exc || result.kind != C_NORMAL || brk)
          break;
      }
    } else { /* general iterables (Map/Set/...) */
      js_value iter, item;
      if (!get_iterator(ctx, rhs, &iter)) {
        gc_pop_scope();
        if (!ctx->has_exc)
          js_throw_type(ctx, "value is not iterable");
        return cnorm();
      }
      gc_push_scope();
      KEEP(iter);
      while (!ctx->has_exc && iterator_step(ctx, iter, &item)) {
        int brk;
        js_env *saved = ctx->cur_env;
        gc_push_scope();
        gc_keep(saved);
        KEEP(item);
        ctx->cur_env = js_new_env(ctx, saved, 0);
        bind_for_target(ctx, n->u.forin.left, item);
        result = ctx->has_exc ? cnorm() : exec_loop_body(ctx, n->u.forin.body, label, &brk);
        ctx->cur_env = saved;
        gc_pop_scope();
        if (ctx->has_exc || result.kind != C_NORMAL || brk)
          break;
      }
      gc_pop_scope();
    }
  } else if (rhs.type == JV_OBJ) { /* for...in */
    js_object *o0 = rhs.u.obj, *o;
    char seen[64][48];
    int nseen = 0, stop = 0;
    if (o0->kind == OBJ_ARRAY) {
      unsigned i;
      for (i = 0; i < o0->elen && !ctx->has_exc && !stop; i++) {
        char ib[24];
        int brk;
        js_env *saved;
        js_value key;
        u64toa((unsigned long long)i, ib, 10);
        key = js_mk_cstring(ctx, ib);
        saved = ctx->cur_env;
        gc_push_scope();
        gc_keep(saved);
        KEEP(key);
        ctx->cur_env = js_new_env(ctx, saved, 0);
        bind_for_target(ctx, n->u.forin.left, key);
        result = ctx->has_exc ? cnorm() : exec_loop_body(ctx, n->u.forin.body, label, &brk);
        ctx->cur_env = saved;
        gc_pop_scope();
        if (ctx->has_exc || result.kind != C_NORMAL || brk)
          stop = 1;
      }
    }
    for (o = o0; o && !ctx->has_exc && !stop; o = o->proto) {
      int k;
      for (k = 0; k < o->props.n && !ctx->has_exc && !stop; k++) {
        int dup = 0, j, brk;
        js_env *saved;
        js_value key;
        if (!(o->props.e[k].flags & P_ENUMERABLE))
          continue;
        for (j = 0; j < nseen; j++)
          if (strcmp(seen[j], o->props.e[k].key) == 0) {
            dup = 1;
            break;
          }
        if (dup)
          continue;
        if (nseen < 64) {
          int m = strlen(o->props.e[k].key);
          if (m > 46)
            m = 46;
          memcpy(seen[nseen], o->props.e[k].key, (unsigned)m);
          seen[nseen][m] = 0;
          nseen++;
        }
        key = js_mk_cstring(ctx, o->props.e[k].key);
        saved = ctx->cur_env;
        gc_push_scope();
        gc_keep(saved);
        KEEP(key);
        ctx->cur_env = js_new_env(ctx, saved, 0);
        bind_for_target(ctx, n->u.forin.left, key);
        result = ctx->has_exc ? cnorm() : exec_loop_body(ctx, n->u.forin.body, label, &brk);
        ctx->cur_env = saved;
        gc_pop_scope();
        if (ctx->has_exc || result.kind != C_NORMAL || brk)
          stop = 1;
      }
    }
  }
  gc_pop_scope();
  return ctx->has_exc ? cnorm() : result;
}

static completion exec_switch(js_ctx *ctx, js_node *n, const char *label)
{
  js_value disc = evaluate(ctx, n->u.sw.disc);
  js_env *saved = ctx->cur_env;
  completion c = cnorm();
  int i, matched = -1;
  if (ctx->has_exc)
    return cnorm();
  gc_push_scope();
  gc_keep(saved);
  KEEP(disc);
  ctx->cur_env = js_new_env(ctx, saved, 0);
  for (i = 0; i < n->u.sw.count; i++) {
    if (!n->u.sw.cases[i].test)
      continue;
    {
      js_value t = evaluate(ctx, n->u.sw.cases[i].test);
      if (ctx->has_exc)
        goto done;
      if (js_strict_eq(disc, t)) {
        matched = i;
        break;
      }
    }
  }
  if (matched < 0)
    for (i = 0; i < n->u.sw.count; i++)
      if (!n->u.sw.cases[i].test) {
        matched = i;
        break;
      }
  if (matched >= 0) {
    for (i = matched; i < n->u.sw.count; i++) {
      c = exec_list(ctx, n->u.sw.cases[i].body, n->u.sw.cases[i].nbody);
      if (ctx->has_exc)
        goto done;
      if (c.kind == C_BREAK && (!c.label || (label && strcmp(c.label, label) == 0))) {
        c = cnorm();
        break;
      }
      if (c.kind != C_NORMAL)
        break;
    }
  }
done:
  ctx->cur_env = saved;
  gc_pop_scope();
  return ctx->has_exc ? cnorm() : c;
}

static completion exec_try(js_ctx *ctx, js_node *n)
{
  completion c;
  c = exec_stmt(ctx, n->u.tc.block, NULL);
  if (ctx->has_exc && n->u.tc.handler) {
    js_value ex = ctx->exc;
    js_env *saved = ctx->cur_env;
    ctx->has_exc = 0;
    ctx->exc_root = NULL;
    gc_push_scope();
    gc_keep(saved);
    KEEP(ex);
    ctx->cur_env = js_new_env(ctx, saved, 0);
    if (n->u.tc.param)
      bind_pattern(ctx, n->u.tc.param, ex, 1, P_WRITABLE | P_ENUMERABLE);
    c = exec_stmt(ctx, n->u.tc.handler, NULL);
    ctx->cur_env = saved;
    gc_pop_scope();
  }
  if (n->u.tc.finalizer) {
    int had = ctx->has_exc;
    js_value saved_exc = ctx->exc;
    void *saved_root = ctx->exc_root;
    completion fc;
    ctx->has_exc = 0;
    ctx->exc_root = NULL;
    fc = exec_stmt(ctx, n->u.tc.finalizer, NULL);
    if (ctx->has_exc)
      return fc; /* finally's throw wins          */
    if (fc.kind != C_NORMAL)
      return fc; /* finally's break/return wins   */
    if (had) {
      ctx->has_exc = 1;
      ctx->exc = saved_exc;
      ctx->exc_root = saved_root;
    }
  }
  return c;
}

static completion exec_list(js_ctx *ctx, js_node **items, int count)
{
  completion c = cnorm();
  int i;
  for (i = 0; i < count; i++) {
    c = exec_stmt(ctx, items[i], NULL);
    if (ctx->has_exc || c.kind != C_NORMAL)
      return c;
  }
  return c;
}

/* Record an exported declaration's bound names into the module's exports object
 * (after the declaration has executed/hoisted into the module scope). Simple
 * identifier bindings only — destructuring patterns in `export const {a}=...`
 * declare locally but aren't re-exported (a v1 limitation). */
static void export_decl_names(js_ctx *ctx, js_node *decl, js_object *exports)
{
  if (!exports || !decl)
    return;
  if (decl->type == JS_VAR_DECL) {
    int i;
    for (i = 0; i < decl->u.var.count; i++) {
      js_node *id = decl->u.var.decls[i].id;
      if (id->type == JS_IDENT) {
        pentry *pe = js_env_lookup(ctx->cur_env, id->u.str.s);
        if (pe)
          js_def(exports, id->u.str.s, pe->value, P_ENUMERABLE | P_WRITABLE);
      }
    }
  } else if (decl->type == JS_FUNC && decl->u.func.name) {
    pentry *pe = js_env_lookup(ctx->cur_env, decl->u.func.name);
    if (pe)
      js_def(exports, decl->u.func.name, pe->value, P_ENUMERABLE | P_WRITABLE);
  } else if (decl->type == JS_CLASS && decl->u.cls.id) {
    pentry *pe = js_env_lookup(ctx->cur_env, decl->u.cls.id->u.str.s);
    if (pe)
      js_def(exports, decl->u.cls.id->u.str.s, pe->value, P_ENUMERABLE | P_WRITABLE);
  }
}

static completion exec_stmt(js_ctx *ctx, js_node *n, const char *label)
{
  if (ctx->has_exc)
    return cnorm();
  switch (n->type) {
  case JS_EXPR_STMT: {
    completion c = cnorm();
    c.value = evaluate(ctx, n->u.one.arg);
    return ctx->has_exc ? cnorm() : c;
  }
  case JS_VAR_DECL:
    return exec_var_decl(ctx, n);
  case JS_BLOCK:
    return exec_block(ctx, n->u.list.items, n->u.list.count);
  case JS_EMPTY:
    return cnorm();
  case JS_FUNC:
    /* Annex B: a block-level function declaration copies its block-scoped
     * value up to the pre-declared function-scope legacy binding when
     * execution reaches it. Top-level declarations (cur_env is already the
     * function scope) were fully bound by hoist_functions and need no copy. */
    if (n->u.func.name && ctx->cur_env && !ctx->cur_env->is_fn_scope) {
      js_env *fe = ctx->cur_env->parent;
      while (fe && !fe->is_fn_scope)
        fe = fe->parent;
      if (fe && pmap_find_in_env(fe, n->u.func.name)) {
        pentry *bpe = js_env_lookup(ctx->cur_env, n->u.func.name);
        if (bpe)
          js_env_define(fe, n->u.func.name, bpe->value, P_WRITABLE | P_ENUMERABLE);
      }
    }
    return cnorm();
  case JS_DEBUGGER:
    return cnorm();
  case JS_IF:
    if (js_to_boolean(evaluate(ctx, n->u.iff.test))) {
      if (ctx->has_exc)
        return cnorm();
      return exec_stmt(ctx, n->u.iff.cons, NULL);
    }
    if (ctx->has_exc)
      return cnorm();
    return n->u.iff.alt ? exec_stmt(ctx, n->u.iff.alt, NULL) : cnorm();
  case JS_WHILE: {
    completion c = cnorm();
    while (!ctx->has_exc) {
      int brk;
      js_value t = evaluate(ctx, n->u.loop.test);
      if (ctx->has_exc || !js_to_boolean(t))
        break;
      c = exec_loop_body(ctx, n->u.loop.body, label, &brk);
      if (ctx->has_exc || c.kind != C_NORMAL || brk)
        break;
    }
    return ctx->has_exc ? cnorm() : c;
  }
  case JS_DO_WHILE: {
    completion c = cnorm();
    do {
      int brk;
      c = exec_loop_body(ctx, n->u.loop.body, label, &brk);
      if (ctx->has_exc || c.kind != C_NORMAL || brk)
        break;
      {
        js_value t = evaluate(ctx, n->u.loop.test);
        if (ctx->has_exc || !js_to_boolean(t))
          break;
      }
    } while (1);
    return ctx->has_exc ? cnorm() : c;
  }
  case JS_FOR:
    return exec_for(ctx, n, label);
  case JS_FOR_IN:
    return exec_for_in(ctx, n, label);
  case JS_SWITCH:
    return exec_switch(ctx, n, label);
  case JS_TRY:
    return exec_try(ctx, n);
  case JS_RETURN: {
    completion c;
    c.kind = C_RETURN;
    c.label = NULL;
    c.value = n->u.one.arg ? evaluate(ctx, n->u.one.arg) : js_undef();
    return ctx->has_exc ? cnorm() : c;
  }
  case JS_BREAK: {
    completion c = cnorm();
    c.kind = C_BREAK;
    c.label = n->u.labeled.label;
    return c;
  }
  case JS_CONTINUE: {
    completion c = cnorm();
    c.kind = C_CONTINUE;
    c.label = n->u.labeled.label;
    return c;
  }
  case JS_THROW: {
    js_value e = evaluate(ctx, n->u.one.arg);
    if (!ctx->has_exc)
      js_throw(ctx, e);
    return cnorm();
  }
  case JS_LABELED: {
    completion c = exec_stmt(ctx, n->u.labeled.body, n->u.labeled.label);
    if (c.kind == C_BREAK && c.label && strcmp(c.label, n->u.labeled.label) == 0)
      return cnorm();
    return c;
  }
  case JS_CLASS: {
    js_value cv = evaluate(ctx, n);
    if (!ctx->has_exc && n->u.cls.id)
      js_env_define(ctx->cur_env, n->u.cls.id->u.str.s, cv, P_WRITABLE | P_ENUMERABLE);
    return cnorm();
  }
  case JS_IMPORT: {
    js_object *ns;
    if (!ctx->load_module)
      return cnorm();
    gc_push_scope();
    ns = ctx->load_module(ctx, n->u.imp.spec, ctx->load_module_ud);
    if (ctx->has_exc) {
      gc_pop_scope();
      return cnorm();
    }
    if (!ns) {
      gc_pop_scope();
      js_throw_type(ctx, "module not found");
      return cnorm();
    }
    gc_keep(ns);
    {
      js_value nsv = js_obj_value(ns);
      int i;
      if (n->u.imp.defname)
        js_env_define(ctx->cur_env, n->u.imp.defname, js_get(ctx, nsv, "default"), P_WRITABLE | P_ENUMERABLE);
      if (n->u.imp.nsname)
        js_env_define(ctx->cur_env, n->u.imp.nsname, nsv, P_WRITABLE | P_ENUMERABLE);
      for (i = 0; i < n->u.imp.nnames; i++)
        js_env_define(ctx->cur_env, n->u.imp.names[i].local, js_get(ctx, nsv, n->u.imp.names[i].imported),
                      P_WRITABLE | P_ENUMERABLE);
    }
    gc_pop_scope();
    return cnorm();
  }
  case JS_EXPORT: {
    js_object *exp = ctx->cur_module_exports;
    if (n->u.exp.defexpr) {
      js_value v = evaluate(ctx, n->u.exp.defexpr);
      if (!ctx->has_exc && exp)
        js_def(exp, "default", v, P_ENUMERABLE | P_WRITABLE);
      return cnorm();
    }
    if (n->u.exp.decl) {
      completion c = exec_stmt(ctx, n->u.exp.decl, NULL);
      if (!ctx->has_exc)
        export_decl_names(ctx, n->u.exp.decl, exp);
      return ctx->has_exc ? cnorm() : c;
    }
    if (n->u.exp.spec) { /* re-export from another module */
      js_object *ns;
      gc_push_scope();
      ns = ctx->load_module ? ctx->load_module(ctx, n->u.exp.spec, ctx->load_module_ud) : NULL;
      if (ctx->has_exc) {
        gc_pop_scope();
        return cnorm();
      }
      if (!ns) {
        gc_pop_scope();
        js_throw_type(ctx, "module not found");
        return cnorm();
      }
      gc_keep(ns);
      if (exp) {
        js_value nsv = js_obj_value(ns);
        if (n->u.exp.star) {
          pmap *pm = &ns->props;
          int i;
          for (i = 0; i < pm->n; i++) {
            pentry *pe = &pm->e[i];
            if ((pe->flags & P_ENUMERABLE) && strcmp(pe->key, "default") != 0)
              js_def(exp, pe->key, js_get(ctx, nsv, pe->key), P_ENUMERABLE | P_WRITABLE);
          }
        } else {
          int i;
          for (i = 0; i < n->u.exp.nnames; i++)
            js_def(exp, n->u.exp.names[i].local, js_get(ctx, nsv, n->u.exp.names[i].imported),
                   P_ENUMERABLE | P_WRITABLE);
        }
      }
      gc_pop_scope();
      return cnorm();
    }
    if (exp) { /* local: export { a, b as c } */
      int i;
      for (i = 0; i < n->u.exp.nnames; i++) {
        pentry *pe = js_env_lookup(ctx->cur_env, n->u.exp.names[i].imported);
        if (pe)
          js_def(exp, n->u.exp.names[i].local, pe->value, P_ENUMERABLE | P_WRITABLE);
      }
    }
    return cnorm();
  }
  default: {
    completion c = cnorm();
    c.value = evaluate(ctx, n);
    return ctx->has_exc ? cnorm() : c;
  }
  }
}

/* ------------------------------------------------------- calls ------------ */

static js_value call_function(js_ctx *ctx, js_value fnval, js_value thisv, js_value *args, int argc)
{
  js_object *o = fnval.u.obj;
  js_func *f;
  js_node *node;

  if (o->kind == OBJ_PROXY)
    return js_proxy_apply(ctx, o, thisv, args, argc); /* apply trap */
  f = o->fn;
  js_env *env, *saved;
  js_value result = js_undef();
  completion c;
  int i, ai;
  int pushed = 0;
  char probe;

  /* Guard before the native dispatch below — a native callback (forEach, JSON
   * reviver, sort comparator ...) recurses on the C stack without bumping
   * depth. */
  if (stack_overflow(ctx, &probe))
    return js_throw_range(ctx, "maximum call stack size exceeded");

  if (ctx->nframes < 64) {
    ctx->frames[ctx->nframes++] = (f->name && f->name[0]) ? f->name : "<anonymous>";
    pushed = 1;
  }

  if (f->native) {
    js_value r;
    ctx->native_data = f->data;
    r = f->native(ctx, thisv, args, argc);
    if (pushed)
      ctx->nframes--;
    return r;
  }

  if (ctx->depth >= MAX_DEPTH) {
    if (pushed)
      ctx->nframes--;
    return js_throw_range(ctx, "maximum call stack size exceeded");
  }
  ctx->depth++;
  node = f->node;
  saved = ctx->cur_env;

  gc_push_scope();
  gc_keep(saved);
  gc_keep(o);
  KEEP(thisv);
  for (i = 0; i < argc; i++)
    KEEP(args[i]);

  env = js_new_env(ctx, f->closure, 1);
  gc_keep(env);
  if (!f->is_arrow) {
    env->has_this = 1;
    env->thisv = thisv;
    env->home = f->home;
  }
  ctx->cur_env = env;

  if (node) {
    ai = 0;
    for (i = 0; i < node->u.func.nparams && !ctx->has_exc; i++) {
      js_node *p = node->u.func.params[i];
      if (p->type == JS_REST) {
        js_object *rest = js_new_array(ctx, 0);
        gc_keep(rest);
        for (; ai < argc; ai++)
          js_array_push(ctx, rest, args[ai]);
        bind_pattern(ctx, p->u.one.arg, js_obj_value(rest), 1, P_WRITABLE | P_ENUMERABLE);
        break;
      }
      {
        js_value v = ai < argc ? args[ai] : js_undef();
        ai++;
        bind_pattern(ctx, p, v, 1, P_WRITABLE | P_ENUMERABLE);
      }
    }
    if (!f->is_arrow && !ctx->has_exc) {
      js_object *argobj = js_new_array(ctx, 0);
      gc_keep(argobj);
      for (i = 0; i < argc; i++)
        js_array_push(ctx, argobj, args[i]);
      js_env_define(env, "arguments", js_obj_value(argobj), P_WRITABLE);
    }
  }

  /* class instance fields are initialised on `this` (this is the only place we
   * have the home/class binding and the bound this together). */
  if (f->class_node && !f->is_arrow && !ctx->has_exc) {
    js_node *cls = f->class_node;
    int k;
    for (k = 0; k < cls->u.cls.nmembers && !ctx->has_exc; k++) {
      js_member *m = &cls->u.cls.members[k];
      char buf[DTOA_MAX];
      const char *key;
      js_value kh, v;
      if (m->kind != JS_M_FIELD || m->is_static)
        continue;
      kh = prop_key_value(ctx, m->key, m->computed, buf, (int)sizeof buf, &key);
      if (ctx->has_exc)
        break;
      gc_push_scope();
      KEEP(kh);
      v = m->value ? evaluate(ctx, m->value) : js_undef();
      if (!ctx->has_exc) {
        KEEP(v);
        js_set(ctx, thisv, key, v);
      }
      gc_pop_scope();
    }
  }

  if (!ctx->has_exc) {
    if (node && node->u.func.is_generator) {
      js_throw_type(ctx, "generator functions need coroutine runtime (not yet implemented)");
    } else if (node) {
      if (node->u.func.is_expr_body)
        result = evaluate(ctx, node->u.func.body);
      else {
        js_node *body = node->u.func.body; /* JS_BLOCK */
        hoist_vars(ctx, env, body);
        hoist_block_funcs(ctx, env, body);
        hoist_functions(ctx, body->u.list.items, body->u.list.count);
        c = exec_list(ctx, body->u.list.items, body->u.list.count);
        if (!ctx->has_exc && c.kind == C_RETURN)
          result = c.value;
      }
    } else if (f->class_node && f->class_node->u.cls.super) {
      /* synthetic default constructor of a derived class: super(...args) */
      js_object *home = f->home;
      js_value sc = (home && home->proto) ? js_get(ctx, js_obj_value(home->proto), "constructor") : js_undef();
      if (js_is_callable(sc))
        call_function(ctx, sc, thisv, args, argc);
    }
  }

  /* an async function settles a promise with its return value or thrown error
   * (synchronous model: await unwraps already-settled promises inline). */
  if (node && node->u.func.is_async) {
    if (ctx->has_exc) {
      js_value e = ctx->exc;
      ctx->has_exc = 0;
      ctx->exc_root = NULL;
      KEEP(e);
      result = js_promise_rejected(ctx, e);
    } else {
      KEEP(result);
      result = js_promise_resolved(ctx, result);
    }
  }

  ctx->cur_env = saved;
  gc_pop_scope();
  ctx->depth--;
  if (pushed)
    ctx->nframes--;
  return ctx->has_exc ? js_undef() : result;
}

js_value js_call(js_ctx *ctx, js_value fnval, js_value thisv, js_value *args, int argc)
{
  if (!js_is_callable(fnval))
    return js_throw_type(ctx, "value is not a function");
  return call_function(ctx, fnval, thisv, args, argc);
}

js_value js_construct(js_ctx *ctx, js_value fnval, js_value *args, int argc)
{
  js_object *fn;
  js_value proto, thisobj, result;
  if (!js_is_callable(fnval))
    return js_throw_type(ctx, "value is not a constructor");
  fn = fnval.u.obj;
  if (fn->kind == OBJ_PROXY)
    return js_proxy_construct(ctx, fn, args, argc); /* construct trap */
  { /* a bound function (Function.prototype.bind) constructs its target with the
     * bound partial args prepended, ignoring the bound `this`. */
    pentry *pb = pmap_find_pub(fn, "@bound");
    if (pb && pb->value.type == JV_OBJ && pb->value.u.obj->kind == OBJ_ARRAY) {
      js_object *rec = pb->value.u.obj;
      js_value target = rec->elen > 0 ? rec->elems[0] : js_undef();
      int nbound = (int)rec->elen - 2, total, i;
      js_value *cv, res;
      if (nbound < 0)
        nbound = 0;
      total = nbound + argc;
      cv = total ? (js_value *)malloc((unsigned)total * sizeof *cv) : 0;
      if (total && !cv)
        return js_throw_range(ctx, "bound construct: out of memory");
      gc_push_scope();
      KEEP(fnval);
      KEEP(target);
      for (i = 0; i < nbound; i++) {
        cv[i] = rec->elems[2 + i];
        KEEP(cv[i]);
      }
      for (i = 0; i < argc; i++) {
        cv[nbound + i] = args[i];
        KEEP(cv[nbound + i]);
      }
      res = js_construct(ctx, target, cv, total);
      gc_pop_scope();
      if (cv)
        free(cv);
      return res;
    }
  }
  if (fn->fn->native) {
    ctx->native_data = fn->fn->data;
    return fn->fn->native(ctx, js_undef(), args, argc);
  } /* native ctors build their own object */
  if (fn->fn->is_arrow)
    return js_throw_type(ctx, "arrow function is not a constructor");

  gc_push_scope();
  KEEP(fnval);
  {
    int i;
    for (i = 0; i < argc; i++)
      KEEP(args[i]);
  }
  proto = js_get(ctx, fnval, "prototype");
  thisobj = js_obj_value(js_new_object(ctx, proto.type == JV_OBJ ? proto.u.obj : ctx->object_proto));
  KEEP(thisobj);
  result = call_function(ctx, fnval, thisobj, args, argc);
  gc_pop_scope();
  if (ctx->has_exc)
    return js_undef();
  return result.type == JV_OBJ ? result : thisobj;
}

/* ------------------------------------------------------- eval / Function -- */

/* Parse `src` into a Program whose arena is kept alive for the context's life
 * (functions defined by eval'd / dynamically-built code outlive the call). */
static js_node *compile_dyn(js_ctx *ctx, const char *src, js_parse_error *err)
{
  js_arena *a = (js_arena *)malloc(sizeof *a);
  js_node *prog;
  if (!a) {
    if (err) {
      err->msg = "out of memory";
      err->line = 0;
      err->col = 0;
    }
    return NULL;
  }
  js_arena_init(a);
  prog = js_parse(src, a, err);
  if (!prog) {
    js_arena_free(a);
    free(a);
    return NULL;
  }
  if (ctx->dyn_n == ctx->dyn_cap) {
    int nc = ctx->dyn_cap ? ctx->dyn_cap * 2 : 4;
    ctx->dyn_arenas = (js_arena **)realloc(ctx->dyn_arenas, (unsigned)nc * sizeof(js_arena *));
    ctx->dyn_cap = nc;
  }
  ctx->dyn_arenas[ctx->dyn_n++] = a;
  return prog;
}

js_value js_builtin_eval(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_node *prog;
  js_parse_error e;
  completion c;
  (void)thisv;
  if (argc < 1)
    return js_undef();
  if (args[0].type != JV_STR)
    return args[0]; /* eval of a non-string -> itself */
  prog = compile_dyn(ctx, args[0].u.str->data, &e);
  if (!prog)
    return js_throw_error(ctx, ctx->syntax_error_proto, "SyntaxError", e.msg ? e.msg : "parse error");
  /* direct: declarations land in the current scope. That can inject a binding
   * into a scope some identifier cache already resolved past, so drop all
   * caches. */
  ctx->ident_cache_gen++;
  {
    int i;
    for (i = 0; i < prog->u.list.count; i++)
      hoist_vars(ctx, ctx->cur_env, prog->u.list.items[i]);
  }
  {
    int i;
    for (i = 0; i < prog->u.list.count; i++)
      hoist_block_funcs(ctx, ctx->cur_env, prog->u.list.items[i]);
  }
  hoist_functions(ctx, prog->u.list.items, prog->u.list.count);
  c = exec_list(ctx, prog->u.list.items, prog->u.list.count);
  return ctx->has_exc ? js_undef() : c.value;
}

/* grow buffer used only to assemble the Function-constructor source */
typedef struct {
  char *b;
  int n, cap;
} srcbuf;
static void srcb(srcbuf *s, const char *t, int n)
{
  if (s->n + n + 1 > s->cap) {
    int c = s->cap ? s->cap * 2 : 64;
    while (c < s->n + n + 1)
      c *= 2;
    s->b = (char *)realloc(s->b, (unsigned)c);
    s->cap = c;
  }
  memcpy(s->b + s->n, t, (unsigned)n);
  s->n += n;
  s->b[s->n] = 0;
}

js_value js_builtin_function(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  srcbuf sb;
  js_node *prog;
  js_parse_error e;
  js_env *saved;
  js_value r;
  int i;
  (void)thisv;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  srcb(&sb, "(function anonymous(", 20);
  for (i = 0; i + 1 < argc; i++) {
    js_value s = js_to_string(ctx, args[i]);
    if (ctx->has_exc) {
      if (sb.b)
        free(sb.b);
      return js_undef();
    }
    if (i)
      srcb(&sb, ",", 1);
    srcb(&sb, s.u.str->data, s.u.str->len);
  }
  srcb(&sb, "\n){", 3);
  if (argc >= 1) {
    js_value body = js_to_string(ctx, args[argc - 1]);
    if (ctx->has_exc) {
      if (sb.b)
        free(sb.b);
      return js_undef();
    }
    srcb(&sb, body.u.str->data, body.u.str->len);
  }
  srcb(&sb, "\n})", 3);
  prog = compile_dyn(ctx, sb.b ? sb.b : "(function(){})", &e);
  if (sb.b)
    free(sb.b);
  if (!prog)
    return js_throw_error(ctx, ctx->syntax_error_proto, "SyntaxError", e.msg ? e.msg : "parse error");
  /* Function-built functions are global-scoped, not closures over the caller.
   */
  saved = ctx->cur_env;
  ctx->cur_env = ctx->global_env;
  {
    completion c = exec_list(ctx, prog->u.list.items, prog->u.list.count);
    r = c.value;
  }
  ctx->cur_env = saved;
  return ctx->has_exc ? js_undef() : r;
}

/* ------------------------------------------------------- run -------------- */

int js_run(js_ctx *ctx, js_node *program, js_value *result)
{
  completion c;
  int i;
  ctx->cur_env = ctx->global_env;
  for (i = 0; i < program->u.list.count; i++)
    hoist_vars(ctx, ctx->global_env, program->u.list.items[i]);
  for (i = 0; i < program->u.list.count; i++)
    hoist_block_funcs(ctx, ctx->global_env, program->u.list.items[i]);
  hoist_functions(ctx, program->u.list.items, program->u.list.count);
  c = exec_list(ctx, program->u.list.items, program->u.list.count);
  if (ctx->has_exc)
    return -1;
  if (result)
    *result = c.value;
  return 0;
}

void js_set_module_loader(js_ctx *ctx, js_object *(*fn)(js_ctx *, const char *, void *), void *ud)
{
  ctx->load_module = fn;
  ctx->load_module_ud = ud;
}

/* Run a module body in its own top-level scope (a function-scope child of
 * global, so `var`/function hoist there, not onto window). `export`s land in
 * `exports`. */
int js_run_module(js_ctx *ctx, js_node *program, js_object *exports, const char *url)
{
  completion c;
  int i;
  js_env *saved_env = ctx->cur_env;
  js_object *saved_exp = ctx->cur_module_exports;
  const char *saved_url = ctx->cur_module_url;
  js_env *menv;
  gc_push_scope();
  menv = (js_env *)gc_keep(js_new_env(ctx, ctx->global_env, 1));
  /* Pin exports in this module's scope: it must survive GC for the whole body
   * AND every nested import it triggers (this scope pops only after exec_list),
   * so the engine doesn't rely on the embedder rooting it separately. */
  if (exports)
    gc_keep(exports);
  ctx->cur_env = menv;
  ctx->cur_module_exports = exports;
  ctx->cur_module_url = url;
  for (i = 0; i < program->u.list.count; i++)
    hoist_vars(ctx, menv, program->u.list.items[i]);
  for (i = 0; i < program->u.list.count; i++)
    hoist_block_funcs(ctx, menv, program->u.list.items[i]);
  hoist_functions(ctx, program->u.list.items, program->u.list.count);
  c = exec_list(ctx, program->u.list.items, program->u.list.count);
  (void)c;
  ctx->cur_env = saved_env;
  ctx->cur_module_exports = saved_exp;
  ctx->cur_module_url = saved_url;
  gc_pop_scope();
  return ctx->has_exc ? -1 : 0;
}

/* ------------------------------------------------------- ctx lifecycle ---- */

js_ctx *js_ctx_new(void (*print)(js_ctx *ctx, int level, const char *s, int len))
{
  js_ctx *ctx = (js_ctx *)malloc(sizeof(js_ctx));
  memset(ctx, 0, sizeof(js_ctx));
  ctx->print = print;
  ctx->rng = 0x9e3779b9u;
  ctx->ident_cache_gen = 1;

  js_gc_setup();
  gc_add_root((void **)&ctx->global);
  gc_add_root((void **)&ctx->global_env);
  gc_add_root((void **)&ctx->cur_env);
  gc_add_root((void **)&ctx->object_proto);
  gc_add_root((void **)&ctx->function_proto);
  gc_add_root((void **)&ctx->array_proto);
  gc_add_root((void **)&ctx->string_proto);
  gc_add_root((void **)&ctx->number_proto);
  gc_add_root((void **)&ctx->boolean_proto);
  gc_add_root((void **)&ctx->error_proto);
  gc_add_root((void **)&ctx->promise_proto);
  gc_add_root((void **)&ctx->symbol_proto);
  gc_add_root((void **)&ctx->bigint_proto);
  gc_add_root((void **)&ctx->map_proto);
  gc_add_root((void **)&ctx->set_proto);
  gc_add_root((void **)&ctx->weakmap_proto);
  gc_add_root((void **)&ctx->weakset_proto);
  gc_add_root((void **)&ctx->date_proto);
  gc_add_root((void **)&ctx->regexp_proto);
  gc_add_root((void **)&ctx->typedarray_proto);
  gc_add_root((void **)&ctx->arraybuffer_proto);
  gc_add_root((void **)&ctx->dataview_proto);
  {
    int i;
    for (i = 0; i < 9; i++)
      gc_add_root((void **)&ctx->typed_protos[i]);
  }
  gc_add_root((void **)&ctx->type_error_proto);
  gc_add_root((void **)&ctx->range_error_proto);
  gc_add_root((void **)&ctx->syntax_error_proto);
  gc_add_root((void **)&ctx->ref_error_proto);
  gc_add_root((void **)&ctx->eval_error_proto);
  gc_add_root((void **)&ctx->uri_error_proto);
  gc_add_root((void **)&ctx->aggregate_error_proto);
  gc_add_root((void **)&ctx->sym_registry);
  gc_add_root((void **)&ctx->sym_for_reg);
  gc_add_root(&ctx->exc_root);

  ctx->object_proto = js_new_object(ctx, NULL);
  ctx->function_proto = js_new_object(ctx, ctx->object_proto);
  ctx->array_proto = js_new_object(ctx, ctx->object_proto);
  ctx->string_proto = js_new_object(ctx, ctx->object_proto);
  ctx->number_proto = js_new_object(ctx, ctx->object_proto);
  ctx->boolean_proto = js_new_object(ctx, ctx->object_proto);
  ctx->error_proto = js_new_object(ctx, ctx->object_proto);
  ctx->promise_proto = js_new_object(ctx, ctx->object_proto);
  ctx->symbol_proto = js_new_object(ctx, ctx->object_proto);
  ctx->bigint_proto = js_new_object(ctx, ctx->object_proto);
  ctx->map_proto = js_new_object(ctx, ctx->object_proto);
  ctx->set_proto = js_new_object(ctx, ctx->object_proto);
  ctx->weakmap_proto = js_new_object(ctx, ctx->object_proto);
  ctx->weakset_proto = js_new_object(ctx, ctx->object_proto);
  ctx->date_proto = js_new_object(ctx, ctx->object_proto);
  ctx->regexp_proto = js_new_object(ctx, ctx->object_proto);
  ctx->typedarray_proto = js_new_object(ctx, ctx->object_proto);
  ctx->arraybuffer_proto = js_new_object(ctx, ctx->object_proto);
  ctx->dataview_proto = js_new_object(ctx, ctx->object_proto);
  {
    int i;
    for (i = 0; i < 9; i++)
      ctx->typed_protos[i] = js_new_object(ctx, ctx->typedarray_proto);
  }
  ctx->type_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->range_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->syntax_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->ref_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->eval_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->uri_error_proto = js_new_object(ctx, ctx->error_proto);
  ctx->aggregate_error_proto = js_new_object(ctx, ctx->error_proto);

  ctx->global = js_new_object(ctx, ctx->object_proto);
  ctx->global_env = js_new_env(ctx, NULL, 1);
  ctx->global_env->has_this = 1;
  ctx->global_env->thisv = js_obj_value(ctx->global);
  ctx->cur_env = ctx->global_env;

  ctx->sym_registry = js_new_array(ctx, 0); /* must exist before js_mk_symbol runs */
  js_setup_builtins(ctx);
  return ctx;
}

void js_ctx_free(js_ctx *ctx)
{
  /* Drop every root this context registered, then collect: with nothing
   * reachable the whole JS heap (object headers + their malloc'd internals via
   * the finalizer) is reclaimed. This matters when the embedder creates a fresh
   * context per use (e.g. the browser, once per page) over the shared GC. */
  gc_remove_root((void **)&ctx->global);
  gc_remove_root((void **)&ctx->global_env);
  gc_remove_root((void **)&ctx->cur_env);
  gc_remove_root((void **)&ctx->object_proto);
  gc_remove_root((void **)&ctx->function_proto);
  gc_remove_root((void **)&ctx->array_proto);
  gc_remove_root((void **)&ctx->string_proto);
  gc_remove_root((void **)&ctx->number_proto);
  gc_remove_root((void **)&ctx->boolean_proto);
  gc_remove_root((void **)&ctx->error_proto);
  gc_remove_root((void **)&ctx->promise_proto);
  gc_remove_root((void **)&ctx->symbol_proto);
  gc_remove_root((void **)&ctx->bigint_proto);
  gc_remove_root((void **)&ctx->map_proto);
  gc_remove_root((void **)&ctx->set_proto);
  gc_remove_root((void **)&ctx->weakmap_proto);
  gc_remove_root((void **)&ctx->weakset_proto);
  gc_remove_root((void **)&ctx->date_proto);
  gc_remove_root((void **)&ctx->regexp_proto);
  gc_remove_root((void **)&ctx->typedarray_proto);
  gc_remove_root((void **)&ctx->arraybuffer_proto);
  gc_remove_root((void **)&ctx->dataview_proto);
  {
    int i;
    for (i = 0; i < 9; i++)
      gc_remove_root((void **)&ctx->typed_protos[i]);
  }
  gc_remove_root((void **)&ctx->type_error_proto);
  gc_remove_root((void **)&ctx->range_error_proto);
  gc_remove_root((void **)&ctx->syntax_error_proto);
  gc_remove_root((void **)&ctx->ref_error_proto);
  gc_remove_root((void **)&ctx->eval_error_proto);
  gc_remove_root((void **)&ctx->uri_error_proto);
  gc_remove_root((void **)&ctx->aggregate_error_proto);
  gc_remove_root((void **)&ctx->sym_registry);
  gc_remove_root((void **)&ctx->sym_for_reg);
  gc_remove_root(&ctx->exc_root);
  ctx->has_exc = 0;
  gc_collect();
  {
    int i;
    for (i = 0; i < ctx->dyn_n; i++) {
      js_arena_free(ctx->dyn_arenas[i]);
      free(ctx->dyn_arenas[i]);
    }
  }
  if (ctx->dyn_arenas)
    free(ctx->dyn_arenas);
  free(ctx);
}
