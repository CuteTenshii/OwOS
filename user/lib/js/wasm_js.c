/* wasm_js.c: the WebAssembly JavaScript API (WebAssembly.Module, Instance, Memory, Table, Global, ...) over
 * the engine in user/lib/wasm.
 *
 * Every WebAssembly object is an OBJ_WASM cell whose `host` starts with a tag. Cells are allocated as the
 * collector's rescan kind, so the pointers the trace hook follows need no write barrier. What keeps what alive:
 *   instance -> its module, the instances of imported functions, its tables/memories/globals, host callables
 *   table    -> the instance of every function it holds, the values of externrefs
 *   memory   -> its current ArrayBuffer, which in turn keeps the memory (its bytes belong to the memory)
 * Exported functions are natives whose data is the wasm_func pointer (as a number) and whose home object is
 * the instance. Hidden "@" properties on the global object hold the namespace and the execution context. */
#include "gc.h"
#include "interp.h"
#include "mutex.h"
#include "owos.h"
#include "value.h"
#include "wasm.h"
#include "wasm_int.h"

#define ARG(i) ((i) < n ? a[i] : js_undef())

enum { WT_MODULE = 1, WT_INSTANCE, WT_MEMORY, WT_TABLE, WT_GLOBAL, WT_EXEC, WT_TAG, WT_EXN };

typedef struct {
  int tag;
  wasm_module *m;
} wmodule;
typedef struct {
  int tag;
  wasm_instance *inst;
  js_object *module_obj;
} winstance;
typedef struct {
  int tag;
  wasm_memory *mem;
  js_object *buf;     /* the current ArrayBuffer, NULL until asked for or after the memory moved */
  uint32_t buf_pages; /* a shared memory's buffer is as long as the memory was when it was made */
} wmemory;
typedef struct {
  int tag;
  wasm_table *tab;
} wtable;
typedef struct {
  int tag;
  wasm_global *g;
} wglobal;
typedef struct {
  int tag;
  wasm_tag *t;
} wtagw;
typedef struct {
  int tag;
  js_object *tagobj; /* the WebAssembly.Tag */
  js_object *vals;   /* the payload as JS values */
} wexn;
typedef struct {
  int tag;
  wasm_exec *x;
  js_ctx *ctx;
  wasm_extern *xs; /* externrefs crossing into wasm during the outermost call; handle = index + 1 */
  uint32_t nxs, capxs;
  uint32_t *ht; /* open-addressing index over xs: entry = handle, 0 empty */
  uint32_t htcap;
  int depth;
} wexec;

static unsigned slots_of(int t) { return t == WASM_V128 ? 2 : 1; }

/* ---- small helpers ------------------------------------------------------- */

static void mcat(char *buf, size_t cap, const char *s)
{
  size_t n = strlen(buf);
  while (*s && n + 1 < cap)
    buf[n++] = *s++;
  buf[n] = 0;
}

static void mcat_u(char *buf, size_t cap, unsigned v)
{
  char t[12];
  int l = 0;
  do
    t[l++] = (char)('0' + v % 10);
  while ((v /= 10) && l < 11);
  while (l--) {
    char one[2] = {t[l], 0};
    mcat(buf, cap, one);
  }
}

static int tag_of(js_value v)
{
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_WASM || !v.u.obj->host)
    return 0;
  return *(int *)v.u.obj->host;
}

static void mark_val(js_value v)
{
  if (v.type == JV_OBJ || v.type == JV_SYMBOL || v.type == JV_BIGINT)
    gc_mark(v.u.obj);
  else if (v.type == JV_STR)
    gc_mark(v.u.str);
}

static void ext_encode(js_value v, wasm_extern *e)
{
  memset(e, 0, sizeof *e);
  if (v.type == JV_NULL)
    return;
  e->w[0] = (uint64_t)v.type + 1;
  memcpy(&e->w[1], &v.u, sizeof v.u);
}

static js_value ext_decode(const wasm_extern *e)
{
  js_value v;
  memset(&v, 0, sizeof v);
  if (!e->w[0]) {
    v.type = JV_NULL;
    return v;
  }
  v.type = (js_type)(e->w[0] - 1);
  memcpy(&v.u, &e->w[1], sizeof v.u);
  return v;
}

/* The hidden "@wasm" global holds the namespace ("ns"), every prototype ("@Module", ...) and the streaming helper */
static js_object *state_get(js_ctx *ctx, const char *key)
{
  pentry *h = pmap_find_pub(ctx->global, "@wasm");
  pentry *p;
  if (!h || h->value.type != JV_OBJ)
    return NULL;
  p = pmap_find_pub(h->value.u.obj, key);
  return p && p->value.type == JV_OBJ ? p->value.u.obj : NULL;
}

static js_object *ns_proto(js_ctx *ctx, const char *key) { return state_get(ctx, key); }

static js_value throw_wasm(js_ctx *ctx, const char *kind, const char *msg)
{
  char key[24] = "@";
  mcat(key, sizeof key, kind);
  return js_throw_error(ctx, ns_proto(ctx, key), kind, msg);
}

/* A new cell for a WebAssembly object; `host` is freed by the finalizer when the cell dies */
static js_object *new_wasm(js_ctx *ctx, const char *proto_key, void *host)
{
  js_object *o = js_new_array(ctx, 0);
  o->kind = OBJ_WASM;
  o->proto = ns_proto(ctx, proto_key);
  o->host = host;
  return o;
}

/* ---- externref handles --------------------------------------------------- */

static uint32_t ext_hash(const wasm_extern *e)
{
  uint64_t h = e->w[0] * 0x9e3779b97f4a7c15ull ^ e->w[1];
  h ^= h >> 29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 32;
  return (uint32_t)h;
}

static uint64_t xr_intern(void *ud, const wasm_extern *e)
{
  wexec *wx = (wexec *)ud;
  uint32_t i;
  if (wx->htcap) {
    for (i = ext_hash(e) & (wx->htcap - 1); wx->ht[i]; i = (i + 1) & (wx->htcap - 1))
      if (wx->xs[wx->ht[i] - 1].w[0] == e->w[0] && wx->xs[wx->ht[i] - 1].w[1] == e->w[1])
        return wx->ht[i];
  }
  if (wx->nxs >= wx->capxs) {
    uint32_t ncap = wx->capxs ? wx->capxs * 2 : 64;
    wasm_extern *nx = (wasm_extern *)realloc(wx->xs, ncap * sizeof *nx);
    if (!nx)
      gc_oom();
    wx->xs = nx;
    wx->capxs = ncap;
  }
  if ((wx->nxs + 1) * 2 > wx->htcap) {
    uint32_t ncap = wx->htcap ? wx->htcap * 2 : 128, k;
    uint32_t *nh = (uint32_t *)calloc(ncap, sizeof *nh);
    if (!nh)
      gc_oom();
    for (k = 0; k < wx->nxs; k++) {
      for (i = ext_hash(&wx->xs[k]) & (ncap - 1); nh[i]; i = (i + 1) & (ncap - 1))
        ;
      nh[i] = k + 1;
    }
    free(wx->ht);
    wx->ht = nh;
    wx->htcap = ncap;
  }
  wx->xs[wx->nxs++] = *e;
  for (i = ext_hash(e) & (wx->htcap - 1); wx->ht[i]; i = (i + 1) & (wx->htcap - 1))
    ;
  wx->ht[i] = wx->nxs;
  return wx->nxs;
}

static void xr_value(void *ud, uint64_t h, wasm_extern *out)
{
  wexec *wx = (wexec *)ud;
  if (h == 0 || h > wx->nxs)
    memset(out, 0, sizeof *out);
  else
    *out = wx->xs[h - 1];
}

/* ---- conversions --------------------------------------------------------- */

static js_value func_to_js(js_ctx *ctx, wasm_func *wf);
static js_value wasm_call_native(js_ctx *ctx, js_value thisv, js_value *a, int n);
static wexec *get_exec(js_ctx *ctx);
static js_value exn_to_js(js_ctx *ctx, wexec *wx, wasm_exn *e);
static wasm_exn *exn_from_js(js_ctx *ctx, wexec *wx, js_value v);

static wasm_func *func_of(js_value v)
{
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_FUNCTION || !v.u.obj->fn || v.u.obj->fn->native != wasm_call_native)
    return NULL;
  return (wasm_func *)(uintptr_t)v.u.obj->fn->data.u.num;
}

/* ToWebAssemblyValue: 0 with an exception pending on failure */
static int to_wasm(js_ctx *ctx, wexec *wx, js_value v, int type, uint64_t *out)
{
  switch (type) {
  case WASM_I32:
    out[0] = (uint32_t)js_to_int32(ctx, v);
    return !ctx->has_exc;
  case WASM_I64: {
    unsigned lo, hi;
    if (!js_bigint_to_u64(ctx, v, &lo, &hi))
      return 0;
    out[0] = (uint64_t)lo | ((uint64_t)hi << 32);
    return 1;
  }
  case WASM_F32:
    out[0] = of_f32((float)js_to_number(ctx, v));
    return !ctx->has_exc;
  case WASM_F64: {
    double d = js_to_number(ctx, v);
    memcpy(out, &d, 8);
    return !ctx->has_exc;
  }
  case WASM_FUNCREF: {
    wasm_func *wf;
    if (v.type == JV_NULL) {
      out[0] = 0;
      return 1;
    }
    wf = func_of(v);
    if (!wf) {
      js_throw_type(ctx, "WebAssembly function reference must be null or an exported WebAssembly function");
      return 0;
    }
    out[0] = (uint64_t)(uintptr_t)wf;
    return 1;
  }
  case WASM_EXTERNREF: {
    wasm_extern e;
    if (v.type == JV_NULL) {
      out[0] = 0;
      return 1;
    }
    ext_encode(v, &e);
    out[0] = xr_intern(wx, &e);
    return 1;
  }
  }
  js_throw_type(ctx, "type incompatibility when transforming from/to JS");
  return 0;
}

static js_value from_wasm(js_ctx *ctx, wexec *wx, const uint64_t *s, int type)
{
  switch (type) {
  case WASM_I32:
    return js_number((double)(int32_t)(uint32_t)s[0]);
  case WASM_I64:
    return js_bigint_from_u64(ctx, (unsigned)s[0], (unsigned)(s[0] >> 32), 1);
  case WASM_F32:
    return js_number((double)f32_of(s[0]));
  case WASM_F64: {
    double d;
    memcpy(&d, s, 8);
    return js_number(d);
  }
  case WASM_FUNCREF:
    return s[0] ? func_to_js(ctx, (wasm_func *)(uintptr_t)s[0]) : js_null();
  case WASM_EXTERNREF: {
    wasm_extern e;
    xr_value(wx, s[0], &e);
    return ext_decode(&e);
  }
  case WASM_EXNREF:
    return js_throw_type(ctx, "type incompatibility when transforming from/to JS");
  }
  return js_undef();
}

/* ---- GC hooks ------------------------------------------------------------ */

static void wasm_trace(js_object *o)
{
  switch (*(int *)o->host) {
  case WT_INSTANCE: {
    winstance *wi = (winstance *)o->host;
    wasm_instance *in = wi->inst;
    const wasm_module *m = in->module;
    uint32_t k;
    gc_mark(wi->module_obj);
    for (k = 0; k < m->nfuncs; k++) {
      wasm_func *f = in->funcs[k];
      gc_mark(in->store[k].js);
      if (f && f->inst != in)
        gc_mark(f->inst->self);
    }
    for (k = 0; k < m->ntables; k++)
      if (in->tables[k])
        gc_mark(in->tables[k]->self);
    for (k = 0; k < m->nmems; k++)
      if (in->mems[k])
        gc_mark(in->mems[k]->self);
    for (k = 0; k < m->nglobals; k++)
      if (in->globals[k])
        gc_mark(in->globals[k]->self);
    for (k = 0; k < m->ntags; k++)
      if (in->tags[k])
        gc_mark(in->tags[k]->self);
    break;
  }
  case WT_MEMORY:
    gc_mark(((wmemory *)o->host)->buf);
    break;
  case WT_TABLE: {
    wasm_table *t = ((wtable *)o->host)->tab;
    uint32_t k;
    for (k = 0; k < t->n; k++) {
      if (t->type == WASM_FUNCREF) {
        if (t->f[k])
          gc_mark(t->f[k]->inst->self);
      } else
        mark_val(ext_decode(&t->x[k]));
    }
    break;
  }
  case WT_GLOBAL: {
    wasm_global *g = ((wglobal *)o->host)->g;
    if (g->type == WASM_FUNCREF && g->v[0])
      gc_mark(((wasm_func *)(uintptr_t)g->v[0])->inst->self);
    else if (g->type == WASM_EXTERNREF)
      mark_val(ext_decode((const wasm_extern *)g->v));
    break;
  }
  case WT_EXEC: {
    wexec *wx = (wexec *)o->host;
    wasm_exn *e;
    uint32_t k;
    for (k = 0; k < wx->nxs; k++)
      mark_val(ext_decode(&wx->xs[k]));
    for (e = wx->x->exns; e; e = e->next)
      if (!e->tag)
        mark_val(ext_decode(&e->js));
    break;
  }
  case WT_EXN:
    gc_mark(((wexn *)o->host)->tagobj);
    gc_mark(((wexn *)o->host)->vals);
    break;
  }
}

static void wasm_finalize(js_object *o)
{
  switch (*(int *)o->host) {
  case WT_MODULE:
    wasm_module_free(((wmodule *)o->host)->m);
    break;
  case WT_INSTANCE:
    wasm_instance_free(((winstance *)o->host)->inst);
    break;
  case WT_MEMORY:
    wasm_memory_free(((wmemory *)o->host)->mem);
    free(((wmemory *)o->host)->mem);
    break;
  case WT_TABLE:
    wasm_table_free(((wtable *)o->host)->tab);
    free(((wtable *)o->host)->tab);
    break;
  case WT_GLOBAL:
    free(((wglobal *)o->host)->g);
    break;
  case WT_TAG:
    wasm_tag_free(((wtagw *)o->host)->t);
    free(((wtagw *)o->host)->t);
    break;
  case WT_EXEC: {
    wexec *wx = (wexec *)o->host;
    wasm_exec_free(wx->x);
    free(wx->xs);
    free(wx->ht);
    break;
  }
  }
  free(o->host);
  o->host = NULL;
}

/* ---- calling ------------------------------------------------------------- */

static int host_call(void *ud, wasm_func *f, uint64_t *slots)
{
  wexec *wx = (wexec *)ud;
  js_ctx *ctx = wx->ctx;
  const wasm_functype *t = f->type;
  js_value stackargs[8], *argv = stackargs, r;
  uint32_t i, pos = 0;
  int bad = 0;
  if (t->np > 8) {
    argv = (js_value *)malloc(t->np * sizeof *argv);
    if (!argv)
      gc_oom();
  }
  gc_push_scope();
  gc_keep(f->inst->self);
  gc_keep(f->js);
  for (i = 0; i < t->np; i++) {
    if (t->p[i] == WASM_V128) {
      js_throw_type(ctx, "type incompatibility when transforming from/to JS");
      bad = 1;
      break;
    }
    argv[i] = from_wasm(ctx, wx, slots + pos, t->p[i]);
    if (ctx->has_exc) {
      bad = 1;
      break;
    }
    pos += slots_of(t->p[i]);
    if (argv[i].type == JV_OBJ || argv[i].type == JV_BIGINT)
      gc_keep(argv[i].u.obj);
    else if (argv[i].type == JV_STR)
      gc_keep(argv[i].u.str);
  }
  if (!bad) {
    r = js_call(ctx, js_obj_value((js_object *)f->js), js_undef(), argv, (int)t->np);
    if (ctx->has_exc)
      bad = 1;
    else if (t->nr == 1) {
      if (!to_wasm(ctx, wx, r, t->r[0], slots))
        bad = 1;
    } else if (t->nr > 1) {
      js_object *arr = r.type == JV_OBJ && r.u.obj->kind == OBJ_ARRAY ? r.u.obj : NULL;
      if (!arr) {
        js_throw_type(ctx, "multi-return value must be iterable");
        bad = 1;
      } else if (js_array_len(arr) != t->nr) {
        js_throw_type(ctx, "multi-return length mismatch");
        bad = 1;
      } else {
        gc_keep(arr);
        pos = 0;
        for (i = 0; i < t->nr && !bad; i++) {
          if (!to_wasm(ctx, wx, js_get_index(ctx, r, i), t->r[i], slots + pos))
            bad = 1;
          pos += slots_of(t->r[i]);
        }
      }
    }
  }
  gc_pop_scope();
  if (argv != stackargs)
    free(argv);
  return bad;
}

static int exn_catch(void *ud, wasm_exn **out)
{
  wexec *wx = (wexec *)ud;
  js_ctx *ctx = wx->ctx;
  js_value e = ctx->exc;
  wasm_exn *r;
  if (!ctx->has_exc || js_irq_stopped(ctx->irq))
    return 0;
  ctx->has_exc = 0;
  if (tag_of(e) == WT_EXN) {
    r = exn_from_js(ctx, wx, e);
    if (!r) {
      if (!ctx->has_exc) {
        ctx->has_exc = 1;
        ctx->exc = e;
      }
      return 0;
    }
  } else {
    r = wasm_exn_new(wx->x, NULL, 0);
    if (!r) {
      ctx->has_exc = 1;
      ctx->exc = e;
      return 0;
    }
    ext_encode(e, &r->js);
  }
  *out = r;
  return 1;
}

static int atomic_wait_hook(void *ud, uint8_t *addr, int is64, uint64_t expected, int64_t timeout)
{
  wexec *wx = (wexec *)ud;
  js_ctx *ctx = wx->ctx;
  unsigned *word = (unsigned *)addr, waited = 0, limit;
  js_irq *irq = ctx->irq;
  uint64_t now =
      is64 ? __atomic_load_n((uint64_t *)addr, __ATOMIC_SEQ_CST) : __atomic_load_n((uint32_t *)addr, __ATOMIC_SEQ_CST);
  if (!ctx->can_block)
    return -1;
  if (now != (is64 ? expected : (uint32_t)expected))
    return 1;
  if (timeout == 0)
    return 2;
  /* 0 means no limit; whole milliseconds, rounded up */
  limit = timeout < 0 || timeout > 4294967294000000ll ? 0 : (unsigned)((timeout + 999999) / 1000000);
  if (!irq)
    return futex_wait(word, (unsigned)expected, limit) ? 2 : 0;
  __atomic_store_n(&irq->waiting, word, __ATOMIC_RELEASE);
  for (;;) {
    unsigned slice = 50;
    if (js_irq_stopped(irq) || (limit && waited >= limit))
      break;
    if (limit && limit - waited < slice)
      slice = limit - waited;
    if (!futex_wait(word, (unsigned)expected, slice)) {
      __atomic_store_n(&irq->waiting, (unsigned *)0, __ATOMIC_RELEASE);
      return 0;
    }
    waited += slice;
  }
  __atomic_store_n(&irq->waiting, (unsigned *)0, __ATOMIC_RELEASE);
  return 2;
}

static uint32_t atomic_notify_hook(void *ud, uint8_t *addr, uint32_t count)
{
  (void)ud;
  return (uint32_t)futex_wake((unsigned *)addr, count > 2147483647u ? 2147483647 : (int)count);
}

static wexec *get_exec(js_ctx *ctx)
{
  pentry *e = pmap_find_pub(ctx->global, "@wasmx");
  wexec *wx;
  js_object *o;
  if (e && e->value.type == JV_OBJ)
    return (wexec *)e->value.u.obj->host;
  wx = (wexec *)calloc(1, sizeof *wx);
  if (wx) {
    wx->tag = WT_EXEC;
    wx->ctx = ctx;
    wx->x = wasm_exec_new(wx);
    if (!wx->x) {
      free(wx);
      wx = NULL;
    }
  }
  if (!wx) {
    js_throw_range(ctx, "WebAssembly: out of memory");
    return NULL;
  }
  wx->x->host_call = host_call;
  wx->x->xr_intern = xr_intern;
  wx->x->xr_value = xr_value;
  wx->x->exn_catch = exn_catch;
  wx->x->atomic_wait = atomic_wait_hook;
  wx->x->atomic_notify = atomic_notify_hook;
  o = new_wasm(ctx, "@Exec", wx);
  js_def(ctx->global, "@wasmx", js_obj_value(o), 0);
  return wx;
}

static void exec_leave(wexec *wx)
{
  if (wx->depth == 0) {
    wasm_exec_reset(wx->x);
    wx->nxs = 0;
    if (wx->htcap)
      memset(wx->ht, 0, wx->htcap * sizeof *wx->ht);
  }
}

/* Run a wasm function; 0 on success, else a JS exception is pending */
static int invoke(js_ctx *ctx, wexec *wx, wasm_func *wf, const uint64_t *args, uint64_t *res)
{
  int r;
  wx->depth++;
  r = wasm_call(wx->x, wf, args, res);
  wx->depth--;
  if (r == -1) {
    const char *msg = wx->x->trap ? wx->x->trap : "unreachable";
    if (!strcmp(msg, "call stack exhausted"))
      js_throw_range(ctx, "Maximum call stack size exceeded");
    else
      throw_wasm(ctx, "RuntimeError", msg);
  } else if (r == -3)
    js_throw(ctx, exn_to_js(ctx, wx, wx->x->exc));
  return r;
}

static js_value wasm_call_native(js_ctx *ctx, js_value thisv, js_value *a, int n)
{
  wasm_func *wf = (wasm_func *)(uintptr_t)ctx->native_data.u.num;
  const wasm_functype *t = wf->type;
  wexec *wx = get_exec(ctx);
  uint64_t *buf, *res;
  uint32_t i, pos = 0;
  js_value out = js_undef();
  (void)thisv;
  if (!wx)
    return js_undef();
  buf = (uint64_t *)malloc((t->pslots + t->rslots + 2) * sizeof *buf);
  if (!buf)
    return js_throw_range(ctx, "WebAssembly: out of memory");
  res = buf + t->pslots;
  gc_push_scope();
  gc_keep(wf->inst->self);
  for (i = 0; i < t->np; i++) {
    if (!to_wasm(ctx, wx, ARG((int)i), t->p[i], buf + pos))
      goto done;
    pos += slots_of(t->p[i]);
  }
  if (invoke(ctx, wx, wf, buf, res) != 0)
    goto done;
  if (t->nr == 1)
    out = from_wasm(ctx, wx, res, t->r[0]);
  else if (t->nr > 1) {
    js_object *arr = js_new_array(ctx, 0);
    gc_keep(arr);
    pos = 0;
    for (i = 0; i < t->nr; i++) {
      js_value v = from_wasm(ctx, wx, res + pos, t->r[i]);
      if (v.type == JV_OBJ || v.type == JV_BIGINT)
        gc_keep(v.u.obj);
      js_array_push(ctx, arr, v);
      pos += slots_of(t->r[i]);
    }
    out = js_obj_value(arr);
  }
done:
  gc_pop_scope();
  exec_leave(wx);
  free(buf);
  return ctx->has_exc ? js_undef() : out;
}

static js_value func_to_js(js_ctx *ctx, wasm_func *wf)
{
  js_object *fn;
  char name[12] = "";
  if (wf->js)
    return js_obj_value((js_object *)wf->js);
  mcat_u(name, sizeof name, wf->index);
  gc_push_scope();
  gc_keep(wf->inst->self);
  fn = js_new_native(ctx, wasm_call_native, "", (int)wf->type->np);
  gc_keep(fn);
  js_def(fn, "name", js_mk_cstring(ctx, name), P_CONFIGURABLE);
  fn->fn->data = js_number((double)(uintptr_t)wf);
  fn->fn->home = (js_object *)wf->inst->self;
  wf->js = fn;
  gc_pop_scope();
  return js_obj_value(fn);
}

/* ---- modules ------------------------------------------------------------- */

static void *host_of(js_value v, int tag) { return tag_of(v) == tag ? v.u.obj->host : NULL; }

static int get_bytes(js_value v, const uint8_t **p, size_t *len)
{
  static const uint8_t none[1] = {0};
  js_arraybuffer *ab;
  if (v.type != JV_OBJ)
    return 0;
  if (v.u.obj->kind == OBJ_ARRAYBUFFER && v.u.obj->host) {
    ab = (js_arraybuffer *)v.u.obj->host;
    *p = ab->data ? ab->data : none;
    *len = (size_t)ab->byteLength;
    return 1;
  }
  if (v.u.obj->kind == OBJ_TYPEDARRAY && v.u.obj->host) {
    js_typedarray *ta = (js_typedarray *)v.u.obj->host;
    ab = (js_arraybuffer *)ta->buffer->host;
    *p = ab && ab->data ? ab->data + ta->offset : none;
    *len = (size_t)ta_length(ta) * (size_t)(ta->type >= 0 ? ta_elem_size(ta->type) : 1);
    return 1;
  }
  return 0;
}

static void api_error(js_ctx *ctx, const char *kind, const char *api, const char *msg)
{
  char buf[320] = "";
  mcat(buf, sizeof buf, api);
  mcat(buf, sizeof buf, ": ");
  mcat(buf, sizeof buf, msg);
  if (!strcmp(kind, "TypeError"))
    js_throw_type(ctx, buf);
  else if (!strcmp(kind, "RangeError"))
    js_throw_range(ctx, buf);
  else
    throw_wasm(ctx, kind, buf);
}

static js_object *compile_module(js_ctx *ctx, js_value src, const char *api)
{
  const uint8_t *p;
  size_t len;
  char err[200];
  wmodule *wm;
  wasm_module *m;
  if (!get_bytes(src, &p, &len)) {
    api_error(ctx, "TypeError", api, "Argument 0 must be a buffer source");
    return NULL;
  }
  if (len == 0) {
    api_error(ctx, "CompileError", api, "BufferSource argument is empty");
    return NULL;
  }
  m = wasm_module_new(p, len, err, sizeof err);
  if (!m) {
    api_error(ctx, "CompileError", api, err);
    return NULL;
  }
  wm = (wmodule *)calloc(1, sizeof *wm);
  if (!wm) {
    wasm_module_free(m);
    js_throw_range(ctx, "WebAssembly: out of memory");
    return NULL;
  }
  wm->tag = WT_MODULE;
  wm->m = m;
  {
    js_object *o = new_wasm(ctx, "@Module", wm);
    gc_set_external(o, (unsigned)(len * 4));
    return o;
  }
}

static js_value module_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *o = compile_module(ctx, ARG(0), "WebAssembly.Module()");
  (void)t;
  return o ? js_obj_value(o) : js_undef();
}

static const char *const KIND_NAME[5] = {"function", "table", "memory", "global", "tag"};

static wasm_module *module_arg(js_ctx *ctx, js_value v, const char *api)
{
  wmodule *wm = (wmodule *)host_of(v, WT_MODULE);
  if (!wm)
    api_error(ctx, "TypeError", api, "Argument 0 must be a WebAssembly.Module");
  return wm ? wm->m : NULL;
}

static js_value module_imports(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wasm_module *m = module_arg(ctx, ARG(0), "WebAssembly.Module.imports()");
  js_object *arr;
  uint32_t i;
  (void)t;
  if (!m)
    return js_undef();
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < m->nimports; i++) {
    js_object *d = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(d, "module", js_mk_cstring(ctx, m->imports[i].module), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "name", js_mk_cstring(ctx, m->imports[i].name), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "kind", js_mk_cstring(ctx, KIND_NAME[m->imports[i].kind]), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_array_push(ctx, arr, js_obj_value(d));
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

static js_value module_exports(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wasm_module *m = module_arg(ctx, ARG(0), "WebAssembly.Module.exports()");
  js_object *arr;
  uint32_t i;
  (void)t;
  if (!m)
    return js_undef();
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < m->nexports; i++) {
    js_object *d = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(d, "name", js_mk_cstring(ctx, m->exports[i].name), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_def(d, "kind", js_mk_cstring(ctx, KIND_NAME[m->exports[i].kind]), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    js_array_push(ctx, arr, js_obj_value(d));
  }
  gc_pop_scope();
  return js_obj_value(arr);
}

static js_value module_custom_sections(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wasm_module *m = module_arg(ctx, ARG(0), "WebAssembly.Module.customSections()");
  js_value name;
  js_object *arr;
  uint32_t i;
  (void)t;
  if (!m)
    return js_undef();
  if (n < 2 || a[1].type == JV_UNDEF)
    return js_throw_type(ctx, "WebAssembly.Module.customSections(): Argument 1 is required");
  name = js_to_string(ctx, a[1]);
  if (ctx->has_exc)
    return js_undef();
  gc_push_scope();
  gc_keep(name.u.str);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < m->ncustom; i++)
    if (!strcmp(m->custom[i].name, name.u.str->data))
      js_array_push(ctx, arr, js_new_arraybuffer(ctx, m->custom[i].bytes, (int)m->custom[i].len));
  gc_pop_scope();
  return js_obj_value(arr);
}

/* ---- memories, tables, globals ------------------------------------------- */

static void mem_on_grow(wasm_memory *m)
{
  wmemory *wm = m->self ? (wmemory *)((js_object *)m->self)->host : NULL;
  if (wm && wm->buf && !m->shared) {
    js_arraybuffer_detach(wm->buf);
    wm->buf = NULL;
  }
  if (m->self)
    gc_set_external(m->self, (unsigned)wasm_memory_size(m));
}

static js_object *wrap_memory(js_ctx *ctx, wasm_memory *m)
{
  wmemory *wm = (wmemory *)calloc(1, sizeof *wm);
  js_object *o;
  if (!wm)
    gc_oom();
  wm->tag = WT_MEMORY;
  wm->mem = m;
  m->on_grow = mem_on_grow;
  o = new_wasm(ctx, "@Memory", wm);
  m->self = o;
  gc_set_external(o, (unsigned)wasm_memory_size(m));
  return o;
}

static js_object *wrap_table(js_ctx *ctx, wasm_table *t)
{
  wtable *wt = (wtable *)calloc(1, sizeof *wt);
  js_object *o;
  if (!wt)
    gc_oom();
  wt->tag = WT_TABLE;
  wt->tab = t;
  o = new_wasm(ctx, "@Table", wt);
  t->self = o;
  return o;
}

static js_object *wrap_global(js_ctx *ctx, wasm_global *g)
{
  wglobal *wg = (wglobal *)calloc(1, sizeof *wg);
  js_object *o;
  if (!wg)
    gc_oom();
  wg->tag = WT_GLOBAL;
  wg->g = g;
  o = new_wasm(ctx, "@Global", wg);
  g->self = o;
  return o;
}

/* Read a limit from a descriptor. 1 present, 0 absent, -1 an exception is pending */
static int desc_limit(js_ctx *ctx, js_value d, const char *key, const char *alt, double cap, const char *api,
                      uint32_t *out)
{
  js_value v = js_get(ctx, d, key);
  double x;
  char buf[96] = "Property '";
  if (v.type == JV_UNDEF && alt)
    v = js_get(ctx, d, alt);
  if (ctx->has_exc)
    return -1;
  if (v.type == JV_UNDEF)
    return 0;
  x = js_to_number(ctx, v);
  if (ctx->has_exc)
    return -1;
  mcat(buf, sizeof buf, key);
  if (x != x || x < 0 || x > 4294967295.0) {
    mcat(buf, sizeof buf, "': value must be convertible to a valid number");
    api_error(ctx, "TypeError", api, buf);
    return -1;
  }
  x = (double)(uint32_t)x;
  if (x > cap) {
    mcat(buf, sizeof buf, "': value is above the upper bound");
    api_error(ctx, "RangeError", api, buf);
    return -1;
  }
  *out = (uint32_t)x;
  return 1;
}

/* A 64-bit memory's limits are BigInts. 1 present, 0 absent, -1 an exception is pending */
static int desc_big(js_ctx *ctx, js_value d, const char *key, const char *alt, uint64_t *out)
{
  js_value v = js_get(ctx, d, key);
  unsigned lo, hi;
  if (v.type == JV_UNDEF && alt)
    v = js_get(ctx, d, alt);
  if (ctx->has_exc)
    return -1;
  if (v.type == JV_UNDEF)
    return 0;
  if (!js_bigint_to_u64(ctx, v, &lo, &hi))
    return -1;
  *out = (uint64_t)lo | ((uint64_t)hi << 32);
  return 1;
}

static js_value memory_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *api = "WebAssembly.Memory()";
  uint32_t init = 0, max = 0;
  uint64_t init64 = 0, max64 = 0;
  int r, has_max, is64 = 0, shared;
  js_value av;
  wasm_memory *m;
  (void)t;
  if (ARG(0).type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Memory(): Argument 0 must be a memory descriptor");
  shared = js_to_boolean(js_get(ctx, a[0], "shared"));
  av = js_get(ctx, a[0], "address");
  if (av.type != JV_UNDEF) {
    av = js_to_string(ctx, av);
    if (ctx->has_exc)
      return js_undef();
    is64 = !strcmp(av.u.str->data, "i64");
    if (!is64 && strcmp(av.u.str->data, "i32"))
      return js_throw_type(ctx, "WebAssembly.Memory(): Descriptor property 'address' must be 'i32' or 'i64'");
  }
  if (is64) {
    r = desc_big(ctx, a[0], "initial", "minimum", &init64);
    if (r < 0)
      return js_undef();
    if (r == 0)
      return js_throw_type(ctx, "WebAssembly.Memory(): Property 'initial' is required");
    if (init64 > WASM_MAX_PAGES)
      return js_throw_range(ctx, "WebAssembly.Memory(): Property 'initial': value is above the upper bound");
    init = (uint32_t)init64;
    has_max = desc_big(ctx, a[0], "maximum", NULL, &max64);
    if (has_max < 0)
      return js_undef();
    if (has_max && max64 < init64)
      return js_throw_range(ctx, "WebAssembly.Memory(): Property 'maximum': value is below the lower bound");
    max = max64 > WASM_MAX_PAGES ? WASM_MAX_PAGES : (uint32_t)max64;
    goto limits_done;
  }
  r = desc_limit(ctx, a[0], "initial", "minimum", WASM_MAX_PAGES, api, &init);
  if (r < 0)
    return js_undef();
  if (r == 0)
    return js_throw_type(ctx, "WebAssembly.Memory(): Property 'initial' is required");
  has_max = desc_limit(ctx, a[0], "maximum", NULL, 4294967295.0, api, &max);
  if (has_max < 0)
    return js_undef();
  if (has_max && max < init)
    return js_throw_range(ctx, "WebAssembly.Memory(): Property 'maximum': value is below the lower bound");
  if (has_max && max > WASM_MAX_PAGES)
    max = WASM_MAX_PAGES;
limits_done:
  if (shared && !has_max)
    return js_throw_type(ctx, "WebAssembly.Memory(): Property 'maximum' is required for shared memory");
  m = (wasm_memory *)calloc(1, sizeof *m);
  if (m)
    m->is64 = (uint8_t)is64;
  if (!m || (shared ? wasm_memory_alloc_shared(m, init, max) : wasm_memory_alloc(m, init))) {
    free(m);
    return js_throw_range(ctx, "WebAssembly.Memory(): could not allocate memory");
  }
  if (!shared) {
    m->has_max = (uint8_t)has_max;
    m->max = max;
  }
  return js_obj_value(wrap_memory(ctx, m));
}

static js_value memory_buffer(js_ctx *ctx, js_value t, js_value *a, int n)
{
  static unsigned char empty[1];
  wmemory *wm = (wmemory *)host_of(t, WT_MEMORY);
  (void)a;
  (void)n;
  if (!wm)
    return js_throw_type(ctx, "WebAssembly.Memory.buffer: Receiver is not a WebAssembly.Memory");
  if (wm->mem->shared) {
    uint32_t pages = wasm_memory_pages(wm->mem);
    if (!wm->buf || wm->buf_pages != pages) {
      js_value b = js_sab_wrap(ctx, wm->mem->data, (int)(pages * WASM_PAGE));
      if (ctx->has_exc)
        return js_undef();
      wm->buf = b.u.obj;
      wm->buf_pages = pages;
    }
  } else if (!wm->buf) {
    wasm_memory *m = wm->mem;
    js_value b =
        js_arraybuffer_external(ctx, m->data ? m->data : empty, (int)wasm_memory_size(m), (js_object *)m->self);
    if (ctx->has_exc)
      return js_undef();
    wm->buf = b.u.obj;
  }
  return js_obj_value(wm->buf);
}

static int arg_u32(js_ctx *ctx, js_value v, const char *api, uint32_t *out)
{
  double x = js_to_number(ctx, v);
  if (ctx->has_exc)
    return 0;
  if (x != x || x < 0 || x > 4294967295.0) {
    api_error(ctx, "TypeError", api, "Argument 0 must be convertible to a valid number");
    return 0;
  }
  *out = (uint32_t)x;
  return 1;
}

static js_value memory_grow(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wmemory *wm = (wmemory *)host_of(t, WT_MEMORY);
  uint32_t delta;
  int64_t r;
  if (!wm)
    return js_throw_type(ctx, "WebAssembly.Memory.grow(): Receiver is not a WebAssembly.Memory");
  if (wm->mem->is64) {
    unsigned lo, hi;
    if (!js_bigint_to_u64(ctx, ARG(0), &lo, &hi))
      return js_undef();
    r = hi ? -1 : wasm_memory_grow(wm->mem, lo);
    if (r < 0)
      return js_throw_range(ctx, "WebAssembly.Memory.grow(): Maximum memory size exceeded");
    return js_bigint_from_u64(ctx, (unsigned)r, 0, 0);
  }
  if (!arg_u32(ctx, ARG(0), "WebAssembly.Memory.grow()", &delta))
    return js_undef();
  r = wasm_memory_grow(wm->mem, delta);
  if (r < 0)
    return js_throw_range(ctx, "WebAssembly.Memory.grow(): Maximum memory size exceeded");
  return js_number((double)r);
}

static js_value table_init_value(js_ctx *ctx, js_value v, int type, int given, wasm_func **f, wasm_extern *x,
                                 const char *api)
{
  memset(x, 0, sizeof *x);
  *f = NULL;
  if (type == WASM_FUNCREF) {
    if (v.type == JV_UNDEF || v.type == JV_NULL)
      return js_undef();
    *f = func_of(v);
    if (!*f)
      api_error(ctx, "TypeError", api, "Argument 1 is invalid for table: function-typed object expected");
  } else
    ext_encode(given ? v : js_undef(), x);
  return js_undef();
}

static js_value table_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *api = "WebAssembly.Table()";
  uint32_t init = 0, max = 0;
  int r, has_max, type;
  js_value el;
  wasm_table *tab;
  wasm_func *f;
  wasm_extern x;
  (void)t;
  if (ARG(0).type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Table(): Argument 0 must be a table descriptor");
  el = js_to_string(ctx, js_get(ctx, a[0], "element"));
  if (ctx->has_exc)
    return js_undef();
  if (!strcmp(el.u.str->data, "anyfunc") || !strcmp(el.u.str->data, "funcref"))
    type = WASM_FUNCREF;
  else if (!strcmp(el.u.str->data, "externref"))
    type = WASM_EXTERNREF;
  else
    return js_throw_type(ctx,
                         "WebAssembly.Table(): Descriptor property 'element' must be a WebAssembly reference type");
  r = desc_limit(ctx, a[0], "initial", "minimum", WASM_MAX_TABLE, api, &init);
  if (r < 0)
    return js_undef();
  if (r == 0)
    return js_throw_type(ctx, "WebAssembly.Table(): Property 'initial' is required");
  has_max = desc_limit(ctx, a[0], "maximum", NULL, 4294967295.0, api, &max);
  if (has_max < 0)
    return js_undef();
  if (has_max && max < init)
    return js_throw_range(ctx, "WebAssembly.Table(): Property 'maximum': value is below the lower bound");
  table_init_value(ctx, ARG(1), type, n > 1, &f, &x, "WebAssembly.Table()");
  if (ctx->has_exc)
    return js_undef();
  tab = (wasm_table *)calloc(1, sizeof *tab);
  if (!tab)
    return js_throw_range(ctx, "WebAssembly.Table(): could not allocate table");
  tab->type = (uint8_t)type;
  tab->has_max = (uint8_t)has_max;
  tab->max = max;
  if (wasm_table_alloc(tab, init)) {
    free(tab);
    return js_throw_range(ctx, "WebAssembly.Table(): could not allocate table");
  }
  for (r = 0; (uint32_t)r < init; r++) {
    if (type == WASM_FUNCREF)
      tab->f[r] = f;
    else
      tab->x[r] = x;
  }
  return js_obj_value(wrap_table(ctx, tab));
}

static js_value table_length(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wtable *wt = (wtable *)host_of(t, WT_TABLE);
  (void)a;
  (void)n;
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Table.length: Receiver is not a WebAssembly.Table");
  return js_number((double)wt->tab->n);
}

static js_value table_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wtable *wt = (wtable *)host_of(t, WT_TABLE);
  uint32_t i;
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Table.get(): Receiver is not a WebAssembly.Table");
  if (!arg_u32(ctx, ARG(0), "WebAssembly.Table.get()", &i))
    return js_undef();
  if (i >= wt->tab->n)
    return js_throw_range(ctx, "WebAssembly.Table.get(): invalid address in table");
  if (wt->tab->type == WASM_FUNCREF)
    return wt->tab->f[i] ? func_to_js(ctx, wt->tab->f[i]) : js_null();
  return ext_decode(&wt->tab->x[i]);
}

static js_value table_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wtable *wt = (wtable *)host_of(t, WT_TABLE);
  uint32_t i;
  wasm_func *f;
  wasm_extern x;
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Table.set(): Receiver is not a WebAssembly.Table");
  if (!arg_u32(ctx, ARG(0), "WebAssembly.Table.set()", &i))
    return js_undef();
  if (i >= wt->tab->n)
    return js_throw_range(ctx, "WebAssembly.Table.set(): invalid address in table");
  table_init_value(ctx, ARG(1), wt->tab->type, n > 1, &f, &x, "WebAssembly.Table.set()");
  if (ctx->has_exc)
    return js_undef();
  if (wt->tab->type == WASM_FUNCREF)
    wt->tab->f[i] = f;
  else
    wt->tab->x[i] = x;
  return js_undef();
}

static js_value table_grow(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wtable *wt = (wtable *)host_of(t, WT_TABLE);
  uint32_t delta;
  wasm_func *f;
  wasm_extern x;
  int64_t r;
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Table.grow(): Receiver is not a WebAssembly.Table");
  if (!arg_u32(ctx, ARG(0), "WebAssembly.Table.grow()", &delta))
    return js_undef();
  table_init_value(ctx, ARG(1), wt->tab->type, n > 1, &f, &x, "WebAssembly.Table.grow()");
  if (ctx->has_exc)
    return js_undef();
  r = wasm_table_grow(wt->tab, delta, f, &x);
  if (r < 0)
    return js_throw_range(ctx, "WebAssembly.Table.grow(): failed to grow table");
  return js_number((double)r);
}

static int parse_valtype(const char *s)
{
  if (!strcmp(s, "i32"))
    return WASM_I32;
  if (!strcmp(s, "i64"))
    return WASM_I64;
  if (!strcmp(s, "f32"))
    return WASM_F32;
  if (!strcmp(s, "f64"))
    return WASM_F64;
  if (!strcmp(s, "v128"))
    return WASM_V128;
  if (!strcmp(s, "anyfunc") || !strcmp(s, "funcref"))
    return WASM_FUNCREF;
  if (!strcmp(s, "externref"))
    return WASM_EXTERNREF;
  return 0;
}

/* The JS value to store in a global of this type, as raw slots */
static int global_from_js(js_ctx *ctx, js_value v, int given, int type, uint64_t out[2])
{
  out[0] = out[1] = 0;
  if (type == WASM_V128) {
    js_throw_type(ctx, "type incompatibility when transforming from/to JS");
    return 0;
  }
  if (type == WASM_EXTERNREF) {
    ext_encode(given ? v : js_undef(), (wasm_extern *)out);
    return 1;
  }
  if (!given || (type == WASM_FUNCREF && v.type == JV_UNDEF))
    return 1;
  return to_wasm(ctx, NULL, v, type, out);
}

static js_value global_to_js(js_ctx *ctx, wasm_global *g)
{
  if (g->type == WASM_V128)
    return js_throw_type(ctx, "type incompatibility when transforming from/to JS");
  if (g->type == WASM_EXTERNREF)
    return ext_decode((const wasm_extern *)g->v);
  return from_wasm(ctx, NULL, g->v, g->type);
}

static js_value global_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value tv;
  int type;
  wasm_global *g;
  (void)t;
  if (ARG(0).type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Global(): Argument 0 must be a global descriptor");
  tv = js_to_string(ctx, js_get(ctx, a[0], "value"));
  if (ctx->has_exc)
    return js_undef();
  type = parse_valtype(tv.u.str->data);
  if (!type)
    return js_throw_type(ctx, "WebAssembly.Global(): Descriptor property 'value' must be a WebAssembly type");
  g = (wasm_global *)calloc(1, sizeof *g);
  if (!g)
    return js_throw_range(ctx, "WebAssembly.Global(): out of memory");
  g->type = (uint8_t)type;
  g->mut = (uint8_t)js_to_boolean(js_get(ctx, a[0], "mutable"));
  if (ctx->has_exc || (type != WASM_V128 && !global_from_js(ctx, ARG(1), n > 1, type, g->v))) {
    free(g);
    return js_undef();
  }
  return js_obj_value(wrap_global(ctx, g));
}

static js_value global_value_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wglobal *wg = (wglobal *)host_of(t, WT_GLOBAL);
  (void)a;
  (void)n;
  if (!wg)
    return js_throw_type(ctx, "WebAssembly.Global.value: Receiver is not a WebAssembly.Global");
  return global_to_js(ctx, wg->g);
}

static js_value global_value_set(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wglobal *wg = (wglobal *)host_of(t, WT_GLOBAL);
  uint64_t v[2];
  if (!wg)
    return js_throw_type(ctx, "WebAssembly.Global.value: Receiver is not a WebAssembly.Global");
  if (!wg->g->mut)
    return js_throw_type(ctx, "WebAssembly.Global.value: Can't set the value of an immutable global.");
  if (n < 1)
    return js_throw_type(ctx, "WebAssembly.Global.value: Argument 0 is required");
  if (!global_from_js(ctx, a[0], 1, wg->g->type, v))
    return js_undef();
  wg->g->v[0] = v[0];
  wg->g->v[1] = v[1];
  return js_undef();
}

static js_object *wrap_tag(js_ctx *ctx, wasm_tag *t)
{
  wtagw *wt = (wtagw *)calloc(1, sizeof *wt);
  js_object *o;
  if (!wt)
    gc_oom();
  wt->tag = WT_TAG;
  wt->t = t;
  o = new_wasm(ctx, "@Tag", wt);
  t->self = o;
  return o;
}

static js_value exn_to_js(js_ctx *ctx, wexec *wx, wasm_exn *e)
{
  wexn *we;
  js_object *vals, *o;
  uint32_t i, pos = 0;
  if (!e->tag)
    return ext_decode(&e->js);
  we = (wexn *)calloc(1, sizeof *we);
  if (!we)
    gc_oom();
  we->tag = WT_EXN;
  gc_push_scope();
  vals = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < e->tag->np; i++) {
    js_value v = from_wasm(ctx, wx, e->slots + pos, e->tag->types[i]);
    if (v.type == JV_OBJ || v.type == JV_BIGINT)
      gc_keep(v.u.obj);
    else if (v.type == JV_STR)
      gc_keep(v.u.str);
    js_array_push(ctx, vals, v);
    pos += slots_of(e->tag->types[i]);
  }
  we->tagobj = (js_object *)e->tag->self;
  we->vals = vals;
  o = new_wasm(ctx, "@Exception", we);
  gc_pop_scope();
  return js_obj_value(o);
}

static wasm_exn *exn_from_js(js_ctx *ctx, wexec *wx, js_value v)
{
  wexn *we = (wexn *)host_of(v, WT_EXN);
  wasm_tag *tg;
  wasm_exn *e;
  uint32_t i, pos = 0;
  if (!we)
    return NULL;
  tg = ((wtagw *)we->tagobj->host)->t;
  e = wasm_exn_new(wx->x, tg, tg->pslots);
  if (!e) {
    js_throw_range(ctx, "WebAssembly: out of memory");
    return NULL;
  }
  for (i = 0; i < tg->np; i++) {
    if (!to_wasm(ctx, wx, js_get_index(ctx, js_obj_value(we->vals), i), tg->types[i], e->slots + pos))
      return NULL;
    pos += slots_of(tg->types[i]);
  }
  return e;
}

/* The value a JS argument becomes in a tag's payload */
static int canon_arg(js_ctx *ctx, js_value v, int type, js_value *out)
{
  switch (type) {
  case WASM_I32:
    *out = js_number((double)js_to_int32(ctx, v));
    return !ctx->has_exc;
  case WASM_I64: {
    unsigned lo, hi;
    if (!js_bigint_to_u64(ctx, v, &lo, &hi))
      return 0;
    *out = js_bigint_from_u64(ctx, lo, hi, 1);
    return !ctx->has_exc;
  }
  case WASM_F32:
    *out = js_number((double)(float)js_to_number(ctx, v));
    return !ctx->has_exc;
  case WASM_F64:
    *out = js_number(js_to_number(ctx, v));
    return !ctx->has_exc;
  case WASM_FUNCREF:
    if (v.type != JV_NULL && !func_of(v)) {
      js_throw_type(ctx, "WebAssembly.Exception: function reference must be null or an exported WebAssembly function");
      return 0;
    }
    *out = v;
    return 1;
  case WASM_EXTERNREF:
    *out = v;
    return 1;
  }
  js_throw_type(ctx, "WebAssembly.Exception: type incompatibility when transforming from/to JS");
  return 0;
}

static js_value tag_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value list, len;
  uint8_t types[64];
  uint32_t np, i;
  wasm_tag *tg;
  (void)t;
  if (ARG(0).type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Tag(): Argument 0 must be a tag type");
  list = js_get(ctx, a[0], "parameters");
  if (list.type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Tag(): Argument 0 must be a tag type with 'parameters'");
  len = js_get(ctx, list, "length");
  np = (uint32_t)js_to_number(ctx, len);
  if (ctx->has_exc)
    return js_undef();
  if (np > sizeof types)
    return js_throw_range(ctx, "WebAssembly.Tag(): too many parameters");
  for (i = 0; i < np; i++) {
    js_value s = js_to_string(ctx, js_get_index(ctx, list, i));
    if (ctx->has_exc)
      return js_undef();
    types[i] = (uint8_t)parse_valtype(s.u.str->data);
    if (!types[i] || types[i] == WASM_V128)
      return js_throw_type(ctx, "WebAssembly.Tag(): Argument 0 parameter type at index #0 must be a value type");
  }
  tg = (wasm_tag *)calloc(1, sizeof *tg);
  if (!tg || wasm_tag_new(tg, types, np)) {
    free(tg);
    return js_throw_range(ctx, "WebAssembly.Tag(): out of memory");
  }
  return js_obj_value(wrap_tag(ctx, tg));
}

static js_value exception_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wtagw *wt = (wtagw *)host_of(ARG(0), WT_TAG);
  wexn *we;
  js_object *vals, *o;
  uint32_t i;
  js_value len;
  (void)t;
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Exception(): Argument 0 must be a WebAssembly tag");
  if (ARG(1).type != JV_OBJ)
    return js_throw_type(ctx, "WebAssembly.Exception(): Argument 1 must be an iterable");
  len = js_get(ctx, a[1], "length");
  if (ctx->has_exc)
    return js_undef();
  if (js_to_number(ctx, len) != (double)wt->t->np)
    return js_throw_type(ctx, "WebAssembly.Exception(): Number of exception values does not match signature length");
  gc_push_scope();
  vals = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < wt->t->np; i++) {
    js_value v;
    if (!canon_arg(ctx, js_get_index(ctx, a[1], i), wt->t->types[i], &v)) {
      gc_pop_scope();
      return js_undef();
    }
    if (v.type == JV_OBJ || v.type == JV_BIGINT)
      gc_keep(v.u.obj);
    else if (v.type == JV_STR)
      gc_keep(v.u.str);
    js_array_push(ctx, vals, v);
  }
  we = (wexn *)calloc(1, sizeof *we);
  if (!we)
    gc_oom();
  we->tag = WT_EXN;
  we->tagobj = a[0].u.obj;
  we->vals = vals;
  o = new_wasm(ctx, "@Exception", we);
  gc_pop_scope();
  return js_obj_value(o);
}

static js_value exception_is(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wexn *we = (wexn *)host_of(t, WT_EXN);
  wtagw *wt = (wtagw *)host_of(ARG(0), WT_TAG);
  if (!we)
    return js_throw_type(ctx, "WebAssembly.Exception.is(): Receiver is not a WebAssembly.Exception");
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Exception.is(): Argument 0 must be a WebAssembly tag");
  return js_bool(((wtagw *)we->tagobj->host)->t == wt->t);
}

static js_value exception_get_arg(js_ctx *ctx, js_value t, js_value *a, int n)
{
  wexn *we = (wexn *)host_of(t, WT_EXN);
  wtagw *wt = (wtagw *)host_of(ARG(0), WT_TAG);
  uint32_t i;
  if (!we)
    return js_throw_type(ctx, "WebAssembly.Exception.getArg(): Receiver is not a WebAssembly.Exception");
  if (!wt)
    return js_throw_type(ctx, "WebAssembly.Exception.getArg(): Argument 0 must be a WebAssembly tag");
  if (((wtagw *)we->tagobj->host)->t != wt->t)
    return js_throw_type(ctx, "WebAssembly.Exception.getArg(): First argument does not match the exception tag");
  if (!arg_u32(ctx, ARG(1), "WebAssembly.Exception.getArg()", &i))
    return js_undef();
  if (i >= wt->t->np)
    return js_throw_range(ctx, "WebAssembly.Exception.getArg(): Index out of range");
  return js_get_index(ctx, js_obj_value(we->vals), i);
}

/* ---- instances ----------------------------------------------------------- */

static js_object *make_global(js_ctx *ctx, int type, int mut, const uint64_t v[2])
{
  wasm_global *g = (wasm_global *)calloc(1, sizeof *g);
  if (!g)
    gc_oom();
  g->type = (uint8_t)type;
  g->mut = (uint8_t)mut;
  g->v[0] = v[0];
  g->v[1] = v[1];
  return wrap_global(ctx, g);
}

static void link_error(js_ctx *ctx, const char *api, uint32_t idx, const wasm_import *im, int with_name,
                       const char *msg)
{
  char buf[400] = "";
  mcat(buf, sizeof buf, api);
  mcat(buf, sizeof buf, ": Import #");
  mcat_u(buf, sizeof buf, idx);
  mcat(buf, sizeof buf, " \"");
  mcat(buf, sizeof buf, im->module);
  mcat(buf, sizeof buf, "\"");
  if (with_name) {
    mcat(buf, sizeof buf, " \"");
    mcat(buf, sizeof buf, im->name);
    mcat(buf, sizeof buf, "\"");
  }
  mcat(buf, sizeof buf, ": ");
  mcat(buf, sizeof buf, msg);
  if (with_name)
    throw_wasm(ctx, "LinkError", buf);
  else
    js_throw_type(ctx, buf);
}

/* Resolve one import into the instance; 0 with an exception pending on failure */
static int resolve_import(js_ctx *ctx, wasm_instance *in, const wasm_module *m, uint32_t k, js_value imports,
                          uint32_t *cnt, const char *api)
{
  const wasm_import *im = &m->imports[k];
  js_value modv = js_get(ctx, imports, im->module), v;
  if (ctx->has_exc)
    return 0;
  if (modv.type != JV_OBJ) {
    link_error(ctx, api, k, im, 0, "module is not an object or function");
    return 0;
  }
  v = js_get(ctx, modv, im->name);
  if (ctx->has_exc)
    return 0;
  if (v.type == JV_OBJ)
    gc_keep(v.u.obj);
  else if (v.type == JV_STR)
    gc_keep(v.u.str);
  switch (im->kind) {
  case WASM_KIND_FUNC: {
    const wasm_functype *want = &m->types[im->type];
    wasm_func *other, *s;
    uint32_t fi = cnt[WASM_KIND_FUNC]++;
    if (!js_is_callable(v)) {
      link_error(ctx, api, k, im, 1, "function import requires a callable");
      return 0;
    }
    other = func_of(v);
    if (other) {
      if (!wasm_functype_eq(other->type, want)) {
        link_error(ctx, api, k, im, 1, "imported function does not match the expected type");
        return 0;
      }
      in->funcs[fi] = other;
    } else {
      s = &in->store[fi];
      s->type = want;
      s->inst = in;
      s->code = NULL;
      s->index = fi;
      s->js = v.u.obj;
      in->funcs[fi] = s;
    }
    return 1;
  }
  case WASM_KIND_TABLE: {
    wtable *wt = (wtable *)host_of(v, WT_TABLE);
    uint32_t ti = cnt[WASM_KIND_TABLE]++;
    if (!wt) {
      link_error(ctx, api, k, im, 1, "table import requires a WebAssembly.Table");
      return 0;
    }
    if (wt->tab->type != im->table.elem) {
      link_error(ctx, api, k, im, 1, "imported table does not match the expected type");
      return 0;
    }
    if (!wasm_limits_match(wt->tab->n, wt->tab->has_max, wt->tab->max, &im->table.lim)) {
      link_error(ctx, api, k, im, 1, "table import has a smaller size or larger maximum than declared");
      return 0;
    }
    in->tables[ti] = wt->tab;
    return 1;
  }
  case WASM_KIND_MEMORY: {
    wmemory *wm = (wmemory *)host_of(v, WT_MEMORY);
    uint32_t mi = cnt[WASM_KIND_MEMORY]++;
    if (!wm) {
      link_error(ctx, api, k, im, 1, "memory import must be a WebAssembly.Memory object");
      return 0;
    }
    if (wm->mem->is64 != im->mem.is64) {
      link_error(ctx, api, k, im, 1, "memory import has a different address type than declared");
      return 0;
    }
    if (wm->mem->shared != im->mem.shared) {
      link_error(ctx, api, k, im, 1, "memory import has a different sharedness than declared");
      return 0;
    }
    if (!wasm_limits_match(wasm_memory_pages(wm->mem), wm->mem->has_max, wm->mem->max, &im->mem)) {
      link_error(ctx, api, k, im, 1, "memory import has fewer pages or a larger maximum than declared");
      return 0;
    }
    in->mems[mi] = wm->mem;
    return 1;
  }
  case WASM_KIND_TAG: {
    wtagw *wt = (wtagw *)host_of(v, WT_TAG);
    const wasm_functype *want = &m->types[im->type];
    uint32_t gi = cnt[WASM_KIND_TAG]++;
    if (!wt) {
      link_error(ctx, api, k, im, 1, "tag import requires a WebAssembly.Tag");
      return 0;
    }
    if (wt->t->np != want->np || memcmp(wt->t->types, want->p, want->np)) {
      link_error(ctx, api, k, im, 1, "imported tag does not match the expected type");
      return 0;
    }
    in->tags[gi] = wt->t;
    return 1;
  }
  default: {
    wglobal *wg = (wglobal *)host_of(v, WT_GLOBAL);
    uint32_t gi = cnt[WASM_KIND_GLOBAL]++;
    uint64_t raw[2] = {0, 0};
    if (wg) {
      if (wg->g->type != im->global.type || wg->g->mut != im->global.mut) {
        link_error(ctx, api, k, im, 1, "imported global does not match the expected type or mutability");
        return 0;
      }
      in->globals[gi] = wg->g;
      return 1;
    }
    if (im->global.mut) {
      link_error(ctx, api, k, im, 1, "imported mutable global must be a WebAssembly.Global object");
      return 0;
    }
    switch (im->global.type) {
    case WASM_I32:
    case WASM_F32:
    case WASM_F64:
      if (v.type != JV_NUM) {
        link_error(ctx, api, k, im, 1,
                   "global import must be a number, valid Wasm reference, or WebAssembly.Global object");
        return 0;
      }
      break;
    case WASM_I64:
      if (v.type != JV_BIGINT) {
        link_error(ctx, api, k, im, 1, "global import must be a BigInt");
        return 0;
      }
      break;
    case WASM_FUNCREF:
      if (v.type != JV_NULL && !func_of(v)) {
        link_error(ctx, api, k, im, 1, "imported funcref global must be null or an exported WebAssembly function");
        return 0;
      }
      break;
    case WASM_EXTERNREF:
      break;
    default:
      link_error(ctx, api, k, im, 1, "global import of this type is not supported");
      return 0;
    }
    if (!global_from_js(ctx, v, 1, im->global.type, raw))
      return 0;
    in->globals[gi] = ((wglobal *)make_global(ctx, im->global.type, 0, raw)->host)->g;
    return 1;
  }
  }
}

static js_object *instantiate_module(js_ctx *ctx, js_object *module_obj, js_value imports, const char *api)
{
  wasm_module *m = ((wmodule *)module_obj->host)->m;
  wasm_instance *in = wasm_instance_new(m);
  winstance *wi = (winstance *)calloc(1, sizeof *wi);
  js_object *io, *exports;
  uint32_t k, cnt[5] = {0, 0, 0, 0, 0};
  const char *trap;
  wexec *wx = NULL;
  if (!in || !wi) {
    wasm_instance_free(in);
    free(wi);
    js_throw_range(ctx, "WebAssembly.Instance(): out of memory");
    return NULL;
  }
  gc_push_scope();
  gc_keep(module_obj);
  wi->tag = WT_INSTANCE;
  wi->inst = in;
  wi->module_obj = module_obj;
  io = (js_object *)gc_keep(new_wasm(ctx, "@Instance", wi));
  in->self = io;

  if (m->nimports && imports.type != JV_OBJ) {
    api_error(ctx, "TypeError", api, "Imports argument must be present and must be an object");
    goto fail;
  }
  for (k = 0; k < m->nimports; k++)
    if (!resolve_import(ctx, in, m, k, imports, cnt, api))
      goto fail;
  if (wasm_instance_define(in)) {
    js_throw_range(ctx, "WebAssembly.Instance(): out of memory");
    goto fail;
  }
  for (k = m->ntable_imports; k < m->ntables; k++)
    wrap_table(ctx, in->tables[k]);
  for (k = m->nmem_imports; k < m->nmems; k++)
    wrap_memory(ctx, in->mems[k]);
  for (k = m->nglobal_imports; k < m->nglobals; k++)
    wrap_global(ctx, in->globals[k]);
  for (k = m->ntag_imports; k < m->ntags; k++)
    wrap_tag(ctx, in->tags[k]);
  trap = wasm_instance_apply_segments(in);
  if (trap) {
    api_error(ctx, "RuntimeError", api, trap);
    goto fail;
  }

  exports = (js_object *)gc_keep(js_new_object(ctx, NULL));
  for (k = 0; k < m->nexports; k++) {
    const wasm_export *e = &m->exports[k];
    js_value v;
    switch (e->kind) {
    case WASM_KIND_FUNC:
      v = func_to_js(ctx, in->funcs[e->idx]);
      break;
    case WASM_KIND_TABLE:
      v = js_obj_value((js_object *)in->tables[e->idx]->self);
      break;
    case WASM_KIND_MEMORY:
      v = js_obj_value((js_object *)in->mems[e->idx]->self);
      break;
    case WASM_KIND_TAG:
      v = js_obj_value((js_object *)in->tags[e->idx]->self);
      break;
    default:
      v = js_obj_value((js_object *)in->globals[e->idx]->self);
    }
    js_def(exports, e->name, v, P_ENUMERABLE);
  }
  exports->extensible = 0;
  js_def(io, "@exports", js_obj_value(exports), 0);

  if (m->start >= 0) {
    wx = get_exec(ctx);
    if (!wx)
      goto fail;
    invoke(ctx, wx, in->funcs[m->start], NULL, NULL);
    exec_leave(wx);
    if (ctx->has_exc)
      goto fail;
  }
  gc_pop_scope();
  return io;
fail:
  gc_pop_scope();
  return NULL;
}

static js_value instance_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *io;
  (void)t;
  if (!host_of(ARG(0), WT_MODULE))
    return js_throw_type(ctx, "WebAssembly.Instance(): Argument 0 must be a WebAssembly.Module");
  io = instantiate_module(ctx, a[0].u.obj, ARG(1), "WebAssembly.Instance()");
  return io ? js_obj_value(io) : js_undef();
}

static js_value instance_exports(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  if (!host_of(t, WT_INSTANCE))
    return js_throw_type(ctx, "WebAssembly.Instance.exports: Receiver is not a WebAssembly.Instance");
  return js_get(ctx, t, "@exports");
}

/* ---- the WebAssembly namespace ------------------------------------------- */

static js_value ns_validate(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const uint8_t *p;
  size_t len;
  wasm_module *m;
  (void)t;
  if (!get_bytes(ARG(0), &p, &len))
    return js_throw_type(ctx, "WebAssembly.validate(): Argument 0 must be a buffer source");
  m = len ? wasm_module_new(p, len, NULL, 0) : NULL;
  wasm_module_free(m);
  return js_bool(m != NULL);
}

/* A promise settled with the pending exception, or with `v` */
static js_value settle(js_ctx *ctx, js_value v)
{
  if (ctx->has_exc) {
    js_value e = ctx->exc;
    ctx->has_exc = 0;
    gc_push_scope();
    if (e.type == JV_OBJ)
      gc_keep(e.u.obj);
    v = js_promise_rejected(ctx, e);
    gc_pop_scope();
    return v;
  }
  return js_promise_resolved(ctx, v);
}

static js_value ns_compile(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *m = compile_module(ctx, ARG(0), "WebAssembly.compile()");
  (void)t;
  return settle(ctx, m ? js_obj_value(m) : js_undef());
}

static js_value ns_instantiate(js_ctx *ctx, js_value t, js_value *a, int n)
{
  const char *api = "WebAssembly.instantiate()";
  js_object *mo = NULL, *io = NULL, *res;
  js_value out = js_undef();
  (void)t;
  gc_push_scope();
  if (host_of(ARG(0), WT_MODULE)) {
    io = instantiate_module(ctx, a[0].u.obj, ARG(1), api);
    if (io)
      out = js_obj_value(io);
  } else {
    mo = compile_module(ctx, ARG(0), api);
    if (mo) {
      gc_keep(mo);
      io = instantiate_module(ctx, mo, ARG(1), api);
    }
    if (io) {
      gc_keep(io);
      res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
      js_def(res, "module", js_obj_value(mo), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
      js_def(res, "instance", js_obj_value(io), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
      out = js_obj_value(res);
    }
  }
  out = settle(ctx, out);
  gc_pop_scope();
  return out;
}

static js_value blocked_sync(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return throw_wasm(ctx, "CompileError",
                    "WebAssembly: Refused to compile or instantiate WebAssembly module because 'unsafe-eval' or "
                    "'wasm-unsafe-eval' is not an allowed source of script in the Content Security Policy");
}

static js_value blocked_async(js_ctx *ctx, js_value t, js_value *a, int n)
{
  blocked_sync(ctx, t, a, n);
  return settle(ctx, js_undef());
}

void js_wasm_block(js_ctx *ctx)
{
  static const char *const sync[] = {"Module", "Instance"};
  static const char *const async[] = {"compile", "instantiate", "compileStreaming", "instantiateStreaming"};
  js_object *ns = state_get(ctx, "ns");
  unsigned i;
  if (!ns)
    return;
  for (i = 0; i < sizeof sync / sizeof *sync; i++) {
    pentry *c = pmap_find_pub(ns, sync[i]);
    if (c && c->value.type == JV_OBJ && c->value.u.obj->fn)
      c->value.u.obj->fn->native = blocked_sync;
  }
  for (i = 0; i < sizeof async / sizeof *async; i++) {
    pentry *c = pmap_find_pub(ns, async[i]);
    if (c && c->value.type == JV_OBJ && c->value.u.obj->fn)
      c->value.u.obj->fn->native = blocked_async;
  }
}

/* structured clone: a Module and a shared Memory can move between heaps; each message holds a reference */
typedef struct {
  uint8_t *store;
  uint32_t maxpages;
} clone_mem;

static void clone_mem_release(void *p)
{
  js_sab_release(((clone_mem *)p)->store);
  free(p);
}

static void clone_module_release(void *p) { wasm_module_free((wasm_module *)p); }

static int wasm_clone_write(js_ctx *ctx, js_object *o, void **ptr, void (**release)(void *), int *kind)
{
  (void)ctx;
  if (!o->host)
    return 0;
  if (*(int *)o->host == WT_MODULE) {
    wasm_module *m = ((wmodule *)o->host)->m;
    wasm_module_retain(m);
    *ptr = m;
    *release = clone_module_release;
    *kind = WT_MODULE;
    return 1;
  }
  if (*(int *)o->host == WT_MEMORY && ((wmemory *)o->host)->mem->shared) {
    wasm_memory *m = ((wmemory *)o->host)->mem;
    clone_mem *c = (clone_mem *)malloc(sizeof *c);
    if (!c)
      return 0;
    js_sab_retain(m->data);
    c->store = m->data;
    c->maxpages = m->max;
    *ptr = c;
    *release = clone_mem_release;
    *kind = WT_MEMORY;
    return 1;
  }
  return 0;
}

static js_value wasm_clone_read(js_ctx *ctx, int kind, void *ptr)
{
  if (kind == WT_MODULE) {
    wmodule *wm = (wmodule *)calloc(1, sizeof *wm);
    if (!wm)
      gc_oom();
    wm->tag = WT_MODULE;
    wm->m = (wasm_module *)ptr;
    wasm_module_retain(wm->m);
    return js_obj_value(new_wasm(ctx, "@Module", wm));
  }
  if (kind == WT_MEMORY) {
    clone_mem *c = (clone_mem *)ptr;
    wasm_memory *m = (wasm_memory *)calloc(1, sizeof *m);
    if (!m)
      gc_oom();
    wasm_memory_attach(m, c->store, c->maxpages);
    return js_obj_value(wrap_memory(ctx, m));
  }
  return js_undef();
}

static void shared_retain(void *p) { js_sab_retain((unsigned char *)p); }
static void shared_release(void *p) { js_sab_release((unsigned char *)p); }
static void *shared_alloc(size_t n) { return n > 2147483647u ? NULL : js_sab_alloc((int)n); }

static const char k_streaming_js[] =
    "return Promise.resolve(src).then(function (r) { return r.arrayBuffer(); }).then(function (b) {"
    " return inst ? WebAssembly.instantiate(b, imp) : WebAssembly.compile(b); });";

static js_value ns_streaming(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value inst = ctx->native_data, fn, args[4], argv[3], r;
  pentry *hp = pmap_find_pub(ctx->global, "@wasm");
  js_object *state = hp && hp->value.type == JV_OBJ ? hp->value.u.obj : NULL;
  pentry *cached = state ? pmap_find_pub(state, "streaming") : NULL;
  (void)t;
  if (!state)
    return js_undef();
  gc_push_scope();
  if (cached && cached->value.type == JV_OBJ)
    fn = cached->value;
  else {
    args[0] = js_mk_cstring(ctx, "src");
    gc_keep(args[0].u.str);
    args[1] = js_mk_cstring(ctx, "imp");
    gc_keep(args[1].u.str);
    args[2] = js_mk_cstring(ctx, "inst");
    gc_keep(args[2].u.str);
    args[3] = js_mk_cstring(ctx, k_streaming_js);
    gc_keep(args[3].u.str);
    fn = js_builtin_function(ctx, js_undef(), args, 4);
    if (ctx->has_exc) {
      gc_pop_scope();
      return settle(ctx, js_undef());
    }
    js_def(state, "streaming", fn, 0);
  }
  gc_keep(fn.u.obj);
  argv[0] = ARG(0);
  argv[1] = ARG(1);
  argv[2] = inst;
  r = js_call(ctx, fn, js_undef(), argv, 3);
  gc_pop_scope();
  return r;
}

static js_object *def_class(js_ctx *ctx, js_object *state, js_object *ns, const char *name, js_native ctor, int arity,
                            js_object *proto)
{
  char key[24] = "@";
  js_object *c = js_new_native(ctx, ctor, name, arity);
  gc_keep(c);
  mcat(key, sizeof key, name);
  js_def(state, key, js_obj_value(proto), 0);
  js_def(c, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(ns, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  return c;
}

static js_object *def_proto(js_ctx *ctx, const char *tag)
{
  char full[40] = "WebAssembly.";
  js_object *p = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  mcat(full, sizeof full, tag);
  js_def(p, js_symbol_key(ctx->sym_tostringtag), js_mk_cstring(ctx, full), P_CONFIGURABLE);
  return p;
}

static void def_getter(js_ctx *ctx, js_object *proto, const char *name, js_native get, js_native set)
{
  js_object *g = (js_object *)gc_keep(js_new_native(ctx, get, name, 0));
  js_object *s = set ? (js_object *)gc_keep(js_new_native(ctx, set, name, 1)) : NULL;
  js_def_accessor(proto, name, g, s, P_CONFIGURABLE | P_ENUMERABLE);
}

/* An Error subclass, constructible with or without new: its prototype travels in the native's data */
static js_value wasm_error_ctor(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value pv = ctx->native_data, msg = js_undef();
  js_value name;
  (void)t;
  if (n > 0 && a[0].type != JV_UNDEF) {
    msg = js_to_string(ctx, a[0]);
    if (ctx->has_exc)
      return js_undef();
  }
  name = js_get(ctx, pv, "name");
  return js_make_error(ctx, pv.u.obj, name.type == JV_STR ? name.u.str->data : "Error",
                       msg.type == JV_STR ? msg.u.str->data : "");
}

static void def_error(js_ctx *ctx, js_object *state, js_object *ns, const char *name)
{
  char key[24] = "@";
  js_object *p = (js_object *)gc_keep(js_new_object(ctx, ctx->error_proto));
  js_object *c = (js_object *)gc_keep(js_new_native(ctx, wasm_error_ctor, name, 1));
  c->fn->data = js_obj_value(p);
  mcat(key, sizeof key, name);
  js_def(state, key, js_obj_value(p), 0);
  js_def(p, "name", js_mk_cstring(ctx, name), P_WRITABLE | P_CONFIGURABLE);
  js_def(p, "message", js_mk_cstring(ctx, ""), P_WRITABLE | P_CONFIGURABLE);
  js_def(c, "prototype", js_obj_value(p), 0);
  js_def(p, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(ns, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
}

void js_setup_wasm(js_ctx *ctx)
{
  js_object *ns, *state, *p, *c;
  gc_push_scope();
  js_wasm_trace = wasm_trace;
  js_wasm_finalizer = wasm_finalize;
  js_wasm_clone_write = wasm_clone_write;
  js_wasm_clone_read = wasm_clone_read;
  wasm_shared_alloc = shared_alloc;
  wasm_shared_retain = shared_retain;
  wasm_shared_release = shared_release;
  ns = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ctx->global, "WebAssembly", js_obj_value(ns), P_WRITABLE | P_CONFIGURABLE);
  state = (js_object *)gc_keep(js_new_object(ctx, NULL));
  js_def(ctx->global, "@wasm", js_obj_value(state), 0);
  js_def(state, "ns", js_obj_value(ns), 0);
  js_def(ns, js_symbol_key(ctx->sym_tostringtag), js_mk_cstring(ctx, "WebAssembly"), P_CONFIGURABLE);

  p = def_proto(ctx, "Module");
  c = def_class(ctx, state, ns, "Module", module_ctor, 1, p);
  js_def_native(ctx, c, "imports", module_imports, 1);
  js_def_native(ctx, c, "exports", module_exports, 1);
  js_def_native(ctx, c, "customSections", module_custom_sections, 2);

  p = def_proto(ctx, "Instance");
  def_class(ctx, state, ns, "Instance", instance_ctor, 1, p);
  def_getter(ctx, p, "exports", instance_exports, NULL);

  p = def_proto(ctx, "Memory");
  def_class(ctx, state, ns, "Memory", memory_ctor, 1, p);
  def_getter(ctx, p, "buffer", memory_buffer, NULL);
  js_def_native(ctx, p, "grow", memory_grow, 1);

  p = def_proto(ctx, "Table");
  def_class(ctx, state, ns, "Table", table_ctor, 1, p);
  def_getter(ctx, p, "length", table_length, NULL);
  js_def_native(ctx, p, "get", table_get, 1);
  js_def_native(ctx, p, "set", table_set, 1);
  js_def_native(ctx, p, "grow", table_grow, 1);

  p = def_proto(ctx, "Global");
  def_class(ctx, state, ns, "Global", global_ctor, 1, p);
  def_getter(ctx, p, "value", global_value_get, global_value_set);
  js_def_native(ctx, p, "valueOf", global_value_get, 0);

  p = def_proto(ctx, "Tag");
  def_class(ctx, state, ns, "Tag", tag_ctor, 1, p);
  p = def_proto(ctx, "Exception");
  def_class(ctx, state, ns, "Exception", exception_ctor, 2, p);
  js_def_native(ctx, p, "is", exception_is, 1);
  js_def_native(ctx, p, "getArg", exception_get_arg, 2);

  def_error(ctx, state, ns, "CompileError");
  def_error(ctx, state, ns, "LinkError");
  def_error(ctx, state, ns, "RuntimeError");

  js_def_native(ctx, ns, "validate", ns_validate, 1);
  js_def_native(ctx, ns, "compile", ns_compile, 1);
  js_def_native(ctx, ns, "instantiate", ns_instantiate, 1);
  c = (js_object *)gc_keep(js_new_native(ctx, ns_streaming, "compileStreaming", 1));
  c->fn->data = js_bool(0);
  js_def(ns, "compileStreaming", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  c = (js_object *)gc_keep(js_new_native(ctx, ns_streaming, "instantiateStreaming", 1));
  c->fn->data = js_bool(1);
  js_def(ns, "instantiateStreaming", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}
