/* Streams API core: ReadableStream, WritableStream, TransformStream and piping, as native classes.
 *
 * Each stream, reader, writer and controller is an OBJ_HOST object whose host struct holds the state. The collector
 * reaches the JS values in those structs through js_host_trace. Promise reactions run where they are triggered, so
 * the callbacks that continue an asynchronous step wait in one job queue (after/job_run) and run one at a time, the
 * same discipline the script half uses through N.after. */

#include "streamscore.h"

#define ALL (P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE)
#define KEEP(v) keep_value(v)

enum { SK_RS = HK_STREAM, SK_RD, SK_RC, SK_WS, SK_WR, SK_WC, SK_TS, SK_TC, SK_PIPE };
enum { PI_RS, PI_RD, PI_RC, PI_WS, PI_WR, PI_WC, PI_TS, PI_TC, PI_N };
enum { R_READABLE, R_CLOSED, R_ERRORED };
enum { W_WRITABLE, W_CLOSED, W_ERRORED };
enum { OP_WRITE, OP_CLOSE };

struct sh {
  int tag;
  js_object *self;
};

/* A double-ended queue of fixed-size elements */
typedef struct {
  char *p;
  unsigned head, n, cap, esz;
} dq;

struct qe {
  js_value v;
  double size;
};
struct op {
  int type;
  js_value chunk;
  double size;
  js_object *d;
};

struct rs {
  struct sh h;
  int state, started, pulling, pull_again, close_requested, disturbed, bytes;
  js_value err, size_fn, src, start_fn, pull_fn, cancel_fn;
  js_object *reader, *ctrl, *ts;
  dq q; /* struct qe */
  double qsize, hwm;
};
struct rd {
  struct sh h;
  js_object *stream, *closed;
  dq reqs; /* js_object * */
};
/* a controller: the stream (or transform) it belongs to */
struct own {
  struct sh h;
  js_object *owner;
};
struct ws {
  struct sh h;
  int state, started, close_requested, has_inflight;
  js_value err, size_fn, sink, write_fn, close_fn, abort_fn, abort_ctl;
  js_object *writer, *ctrl, *ts;
  dq ops; /* struct op */
  struct op inflight;
  double qsize, hwm;
};
struct wr {
  struct sh h;
  js_object *stream, *ready, *closed;
  int ready_settled;
};
struct ts {
  struct sh h;
  int backpressure;
  js_object *bpd, *start_d, *ctrl, *readable, *writable;
  js_value tr;
};
struct pipe {
  struct sh h;
  js_object *reader, *writer, *result;
  js_value signal, sd_err;
  int shutting_down, prevent_close, prevent_abort, prevent_cancel, sd_failed;
};

#define RS(o) ((struct rs *)(o)->host)
#define RD(o) ((struct rd *)(o)->host)
#define WS(o) ((struct ws *)(o)->host)
#define WR(o) ((struct wr *)(o)->host)
#define TS(o) ((struct ts *)(o)->host)
#define PIPE(o) ((struct pipe *)(o)->host)

static __thread js_object *g_proto[PI_N];

/* ---- helpers ------------------------------------------------------------ */

static js_value keep_value(js_value v)
{
  if (v.type == JV_OBJ || v.type == JV_SYMBOL || v.type == JV_BIGINT)
    gc_keep(v.u.obj);
  else if (v.type == JV_STR)
    gc_keep(v.u.str);
  return v;
}

static void mark(js_value v)
{
  if (v.type == JV_OBJ || v.type == JV_SYMBOL || v.type == JV_BIGINT)
    gc_mark(v.u.obj);
  else if (v.type == JV_STR)
    gc_mark(v.u.str);
}

static void dq_init(dq *q, unsigned esz)
{
  memset(q, 0, sizeof *q);
  q->esz = esz;
}
static void *dq_at(dq *q, unsigned i) { return q->p + (q->head + i) * q->esz; }
static void *dq_push(dq *q)
{
  if (q->head + q->n == q->cap) {
    if (q->head && q->head >= q->n) {
      memmove(q->p, q->p + q->head * q->esz, q->n * q->esz);
      q->head = 0;
    } else {
      unsigned nc = q->cap ? q->cap * 2 : 4;
      char *np = realloc(q->p, nc * q->esz);
      if (!np)
        gc_oom();
      q->p = np;
      q->cap = nc;
    }
  }
  return dq_at(q, q->n++);
}
static void dq_shift(dq *q)
{
  q->head++;
  q->n--;
  if (!q->n)
    q->head = 0;
}
static void dq_clear(dq *q) { q->head = q->n = 0; }

static js_value obj(js_object *o) { return js_obj_value(o); }

static js_value terr(js_ctx *ctx, const char *m) { return js_make_error(ctx, ctx->type_error_proto, "TypeError", m); }

static int is_obj(js_value v) { return v.type == JV_OBJ; }
static int nullish(js_value v) { return v.type == JV_UNDEF || v.type == JV_NULL; }
static int is_view(js_value v) { return v.type == JV_OBJ && v.u.obj->kind == OBJ_TYPEDARRAY; }

/* the exception being thrown, taken out of the context so code can run before rethrowing it */
static js_value take_exc(js_ctx *ctx)
{
  js_value e = ctx->exc;
  ctx->has_exc = 0;
  return KEEP(e);
}

static js_object *prom(js_ctx *ctx) { return js_promise_new(ctx); }
static void p_ok(js_ctx *ctx, js_object *p, js_value v) { js_promise_settle(ctx, p, 0, v); }
static void p_err(js_ctx *ctx, js_object *p, js_value e) { js_promise_settle(ctx, p, 1, e); }
static js_value p_resolved(js_ctx *ctx, js_value v)
{
  js_object *p;
  gc_push_scope();
  KEEP(v);
  p = prom(ctx);
  p_ok(ctx, p, v);
  gc_pop_scope();
  return obj(p);
}
static js_value p_rejected_msg(js_ctx *ctx, const char *msg)
{
  js_value r;
  gc_push_scope();
  r = js_promise_rejected(ctx, KEEP(terr(ctx, msg)));
  gc_pop_scope();
  return r;
}

static js_value iter_result(js_ctx *ctx, js_value v, int done)
{
  js_object *o;
  gc_push_scope();
  KEEP(v);
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "value", v, ALL);
  js_def(o, "done", js_bool(done), ALL);
  gc_pop_scope();
  return obj(o);
}

static js_object *sk_of(js_value v, int tag)
{
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_HOST || !v.u.obj->host || ((struct sh *)v.u.obj->host)->tag != tag)
    return 0;
  return v.u.obj;
}

static int inherits(js_object *o, js_object *proto)
{
  for (o = o->proto; o; o = o->proto)
    if (o == proto)
      return 1;
  return 0;
}

/* The object a constructor fills in: `thisv` when a subclass constructor is running super(), else a new one */
static js_object *host_alloc(js_ctx *ctx, js_value thisv, int pi, int tag, unsigned size)
{
  struct sh *h = calloc(1, size);
  js_object *o;
  if (!h)
    gc_oom();
  h->tag = tag;
  if (pi >= 0 && thisv.type == JV_OBJ && thisv.u.obj->kind == OBJ_PLAIN && !thisv.u.obj->host &&
      inherits(thisv.u.obj, g_proto[pi]))
    o = thisv.u.obj;
  else
    o = js_new_object(ctx, pi >= 0 ? g_proto[pi] : ctx->object_proto);
  o->kind = OBJ_HOST;
  o->host = h;
  h->self = o;
  return o;
}

static void wb(js_object *o) { gc_write_barrier(o); }

/* ---- strategies and options ---------------------------------------------- */

static int get_method(js_ctx *ctx, js_value o, const char *name, js_value *out)
{
  js_value f = js_get(ctx, o, name);
  char msg[48];
  if (ctx->has_exc)
    return 0;
  if (nullish(f)) {
    *out = js_undef();
    return 1;
  }
  if (!js_is_callable(f)) {
    snprintf(msg, sizeof msg, "%s must be a function", name);
    js_throw_type(ctx, msg);
    return 0;
  }
  *out = f;
  return 1;
}

static int to_hwm(js_ctx *ctx, js_value v, double *out)
{
  double n = js_to_number(ctx, v);
  if (ctx->has_exc)
    return 0;
  if (n != n || n < 0) {
    js_throw_range(ctx, "Invalid highWaterMark");
    return 0;
  }
  *out = n;
  return 1;
}

static int strategy_hwm(js_ctx *ctx, js_value strategy, double dflt, double *out)
{
  js_value v = js_undef();
  if (is_obj(strategy)) {
    v = js_get(ctx, strategy, "highWaterMark");
    if (ctx->has_exc)
      return 0;
  }
  if (v.type == JV_UNDEF) {
    *out = dflt;
    return 1;
  }
  return to_hwm(ctx, v, out);
}

/* The strategy's size function (undefined: every chunk counts 1) */
static int strategy_size(js_ctx *ctx, js_value strategy, js_value *out)
{
  js_value f = js_undef();
  if (is_obj(strategy)) {
    f = js_get(ctx, strategy, "size");
    if (ctx->has_exc)
      return 0;
  }
  if (f.type != JV_UNDEF && !js_is_callable(f)) {
    js_throw_type(ctx, "size must be a function");
    return 0;
  }
  *out = f;
  return 1;
}

static int chunk_size(js_ctx *ctx, int bytes, js_value fn, js_value chunk, double *out)
{
  double n;
  if (bytes) {
    n = js_to_number(ctx, js_get(ctx, chunk, "byteLength"));
  } else if (fn.type == JV_UNDEF) {
    *out = 1;
    return 1;
  } else {
    js_value r = js_call(ctx, fn, js_undef(), &chunk, 1);
    if (ctx->has_exc)
      return 0;
    n = js_to_number(ctx, r);
  }
  if (ctx->has_exc)
    return 0;
  if (n != n || n < 0 || n == 1.0 / 0.0) {
    js_throw_range(ctx, "Invalid chunk size");
    return 0;
  }
  *out = n;
  return 1;
}

/* ---- jobs ---------------------------------------------------------------- */

enum {
  JK_JS,
  JK_RS_START,
  JK_RS_PULL,
  JK_WS_START,
  JK_WS_WRITE,
  JK_WS_CLOSE,
  JK_TS_START,
  JK_PIPE_SRC_CLOSED,
  JK_PIPE_DST_CLOSED,
  JK_PIPE_READY,
  JK_PIPE_READ,
  JK_PIPE_FINISH,
  JK_PIPE_ABORTED,
  JK_TS_WRAP,
  JK_TS_GO,
  JK_TS_CLOSE,
  JK_FWD
};

static void job_exec(js_ctx *ctx, int kind, js_value a, js_value b, int ok, js_value v);

static __thread js_object *g_jq;
static __thread unsigned g_jhead;
static __thread int g_jrunning;

static void job_run(js_ctx *ctx, int kind, js_value a, js_value b, int ok, js_value v)
{
  js_object *q;
  gc_push_scope();
  KEEP(a);
  KEEP(b);
  KEEP(v);
  if (!g_jq) {
    g_jq = js_new_array(ctx, 0);
    js_def(ctx->global, "@sjobs", obj(g_jq), 0);
  }
  q = g_jq;
  js_array_push(ctx, q, js_number(kind));
  js_array_push(ctx, q, a);
  js_array_push(ctx, q, b);
  js_array_push(ctx, q, js_number(ok));
  js_array_push(ctx, q, v);
  if (g_jrunning) {
    gc_pop_scope();
    return;
  }
  g_jrunning = 1;
  while (g_jhead < q->elen) {
    js_value *e = q->elems + g_jhead;
    int k = (int)e[0].u.num, o = (int)e[3].u.num;
    js_value ja = e[1], jb = e[2], jv = e[4];
    g_jhead += 5;
    if (g_jhead >= 5 * 2048 && g_jhead * 2 >= q->elen) {
      memmove(q->elems, q->elems + g_jhead, (q->elen - g_jhead) * sizeof *q->elems);
      q->elen -= g_jhead;
      g_jhead = 0;
    }
    gc_push_scope();
    KEEP(ja);
    KEEP(jb);
    KEEP(jv);
    job_exec(ctx, k, ja, jb, o, jv);
    ctx->has_exc = 0;
    gc_pop_scope();
  }
  js_array_set_len(ctx, q, 0);
  g_jhead = 0;
  g_jrunning = 0;
  gc_pop_scope();
}

static js_value cell_ok(js_ctx *ctx, js_value t, js_value *args, int argc);
static js_value cell_err(js_ctx *ctx, js_value t, js_value *args, int argc);

static js_object *native_with(js_ctx *ctx, js_native fn, js_value data)
{
  js_object *f = js_new_native(ctx, fn, "", 1);
  f->fn->data = data;
  wb(f);
  return f;
}

/* [kind, a, b] and the two callbacks that feed a settled promise into the job queue or a direct job_exec */
static void react_cell(js_ctx *ctx, js_object *p, int kind, js_value a, js_value b, int queued)
{
  js_object *cell, *fok, *fbad;
  gc_push_scope();
  KEEP(a);
  KEEP(b);
  cell = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, cell, js_number(kind));
  js_array_push(ctx, cell, a);
  js_array_push(ctx, cell, b);
  js_array_push(ctx, cell, js_number(queued));
  fok = (js_object *)gc_keep(native_with(ctx, cell_ok, obj(cell)));
  fbad = (js_object *)gc_keep(native_with(ctx, cell_err, obj(cell)));
  js_promise_react(ctx, p, obj(fok), obj(fbad));
  gc_pop_scope();
}

static void cell_fire(js_ctx *ctx, js_value cell, int ok, js_value v)
{
  js_object *c = cell.u.obj;
  int kind = (int)c->elems[0].u.num, queued = (int)c->elems[3].u.num;
  js_value a = c->elems[1], b = c->elems[2];
  if (queued)
    job_run(ctx, kind, a, b, ok, v);
  else {
    gc_push_scope();
    KEEP(a);
    KEEP(b);
    KEEP(v);
    job_exec(ctx, kind, a, b, ok, v);
    ctx->has_exc = 0;
    gc_pop_scope();
  }
}
static js_value cell_ok(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cell = ctx->native_data;
  (void)t;
  cell_fire(ctx, cell, 1, argc > 0 ? args[0] : js_undef());
  return js_undef();
}
static js_value cell_err(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cell = ctx->native_data;
  (void)t;
  cell_fire(ctx, cell, 0, argc > 0 ? args[0] : js_undef());
  return js_undef();
}

/* Promise.resolve(p): the settled state of p, adopting a thenable; -1 when p is not a promise and not a thenable
 * (it then counts as fulfilled with itself). *out is the promise to react to, or the settled value. */
static int settle_state(js_ctx *ctx, js_value *p, js_value *v)
{
  int st = js_promise_inspect(ctx, *p, v);
  if (st >= 0)
    return st;
  if (p->type == JV_OBJ) {
    js_value then = js_get(ctx, *p, "then");
    if (ctx->has_exc) {
      *v = take_exc(ctx);
      return 2;
    }
    if (js_is_callable(then)) {
      *p = KEEP(js_promise_resolved(ctx, *p));
      return js_promise_inspect(ctx, *p, v);
    }
  }
  *v = *p;
  return 1;
}

/* Promise.resolve(p).then(on ok, on error), each callback serialised through the job queue */
static void after(js_ctx *ctx, js_value p, int kind, js_value a, js_value b)
{
  js_value v = js_undef();
  int st;
  gc_push_scope();
  KEEP(p);
  KEEP(a);
  KEEP(b);
  st = settle_state(ctx, &p, &v);
  KEEP(v);
  if (st == 0)
    react_cell(ctx, p.u.obj, kind, a, b, 1);
  else
    job_run(ctx, kind, a, b, st == 1, v);
  gc_pop_scope();
}

/* The same, but the callbacks run right away on a settled promise (a plain .then) */
static void then_inline(js_ctx *ctx, js_value p, int kind, js_value a, js_value b)
{
  js_value v = js_undef();
  int st;
  gc_push_scope();
  KEEP(p);
  KEEP(a);
  KEEP(b);
  st = settle_state(ctx, &p, &v);
  KEEP(v);
  if (st == 0)
    react_cell(ctx, p.u.obj, kind, a, b, 0);
  else
    job_exec(ctx, kind, a, b, st == 1, v);
  gc_pop_scope();
}

/* Promise.resolve(res).then(function () { return undefined }) */
static js_value then_undef(js_ctx *ctx, js_value res)
{
  js_value v = js_undef(), r;
  int st;
  gc_push_scope();
  KEEP(res);
  st = settle_state(ctx, &res, &v);
  KEEP(v);
  if (st == 1)
    r = p_resolved(ctx, js_undef());
  else if (st == 2)
    r = js_promise_rejected(ctx, v);
  else {
    js_object *d = (js_object *)gc_keep(prom(ctx));
    react_cell(ctx, res.u.obj, JK_FWD, obj(d), js_undef(), 0);
    r = obj(d);
  }
  gc_pop_scope();
  return r;
}

/* ---- host objects: trace and free ----------------------------------------- */

static void sk_trace(js_object *o)
{
  struct sh *h = o->host;
  unsigned i;
  switch (h->tag) {
  case SK_RS: {
    struct rs *r = (struct rs *)h;
    mark(r->err);
    mark(r->size_fn);
    mark(r->src);
    mark(r->start_fn);
    mark(r->pull_fn);
    mark(r->cancel_fn);
    gc_mark(r->reader);
    gc_mark(r->ctrl);
    gc_mark(r->ts);
    for (i = 0; i < r->q.n; i++)
      mark(((struct qe *)dq_at(&r->q, i))->v);
    break;
  }
  case SK_RD: {
    struct rd *d = (struct rd *)h;
    gc_mark(d->stream);
    gc_mark(d->closed);
    for (i = 0; i < d->reqs.n; i++)
      gc_mark(*(js_object **)dq_at(&d->reqs, i));
    break;
  }
  case SK_RC:
  case SK_WC:
  case SK_TC:
    gc_mark(((struct own *)h)->owner);
    break;
  case SK_WS: {
    struct ws *w = (struct ws *)h;
    mark(w->err);
    mark(w->size_fn);
    mark(w->sink);
    mark(w->write_fn);
    mark(w->close_fn);
    mark(w->abort_fn);
    mark(w->abort_ctl);
    gc_mark(w->writer);
    gc_mark(w->ctrl);
    gc_mark(w->ts);
    for (i = 0; i < w->ops.n; i++) {
      struct op *op = dq_at(&w->ops, i);
      mark(op->chunk);
      gc_mark(op->d);
    }
    if (w->has_inflight) {
      mark(w->inflight.chunk);
      gc_mark(w->inflight.d);
    }
    break;
  }
  case SK_WR: {
    struct wr *w = (struct wr *)h;
    gc_mark(w->stream);
    gc_mark(w->ready);
    gc_mark(w->closed);
    break;
  }
  case SK_TS: {
    struct ts *t = (struct ts *)h;
    gc_mark(t->bpd);
    gc_mark(t->start_d);
    gc_mark(t->ctrl);
    gc_mark(t->readable);
    gc_mark(t->writable);
    mark(t->tr);
    break;
  }
  case SK_PIPE: {
    struct pipe *p = (struct pipe *)h;
    gc_mark(p->reader);
    gc_mark(p->writer);
    gc_mark(p->result);
    mark(p->signal);
    mark(p->sd_err);
    break;
  }
  default:
    break;
  }
}

void streams_core_free(js_object *o)
{
  struct sh *h = o->host;
  if (h->tag < HK_STREAM) {
    if (js_host_finalizer && js_host_finalizer != streams_core_free)
      js_host_finalizer(o);
    return;
  }
  switch (h->tag) {
  case SK_RS:
    free(((struct rs *)h)->q.p);
    break;
  case SK_RD:
    free(((struct rd *)h)->reqs.p);
    break;
  case SK_WS:
    free(((struct ws *)h)->ops.p);
    break;
  }
  free(h);
}

/* ---- ReadableStream core ---------------------------------------------------- */

static js_value ts_readable_pull(js_ctx *ctx, js_object *to);
static js_value ts_readable_cancel(js_ctx *ctx, js_object *to, js_value reason);
static js_value ts_write(js_ctx *ctx, js_object *to, js_value chunk);
static js_value ts_close(js_ctx *ctx, js_object *to);
static js_value ts_abort(js_ctx *ctx, js_object *to, js_value reason);
static void ts_error(js_ctx *ctx, js_object *to, js_value e);
static void ws_error(js_ctx *ctx, js_object *wo, js_value e);
static void ws_advance(js_ctx *ctx, js_object *wo);
static void ws_update_ready(js_ctx *ctx, js_object *wo);

static double rs_desired(struct rs *r) { return r->state == R_CLOSED ? 0 : r->hwm - r->qsize; }

static int rs_should_pull(struct rs *r)
{
  if (r->state != R_READABLE || r->close_requested || !r->started)
    return 0;
  if (r->reader && RD(r->reader)->reqs.n > 0)
    return 1;
  return rs_desired(r) > 0;
}

/* Settle and forget every read a reader is waiting on: rejected with e, or completed as done */
static void reqs_settle(js_ctx *ctx, struct rd *d, int reject, js_value e)
{
  dq old = d->reqs;
  unsigned i;
  gc_push_scope();
  KEEP(e);
  dq_init(&d->reqs, sizeof(js_object *));
  for (i = 0; i < old.n; i++)
    gc_keep(*(js_object **)dq_at(&old, i));
  for (i = 0; i < old.n; i++) {
    js_object *p = *(js_object **)dq_at(&old, i);
    if (reject)
      p_err(ctx, p, e);
    else
      p_ok(ctx, p, iter_result(ctx, js_undef(), 1));
  }
  free(old.p);
  gc_pop_scope();
}

static void rs_error(js_ctx *ctx, js_object *ro, js_value e)
{
  struct rs *r = RS(ro);
  js_object *rdo;
  if (r->state != R_READABLE)
    return;
  gc_push_scope();
  gc_keep(ro);
  KEEP(e);
  r->state = R_ERRORED;
  r->err = e;
  dq_clear(&r->q);
  r->qsize = 0;
  wb(ro);
  rdo = r->reader;
  if (rdo) {
    gc_keep(rdo);
    p_err(ctx, RD(rdo)->closed, e);
    reqs_settle(ctx, RD(rdo), 1, e);
  }
  gc_pop_scope();
}

static void rs_close(js_ctx *ctx, js_object *ro)
{
  struct rs *r = RS(ro);
  js_object *rdo;
  if (r->state != R_READABLE)
    return;
  gc_push_scope();
  gc_keep(ro);
  r->state = R_CLOSED;
  rdo = r->reader;
  if (rdo) {
    gc_keep(rdo);
    p_ok(ctx, RD(rdo)->closed, js_undef());
    reqs_settle(ctx, RD(rdo), 0, js_undef());
  }
  gc_pop_scope();
}

static void rs_pull(js_ctx *ctx, js_object *ro)
{
  struct rs *r = RS(ro);
  js_value res;
  if (!rs_should_pull(r))
    return;
  if (r->pulling) {
    r->pull_again = 1;
    return;
  }
  if (!r->ts && !js_is_callable(r->pull_fn))
    return;
  r->pulling = 1;
  gc_push_scope();
  gc_keep(ro);
  if (r->ts)
    res = ts_readable_pull(ctx, r->ts);
  else {
    js_value a = obj(r->ctrl);
    res = js_call(ctx, r->pull_fn, r->src, &a, 1);
  }
  if (ctx->has_exc)
    rs_error(ctx, ro, take_exc(ctx));
  else {
    KEEP(res);
    after(ctx, res, JK_RS_PULL, obj(ro), js_undef());
  }
  gc_pop_scope();
}

/* Returns 0 with the exception pending */
static int rs_close_request(js_ctx *ctx, js_object *ro)
{
  struct rs *r = RS(ro);
  if (r->close_requested || r->state != R_READABLE) {
    js_throw_type(ctx, "Stream is not closable");
    return 0;
  }
  r->close_requested = 1;
  if (r->q.n == 0)
    rs_close(ctx, ro);
  return 1;
}

/* Returns 0 with the exception pending */
static int rs_enqueue(js_ctx *ctx, js_object *ro, js_value chunk)
{
  struct rs *r = RS(ro);
  if (r->close_requested || r->state != R_READABLE) {
    js_throw_type(ctx, "Cannot enqueue: stream is closed or errored");
    return 0;
  }
  if (r->bytes && !is_view(chunk)) {
    js_throw_type(ctx, "Chunk must be an ArrayBuffer view");
    return 0;
  }
  gc_push_scope();
  gc_keep(ro);
  KEEP(chunk);
  if (r->reader && RD(r->reader)->reqs.n > 0) {
    struct rd *d = RD(r->reader);
    js_object *p = *(js_object **)dq_at(&d->reqs, 0);
    dq_shift(&d->reqs);
    gc_keep(p);
    p_ok(ctx, p, iter_result(ctx, chunk, 0));
  } else {
    double size;
    struct qe *e;
    if (!chunk_size(ctx, r->bytes, r->size_fn, chunk, &size)) {
      js_value err = take_exc(ctx);
      rs_error(ctx, ro, err);
      js_throw(ctx, err);
      gc_pop_scope();
      return 0;
    }
    e = dq_push(&r->q);
    e->v = chunk;
    e->size = size;
    r->qsize += size;
    wb(ro);
  }
  rs_pull(ctx, ro);
  gc_pop_scope();
  return 1;
}

static js_value rs_read(js_ctx *ctx, js_object *ro, js_object *rdo)
{
  struct rs *r = RS(ro);
  js_value res;
  r->disturbed = 1;
  if (r->state == R_CLOSED) {
    gc_push_scope();
    res = p_resolved(ctx, KEEP(iter_result(ctx, js_undef(), 1)));
    gc_pop_scope();
    return res;
  }
  if (r->state == R_ERRORED)
    return js_promise_rejected(ctx, r->err);
  gc_push_scope();
  gc_keep(ro);
  if (r->q.n > 0) {
    struct qe *e = dq_at(&r->q, 0);
    js_value chunk = KEEP(e->v);
    r->qsize -= e->size;
    dq_shift(&r->q);
    if (r->qsize < 0)
      r->qsize = 0;
    if (r->close_requested && r->q.n == 0)
      rs_close(ctx, ro);
    else
      rs_pull(ctx, ro);
    res = p_resolved(ctx, KEEP(iter_result(ctx, chunk, 0)));
  } else {
    js_object *d = (js_object *)gc_keep(prom(ctx));
    *(js_object **)dq_push(&RD(rdo)->reqs) = d;
    wb(rdo);
    rs_pull(ctx, ro);
    res = obj(d);
  }
  gc_pop_scope();
  return res;
}

static js_value rs_cancel(js_ctx *ctx, js_object *ro, js_value reason)
{
  struct rs *r = RS(ro);
  js_value res = js_undef();
  r->disturbed = 1;
  if (r->state == R_CLOSED)
    return p_resolved(ctx, js_undef());
  if (r->state == R_ERRORED)
    return js_promise_rejected(ctx, r->err);
  gc_push_scope();
  gc_keep(ro);
  KEEP(reason);
  dq_clear(&r->q);
  r->qsize = 0;
  rs_close(ctx, ro);
  if (r->ts)
    res = ts_readable_cancel(ctx, r->ts, reason);
  else if (js_is_callable(r->cancel_fn))
    res = js_call(ctx, r->cancel_fn, r->src, &reason, 1);
  if (ctx->has_exc)
    res = js_promise_rejected(ctx, take_exc(ctx));
  else
    res = then_undef(ctx, KEEP(res));
  gc_pop_scope();
  return res;
}

/* ---- readers and controllers ---------------------------------------------- */

static js_object *ctrl_new(js_ctx *ctx, int pi, int tag, js_object *owner)
{
  js_object *o;
  gc_push_scope();
  gc_keep(owner);
  o = host_alloc(ctx, js_undef(), pi, tag, sizeof(struct own));
  ((struct own *)o->host)->owner = owner;
  wb(o);
  gc_pop_scope();
  return o;
}

/* A new reader locked to the stream in streamv; NULL with the exception pending */
static js_object *rd_new(js_ctx *ctx, js_value thisv, js_value streamv)
{
  js_object *ro = sk_of(streamv, SK_RS), *rdo;
  struct rs *r;
  struct rd *d;
  if (!ro) {
    js_throw_type(ctx, "Illegal invocation");
    return 0;
  }
  r = RS(ro);
  if (r->reader) {
    js_throw_type(ctx, "ReadableStream is locked");
    return 0;
  }
  gc_push_scope();
  gc_keep(ro);
  rdo = (js_object *)gc_keep(host_alloc(ctx, thisv, PI_RD, SK_RD, sizeof *d));
  d = RD(rdo);
  dq_init(&d->reqs, sizeof(js_object *));
  d->stream = ro;
  d->closed = prom(ctx);
  r->reader = rdo;
  wb(ro);
  wb(rdo);
  if (r->state == R_CLOSED)
    p_ok(ctx, d->closed, js_undef());
  else if (r->state == R_ERRORED)
    p_err(ctx, d->closed, r->err);
  gc_pop_scope();
  return rdo;
}

static void rd_release(js_ctx *ctx, js_object *rdo)
{
  struct rd *d = RD(rdo);
  js_object *ro = d->stream;
  js_value err;
  if (!ro)
    return;
  gc_push_scope();
  gc_keep(rdo);
  gc_keep(ro);
  err = KEEP(terr(ctx, "Reader has been released"));
  reqs_settle(ctx, d, 1, err);
  if (RS(ro)->state == R_READABLE)
    p_err(ctx, d->closed, err);
  else {
    d->closed = prom(ctx);
    wb(rdo);
    p_err(ctx, d->closed, err);
  }
  RS(ro)->reader = 0;
  d->stream = 0;
  gc_pop_scope();
}

static js_value rd_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo = rd_new(ctx, thisv, A(0));
  return rdo ? obj(rdo) : js_undef();
}

static js_value rd_read_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo = sk_of(thisv, SK_RD);
  (void)args;
  (void)argc;
  if (!rdo)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (!RD(rdo)->stream)
    return p_rejected_msg(ctx, "Reader has been released");
  return rs_read(ctx, RD(rdo)->stream, rdo);
}

static js_value rd_cancel_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo = sk_of(thisv, SK_RD);
  if (!rdo)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (!RD(rdo)->stream)
    return p_rejected_msg(ctx, "Reader has been released");
  return rs_cancel(ctx, RD(rdo)->stream, A(0));
}

static js_value rd_closed_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo = sk_of(thisv, SK_RD);
  (void)args;
  (void)argc;
  if (!rdo)
    return p_rejected_msg(ctx, "Illegal invocation");
  return obj(RD(rdo)->closed);
}

static js_value rd_release_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo = sk_of(thisv, SK_RD);
  (void)args;
  (void)argc;
  if (!rdo)
    return js_throw_type(ctx, "Illegal invocation");
  rd_release(ctx, rdo);
  return js_undef();
}

#define RC_OWNER(o) (((struct own *)(o)->host)->owner)

static js_value rc_desired_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_RC);
  struct rs *r;
  (void)ctx;
  (void)args;
  (void)argc;
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  r = RS(RC_OWNER(co));
  return r->state == R_ERRORED ? js_null() : js_number(rs_desired(r));
}

static js_value rc_close_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_RC);
  (void)args;
  (void)argc;
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  rs_close_request(ctx, RC_OWNER(co));
  return js_undef();
}

static js_value rc_enqueue_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_RC);
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  rs_enqueue(ctx, RC_OWNER(co), A(0));
  return js_undef();
}

static js_value rc_error_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_RC);
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  rs_error(ctx, RC_OWNER(co), A(0));
  return js_undef();
}

/* ---- ReadableStream ------------------------------------------------------- */

static js_value rs_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value source = A(0), strategy = A(1), type, start_fn = js_undef(), pull_fn = js_undef(), cancel_fn = js_undef();
  js_value size_fn = js_undef(), started = js_undef();
  double hwm;
  int bytes = 0;
  js_object *ro, *co;
  struct rs *r;
  if (nullish(source))
    source = js_undef();
  else if (!is_obj(source))
    return js_throw_type(ctx, "underlyingSource must be an object");
  if (is_obj(source)) {
    type = js_get(ctx, source, "type");
    if (ctx->has_exc)
      return js_undef();
    if (type.type != JV_UNDEF) {
      js_value s = js_to_string(ctx, type);
      if (ctx->has_exc)
        return js_undef();
      if (strcmp(s.u.str->data, "bytes"))
        return js_throw_range(ctx, "Invalid underlyingSource type");
      bytes = 1;
    }
    if (!get_method(ctx, source, "start", &start_fn) || !get_method(ctx, source, "pull", &pull_fn) ||
        !get_method(ctx, source, "cancel", &cancel_fn))
      return js_undef();
  }
  if (bytes) {
    if (!strategy_hwm(ctx, strategy, 0, &hwm))
      return js_undef();
  } else if (!strategy_size(ctx, strategy, &size_fn) || !strategy_hwm(ctx, strategy, 1, &hwm))
    return js_undef();
  gc_push_scope();
  ro = (js_object *)gc_keep(host_alloc(ctx, thisv, PI_RS, SK_RS, sizeof *r));
  r = RS(ro);
  dq_init(&r->q, sizeof(struct qe));
  r->bytes = bytes;
  r->hwm = hwm;
  r->size_fn = size_fn;
  r->src = source;
  r->start_fn = start_fn;
  r->pull_fn = pull_fn;
  r->cancel_fn = cancel_fn;
  wb(ro);
  co = (js_object *)gc_keep(ctrl_new(ctx, PI_RC, SK_RC, ro));
  r->ctrl = co;
  wb(ro);
  if (js_is_callable(start_fn)) {
    js_value a = obj(co);
    started = js_call(ctx, start_fn, source, &a, 1);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
  }
  KEEP(started);
  after(ctx, started, JK_RS_START, obj(ro), js_undef());
  gc_pop_scope();
  return obj(ro);
}

static js_value rs_cancel_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *ro = sk_of(thisv, SK_RS);
  if (!ro)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (RS(ro)->reader)
    return p_rejected_msg(ctx, "ReadableStream is locked");
  return rs_cancel(ctx, ro, A(0));
}

static js_value rs_get_reader_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *rdo;
  js_value opts = A(0);
  if (!sk_of(thisv, SK_RS))
    return js_throw_type(ctx, "Illegal invocation");
  if (is_obj(opts)) {
    js_value mode = js_get(ctx, opts, "mode");
    if (ctx->has_exc)
      return js_undef();
    if (mode.type != JV_UNDEF) {
      js_value s = js_to_string(ctx, mode);
      if (ctx->has_exc)
        return js_undef();
      return js_throw_type(ctx,
                           !strcmp(s.u.str->data, "byob") ? "BYOB readers are not supported" : "Invalid reader mode");
    }
  }
  rdo = rd_new(ctx, js_undef(), thisv);
  return rdo ? obj(rdo) : js_undef();
}

static js_value rs_locked_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *ro = sk_of(thisv, SK_RS);
  (void)args;
  (void)argc;
  if (!ro)
    return js_throw_type(ctx, "Illegal invocation");
  return js_bool(RS(ro)->reader != 0);
}

/* ---- WritableStream core ---------------------------------------------------- */

static js_object *wr_new(js_ctx *ctx, js_value thisv, js_value streamv);

static void ws_update_ready(js_ctx *ctx, js_object *wo)
{
  struct ws *w = WS(wo);
  struct wr *wr;
  js_object *wro;
  int bp;
  if (!w->writer || w->state != W_WRITABLE)
    return;
  wro = w->writer;
  wr = WR(wro);
  bp = w->close_requested ? 0 : w->hwm - w->qsize <= 0;
  gc_push_scope();
  gc_keep(wro);
  if (bp && wr->ready_settled) {
    wr->ready = prom(ctx);
    wr->ready_settled = 0;
    wb(wro);
  } else if (!bp && !wr->ready_settled) {
    p_ok(ctx, wr->ready, js_undef());
    wr->ready_settled = 1;
  }
  gc_pop_scope();
}

static void ws_error(js_ctx *ctx, js_object *wo, js_value e)
{
  struct ws *w = WS(wo);
  struct wr *wr;
  js_object *wro;
  dq old;
  unsigned i;
  if (w->state != W_WRITABLE)
    return;
  gc_push_scope();
  gc_keep(wo);
  KEEP(e);
  w->state = W_ERRORED;
  w->err = e;
  old = w->ops;
  dq_init(&w->ops, sizeof(struct op));
  w->qsize = 0;
  wb(wo);
  for (i = 0; i < old.n; i++)
    gc_keep(((struct op *)dq_at(&old, i))->d);
  for (i = 0; i < old.n; i++)
    p_err(ctx, ((struct op *)dq_at(&old, i))->d, e);
  free(old.p);
  if (is_obj(w->abort_ctl)) {
    js_value abort = js_get(ctx, w->abort_ctl, "abort");
    if (js_is_callable(abort))
      js_call(ctx, abort, w->abort_ctl, &e, 1);
    ctx->has_exc = 0;
  }
  wro = w->writer;
  if (wro) {
    gc_keep(wro);
    wr = WR(wro);
    p_err(ctx, wr->closed, e);
    if (wr->ready_settled) {
      wr->ready = prom(ctx);
      wb(wro);
    }
    p_err(ctx, wr->ready, e);
    wr->ready_settled = 1;
  }
  gc_pop_scope();
}

static void ws_advance(js_ctx *ctx, js_object *wo)
{
  struct ws *w = WS(wo);
  struct op *front;
  js_value res = js_undef();
  if (!w->started || w->has_inflight || w->state != W_WRITABLE || w->ops.n == 0)
    return;
  gc_push_scope();
  gc_keep(wo);
  front = dq_at(&w->ops, 0);
  w->inflight = *front;
  w->has_inflight = 1;
  dq_shift(&w->ops);
  wb(wo);
  if (w->inflight.type == OP_WRITE) {
    if (w->ts)
      res = ts_write(ctx, w->ts, w->inflight.chunk);
    else if (js_is_callable(w->write_fn)) {
      js_value a[2];
      a[0] = w->inflight.chunk;
      a[1] = obj(w->ctrl);
      res = js_call(ctx, w->write_fn, w->sink, a, 2);
    }
  } else if (w->ts)
    res = ts_close(ctx, w->ts);
  else if (js_is_callable(w->close_fn))
    res = js_call(ctx, w->close_fn, w->sink, 0, 0);
  if (ctx->has_exc)
    res = js_promise_rejected(ctx, take_exc(ctx));
  KEEP(res);
  after(ctx, res, w->inflight.type == OP_WRITE ? JK_WS_WRITE : JK_WS_CLOSE, obj(wo), js_undef());
  gc_pop_scope();
}

static js_value ws_write(js_ctx *ctx, js_object *wo, js_value chunk)
{
  struct ws *w = WS(wo);
  double size;
  js_object *d;
  struct op *op;
  js_value r;
  if (w->state == W_ERRORED)
    return js_promise_rejected(ctx, w->err);
  if (w->state == W_CLOSED || w->close_requested)
    return p_rejected_msg(ctx, "Cannot write to a closing or closed stream");
  gc_push_scope();
  gc_keep(wo);
  KEEP(chunk);
  if (!chunk_size(ctx, 0, w->size_fn, chunk, &size)) {
    js_value e = take_exc(ctx);
    ws_error(ctx, wo, e);
    r = js_promise_rejected(ctx, e);
    gc_pop_scope();
    return r;
  }
  d = (js_object *)gc_keep(prom(ctx));
  op = dq_push(&w->ops);
  op->type = OP_WRITE;
  op->chunk = chunk;
  op->size = size;
  op->d = d;
  w->qsize += size;
  wb(wo);
  ws_update_ready(ctx, wo);
  ws_advance(ctx, wo);
  gc_pop_scope();
  return obj(d);
}

static js_value ws_close(js_ctx *ctx, js_object *wo)
{
  struct ws *w = WS(wo);
  js_object *d;
  struct op *op;
  if (w->state == W_ERRORED)
    return js_promise_rejected(ctx, w->err);
  if (w->state == W_CLOSED || w->close_requested)
    return p_rejected_msg(ctx, "Stream is closed or closing");
  gc_push_scope();
  gc_keep(wo);
  w->close_requested = 1;
  d = (js_object *)gc_keep(prom(ctx));
  op = dq_push(&w->ops);
  op->type = OP_CLOSE;
  op->chunk = js_undef();
  op->size = 0;
  op->d = d;
  wb(wo);
  ws_update_ready(ctx, wo);
  ws_advance(ctx, wo);
  gc_pop_scope();
  return obj(d);
}

static js_value ws_abort(js_ctx *ctx, js_object *wo, js_value reason)
{
  struct ws *w = WS(wo);
  js_value res = js_undef();
  if (w->state != W_WRITABLE)
    return p_resolved(ctx, js_undef());
  gc_push_scope();
  gc_keep(wo);
  KEEP(reason);
  if (w->ts)
    res = ts_abort(ctx, w->ts, reason);
  else if (js_is_callable(w->abort_fn))
    res = js_call(ctx, w->abort_fn, w->sink, &reason, 1);
  if (ctx->has_exc)
    res = js_promise_rejected(ctx, take_exc(ctx));
  KEEP(res);
  ws_error(ctx, wo, reason);
  res = then_undef(ctx, res);
  gc_pop_scope();
  return res;
}

/* ---- writers and controllers ------------------------------------------------ */

/* A new writer locked to the stream in streamv; NULL with the exception pending */
static js_object *wr_new(js_ctx *ctx, js_value thisv, js_value streamv)
{
  js_object *wo = sk_of(streamv, SK_WS), *wro;
  struct ws *w;
  struct wr *wr;
  if (!wo) {
    js_throw_type(ctx, "Illegal invocation");
    return 0;
  }
  w = WS(wo);
  if (w->writer) {
    js_throw_type(ctx, "WritableStream is locked");
    return 0;
  }
  gc_push_scope();
  gc_keep(wo);
  wro = (js_object *)gc_keep(host_alloc(ctx, thisv, PI_WR, SK_WR, sizeof *wr));
  wr = WR(wro);
  wr->stream = wo;
  wr->ready = prom(ctx);
  wb(wro);
  wr->closed = prom(ctx);
  w->writer = wro;
  wb(wo);
  wb(wro);
  if (w->state == W_WRITABLE)
    ws_update_ready(ctx, wo);
  else if (w->state == W_CLOSED) {
    p_ok(ctx, wr->ready, js_undef());
    wr->ready_settled = 1;
    p_ok(ctx, wr->closed, js_undef());
  } else {
    p_err(ctx, wr->ready, w->err);
    wr->ready_settled = 1;
    p_err(ctx, wr->closed, w->err);
  }
  if (w->state == W_WRITABLE && w->hwm - w->qsize > 0 && !wr->ready_settled) {
    p_ok(ctx, wr->ready, js_undef());
    wr->ready_settled = 1;
  }
  gc_pop_scope();
  return wro;
}

static void wr_release(js_ctx *ctx, js_object *wro)
{
  struct wr *wr = WR(wro);
  js_object *wo = wr->stream;
  js_value err;
  if (!wo)
    return;
  gc_push_scope();
  gc_keep(wro);
  gc_keep(wo);
  err = KEEP(terr(ctx, "Writer has been released"));
  wr->ready = prom(ctx);
  wb(wro);
  p_err(ctx, wr->ready, err);
  if (WS(wo)->state == W_WRITABLE)
    p_err(ctx, wr->closed, err);
  else {
    wr->closed = prom(ctx);
    wb(wro);
    p_err(ctx, wr->closed, err);
  }
  WS(wo)->writer = 0;
  wr->stream = 0;
  gc_pop_scope();
}

static js_value wr_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = wr_new(ctx, thisv, A(0));
  return wro ? obj(wro) : js_undef();
}

static js_value wr_closed_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  (void)args;
  (void)argc;
  return wro ? obj(WR(wro)->closed) : p_rejected_msg(ctx, "Illegal invocation");
}

static js_value wr_ready_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  (void)args;
  (void)argc;
  return wro ? obj(WR(wro)->ready) : p_rejected_msg(ctx, "Illegal invocation");
}

static js_value wr_desired_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  struct ws *w;
  (void)args;
  (void)argc;
  if (!wro)
    return js_throw_type(ctx, "Illegal invocation");
  if (!WR(wro)->stream)
    return js_throw_type(ctx, "Writer has been released");
  w = WS(WR(wro)->stream);
  if (w->state == W_ERRORED)
    return js_null();
  return js_number(w->state == W_CLOSED ? 0 : w->hwm - w->qsize);
}

static js_value wr_write_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  if (!wro)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (!WR(wro)->stream)
    return p_rejected_msg(ctx, "Writer has been released");
  return ws_write(ctx, WR(wro)->stream, A(0));
}

static js_value wr_close_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  (void)args;
  (void)argc;
  if (!wro)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (!WR(wro)->stream)
    return p_rejected_msg(ctx, "Writer has been released");
  return ws_close(ctx, WR(wro)->stream);
}

static js_value wr_abort_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  if (!wro)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (!WR(wro)->stream)
    return p_rejected_msg(ctx, "Writer has been released");
  return ws_abort(ctx, WR(wro)->stream, A(0));
}

static js_value wr_release_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro = sk_of(thisv, SK_WR);
  (void)args;
  (void)argc;
  if (!wro)
    return js_throw_type(ctx, "Illegal invocation");
  wr_release(ctx, wro);
  return js_undef();
}

static js_value wc_signal_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_WC);
  struct ws *w;
  (void)args;
  (void)argc;
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  w = WS(RC_OWNER(co));
  if (!is_obj(w->abort_ctl)) {
    js_value ac = js_get(ctx, obj(ctx->global), "AbortController");
    if (js_is_callable(ac)) {
      w->abort_ctl = js_construct(ctx, ac, 0, 0);
      wb(RC_OWNER(co));
      if (ctx->has_exc)
        return js_undef();
    }
  }
  return is_obj(w->abort_ctl) ? js_get(ctx, w->abort_ctl, "signal") : js_undef();
}

static js_value wc_error_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_WC);
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  ws_error(ctx, RC_OWNER(co), A(0));
  return js_undef();
}

/* ---- WritableStream --------------------------------------------------------- */

static js_value ws_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value sink = A(0), type, write_fn = js_undef(), close_fn = js_undef(), abort_fn = js_undef(),
           start_fn = js_undef();
  js_value started = js_undef(), size_fn;
  double hwm;
  js_object *wo, *co;
  struct ws *w;
  if (nullish(sink))
    sink = js_undef();
  else if (!is_obj(sink))
    return js_throw_type(ctx, "underlyingSink must be an object");
  if (is_obj(sink)) {
    type = js_get(ctx, sink, "type");
    if (ctx->has_exc)
      return js_undef();
    if (type.type != JV_UNDEF)
      return js_throw_range(ctx, "Invalid underlyingSink type");
    if (!get_method(ctx, sink, "write", &write_fn) || !get_method(ctx, sink, "close", &close_fn) ||
        !get_method(ctx, sink, "abort", &abort_fn) || !get_method(ctx, sink, "start", &start_fn))
      return js_undef();
  }
  if (!strategy_size(ctx, A(1), &size_fn) || !strategy_hwm(ctx, A(1), 1, &hwm))
    return js_undef();
  gc_push_scope();
  wo = (js_object *)gc_keep(host_alloc(ctx, thisv, PI_WS, SK_WS, sizeof *w));
  w = WS(wo);
  dq_init(&w->ops, sizeof(struct op));
  w->size_fn = size_fn;
  w->hwm = hwm;
  w->sink = sink;
  w->write_fn = write_fn;
  w->close_fn = close_fn;
  w->abort_fn = abort_fn;
  wb(wo);
  co = (js_object *)gc_keep(ctrl_new(ctx, PI_WC, SK_WC, wo));
  w->ctrl = co;
  wb(wo);
  if (js_is_callable(start_fn)) {
    js_value a = obj(co);
    started = js_call(ctx, start_fn, sink, &a, 1);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
  }
  KEEP(started);
  after(ctx, started, JK_WS_START, obj(wo), js_undef());
  gc_pop_scope();
  return obj(wo);
}

static js_value ws_locked_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wo = sk_of(thisv, SK_WS);
  (void)args;
  (void)argc;
  if (!wo)
    return js_throw_type(ctx, "Illegal invocation");
  return js_bool(WS(wo)->writer != 0);
}

static js_value ws_abort_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wo = sk_of(thisv, SK_WS);
  if (!wo)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (WS(wo)->writer)
    return p_rejected_msg(ctx, "WritableStream is locked");
  return ws_abort(ctx, wo, A(0));
}

static js_value ws_close_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wo = sk_of(thisv, SK_WS);
  (void)args;
  (void)argc;
  if (!wo)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (WS(wo)->writer)
    return p_rejected_msg(ctx, "WritableStream is locked");
  return ws_close(ctx, wo);
}

static js_value ws_get_writer_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *wro;
  (void)args;
  (void)argc;
  if (!sk_of(thisv, SK_WS))
    return js_throw_type(ctx, "Illegal invocation");
  wro = wr_new(ctx, js_undef(), thisv);
  return wro ? obj(wro) : js_undef();
}

/* ---- TransformStream core ---------------------------------------------------- */

static void ts_set_backpressure(js_ctx *ctx, js_object *to, int b)
{
  struct ts *t = TS(to);
  gc_push_scope();
  gc_keep(to);
  if (t->bpd)
    p_ok(ctx, t->bpd, js_undef());
  t->bpd = prom(ctx);
  t->backpressure = b;
  wb(to);
  gc_pop_scope();
}

static void ts_error_writable(js_ctx *ctx, js_object *to, js_value e)
{
  struct ts *t = TS(to);
  gc_push_scope();
  gc_keep(to);
  KEEP(e);
  ws_error(ctx, t->writable, e);
  if (t->backpressure)
    ts_set_backpressure(ctx, to, 0);
  gc_pop_scope();
}

static void ts_error(js_ctx *ctx, js_object *to, js_value e)
{
  struct ts *t = TS(to);
  gc_push_scope();
  gc_keep(to);
  KEEP(e);
  rs_error(ctx, t->readable, e);
  ts_error_writable(ctx, to, e);
  gc_pop_scope();
}

/* Returns 0 with the exception pending */
static int ts_enqueue(js_ctx *ctx, js_object *to, js_value chunk)
{
  struct ts *t = TS(to);
  struct rs *r = RS(t->readable);
  int bp;
  if (r->close_requested || r->state != R_READABLE) {
    js_throw_type(ctx, "The readable side is not enqueueable");
    return 0;
  }
  gc_push_scope();
  gc_keep(to);
  KEEP(chunk);
  if (!rs_enqueue(ctx, t->readable, chunk)) {
    js_value e = take_exc(ctx);
    ts_error_writable(ctx, to, e);
    js_throw(ctx, e);
    gc_pop_scope();
    return 0;
  }
  bp = !rs_should_pull(r);
  if (bp != t->backpressure)
    ts_set_backpressure(ctx, to, 1);
  gc_pop_scope();
  return 1;
}

/* The transformer's transform for one chunk: a promise, or any other value, or undefined with the exception pending */
static js_value ts_transform(js_ctx *ctx, js_object *to, js_value chunk)
{
  struct ts *t = TS(to);
  js_value fn, res, a[2], v = js_undef();
  int st;
  gc_push_scope();
  gc_keep(to);
  KEEP(chunk);
  fn = KEEP(js_get(ctx, t->tr, "transform"));
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  if (!js_to_boolean(fn)) {
    if (!ts_enqueue(ctx, to, chunk)) {
      js_value e = take_exc(ctx);
      ts_error(ctx, to, e);
      js_throw(ctx, e);
    }
    gc_pop_scope();
    return js_undef();
  }
  a[0] = chunk;
  a[1] = obj(t->ctrl);
  res = js_call(ctx, fn, t->tr, a, 2);
  if (ctx->has_exc) {
    js_value e = take_exc(ctx);
    ts_error(ctx, to, e);
    js_throw(ctx, e);
    gc_pop_scope();
    return js_undef();
  }
  KEEP(res);
  st = settle_state(ctx, &res, &v);
  KEEP(v);
  if (st == 1) {
    gc_pop_scope();
    return js_undef();
  }
  if (st == 2) {
    js_value r;
    ts_error(ctx, to, v);
    r = js_promise_rejected(ctx, v);
    gc_pop_scope();
    return r;
  }
  {
    js_object *d = (js_object *)gc_keep(prom(ctx));
    react_cell(ctx, res.u.obj, JK_TS_WRAP, obj(to), obj(d), 0);
    gc_pop_scope();
    return obj(d);
  }
}

/* Calls into the transformer for one chunk once the readable side wants data */
static js_value ts_go(js_ctx *ctx, js_object *to, js_value chunk)
{
  struct ts *t = TS(to);
  if (WS(t->writable)->state == W_ERRORED) {
    js_throw(ctx, WS(t->writable)->err);
    return js_undef();
  }
  return ts_transform(ctx, to, chunk);
}

/* The writable side's write: a promise, or undefined, or undefined with the exception pending */
static js_value ts_write(js_ctx *ctx, js_object *to, js_value chunk)
{
  struct ts *t = TS(to);
  js_object *d, *pair;
  js_value r;
  if (!t->backpressure)
    return ts_go(ctx, to, chunk);
  gc_push_scope();
  gc_keep(to);
  KEEP(chunk);
  d = (js_object *)gc_keep(prom(ctx));
  pair = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, pair, chunk);
  js_array_push(ctx, pair, obj(d));
  react_cell(ctx, t->bpd, JK_TS_GO, obj(to), obj(pair), 0);
  r = obj(d);
  gc_pop_scope();
  return r;
}

/* Closes the readable side once the flush is done; 0 and the error when the readable side had failed */
static int ts_flush_ok(js_ctx *ctx, js_object *to, js_value *err)
{
  struct ts *t = TS(to);
  struct rs *r = RS(t->readable);
  if (r->state == R_ERRORED) {
    *err = r->err;
    return 0;
  }
  if (r->state == R_READABLE && !r->close_requested)
    rs_close_request(ctx, t->readable);
  return 1;
}

static js_value ts_close(js_ctx *ctx, js_object *to)
{
  struct ts *t = TS(to);
  js_value fn, res = js_undef(), a, v = js_undef(), r, err = js_undef();
  int st;
  gc_push_scope();
  gc_keep(to);
  fn = KEEP(js_get(ctx, t->tr, "flush"));
  if (!ctx->has_exc && js_to_boolean(fn)) {
    a = obj(t->ctrl);
    res = js_call(ctx, fn, t->tr, &a, 1);
  }
  if (ctx->has_exc)
    res = js_promise_rejected(ctx, take_exc(ctx));
  KEEP(res);
  st = settle_state(ctx, &res, &v);
  KEEP(v);
  if (st == 1)
    r = ts_flush_ok(ctx, to, &err) ? js_undef() : js_promise_rejected(ctx, err);
  else if (st == 2) {
    ts_error(ctx, to, v);
    r = js_promise_rejected(ctx, v);
  } else {
    js_object *d = (js_object *)gc_keep(prom(ctx));
    react_cell(ctx, res.u.obj, JK_TS_CLOSE, obj(to), obj(d), 0);
    r = obj(d);
  }
  gc_pop_scope();
  return r;
}

static js_value ts_abort(js_ctx *ctx, js_object *to, js_value reason)
{
  rs_error(ctx, TS(to)->readable, reason);
  return js_undef();
}

static js_value ts_readable_pull(js_ctx *ctx, js_object *to)
{
  ts_set_backpressure(ctx, to, 0);
  return obj(TS(to)->bpd);
}

static js_value ts_readable_cancel(js_ctx *ctx, js_object *to, js_value reason)
{
  ts_error_writable(ctx, to, reason);
  return js_undef();
}

/* ---- piping ---------------------------------------------------------------- */

enum { PA_NOOP, PA_WRITER_ABORT, PA_READER_CANCEL, PA_WRITER_CLOSE };

static js_value wr_abort_i(js_ctx *ctx, js_object *wro, js_value reason)
{
  return WR(wro)->stream ? ws_abort(ctx, WR(wro)->stream, reason) : p_rejected_msg(ctx, "Writer has been released");
}

static js_value wr_write_i(js_ctx *ctx, js_object *wro, js_value chunk)
{
  return WR(wro)->stream ? ws_write(ctx, WR(wro)->stream, chunk) : p_rejected_msg(ctx, "Writer has been released");
}

static js_value wr_close_i(js_ctx *ctx, js_object *wro)
{
  return WR(wro)->stream ? ws_close(ctx, WR(wro)->stream) : p_rejected_msg(ctx, "Writer has been released");
}

static js_value rd_cancel_i(js_ctx *ctx, js_object *rdo, js_value reason)
{
  return RD(rdo)->stream ? rs_cancel(ctx, RD(rdo)->stream, reason) : p_rejected_msg(ctx, "Reader has been released");
}

static void pipe_finish(js_ctx *ctx, js_object *po, js_value err, int failed)
{
  struct pipe *p = PIPE(po);
  gc_push_scope();
  gc_keep(po);
  KEEP(err);
  wr_release(ctx, p->writer);
  rd_release(ctx, p->reader);
  if (failed)
    p_err(ctx, p->result, err);
  else
    p_ok(ctx, p->result, js_undef());
  gc_pop_scope();
}

static void pipe_shutdown(js_ctx *ctx, js_object *po, int action, js_value err, int failed)
{
  struct pipe *p = PIPE(po);
  js_value res = js_undef();
  if (p->shutting_down)
    return;
  p->shutting_down = 1;
  p->sd_err = err;
  p->sd_failed = failed;
  wb(po);
  gc_push_scope();
  gc_keep(po);
  KEEP(err);
  if (action == PA_WRITER_ABORT)
    res = wr_abort_i(ctx, p->writer, err);
  else if (action == PA_READER_CANCEL)
    res = rd_cancel_i(ctx, p->reader, err);
  else if (action == PA_WRITER_CLOSE)
    res = wr_close_i(ctx, p->writer);
  KEEP(res);
  then_inline(ctx, res, JK_PIPE_FINISH, obj(po), js_undef());
  gc_pop_scope();
}

static void pipe_source_failed(js_ctx *ctx, js_object *po, js_value e)
{
  pipe_shutdown(ctx, po, PIPE(po)->prevent_abort ? PA_NOOP : PA_WRITER_ABORT, e, 1);
}

static void pipe_dest_failed(js_ctx *ctx, js_object *po, js_value e)
{
  pipe_shutdown(ctx, po, PIPE(po)->prevent_cancel ? PA_NOOP : PA_READER_CANCEL, e, 1);
}

static void pipe_pump(js_ctx *ctx, js_object *po)
{
  struct pipe *p = PIPE(po);
  if (p->shutting_down)
    return;
  after(ctx, obj(WR(p->writer)->ready), JK_PIPE_READY, obj(po), js_undef());
}

static void pipe_aborted(js_ctx *ctx, js_object *po)
{
  struct pipe *p = PIPE(po);
  js_value e = js_get(ctx, p->signal, "reason"), acts, all, promise;
  js_object *arr;
  if (ctx->has_exc)
    return;
  gc_push_scope();
  gc_keep(po);
  if (e.type == JV_UNDEF)
    e = js_make_error(ctx, ctx->error_proto, "Error", "Aborted");
  KEEP(e);
  if (p->shutting_down) {
    gc_pop_scope();
    return;
  }
  p->shutting_down = 1;
  p->sd_err = e;
  wb(po);
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (!p->prevent_abort)
    js_array_push(ctx, arr, KEEP(wr_abort_i(ctx, p->writer, e)));
  if (!p->prevent_cancel)
    js_array_push(ctx, arr, KEEP(rd_cancel_i(ctx, p->reader, e)));
  acts = obj(arr);
  promise = KEEP(js_get(ctx, obj(ctx->global), "Promise"));
  all = KEEP(js_get(ctx, promise, "all"));
  if (js_is_callable(all)) {
    all = js_call(ctx, all, promise, &acts, 1);
    if (!ctx->has_exc)
      then_inline(ctx, KEEP(all), JK_PIPE_ABORTED, obj(po), js_undef());
  }
  gc_pop_scope();
}

static js_value pipe_on_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value po = ctx->native_data;
  (void)t;
  (void)args;
  (void)argc;
  pipe_aborted(ctx, po.u.obj);
  ctx->has_exc = 0;
  return js_undef();
}

/* Starts piping source into dest; the promise for the whole pipe, or undefined with the exception pending */
static js_value pipe_start(js_ctx *ctx, js_value srcv, js_value destv, js_value options)
{
  int pc = 0, pa = 0, pcn = 0;
  js_value signal = js_undef(), v, res;
  js_object *rdo, *wro, *po;
  struct pipe *p;
  if (!nullish(options)) {
    if (!is_obj(options))
      return js_throw_type(ctx, "options must be an object");
    v = js_get(ctx, options, "preventClose");
    if (ctx->has_exc)
      return js_undef();
    pc = js_to_boolean(v);
    v = js_get(ctx, options, "preventAbort");
    if (ctx->has_exc)
      return js_undef();
    pa = js_to_boolean(v);
    v = js_get(ctx, options, "preventCancel");
    if (ctx->has_exc)
      return js_undef();
    pcn = js_to_boolean(v);
    signal = js_get(ctx, options, "signal");
    if (ctx->has_exc)
      return js_undef();
  }
  gc_push_scope();
  KEEP(signal);
  rdo = rd_new(ctx, js_undef(), srcv);
  if (!rdo) {
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(rdo);
  wro = wr_new(ctx, js_undef(), destv);
  if (!wro) {
    js_value e = take_exc(ctx);
    rd_release(ctx, rdo);
    js_throw(ctx, e);
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(wro);
  po = (js_object *)gc_keep(host_alloc(ctx, js_undef(), -1, SK_PIPE, sizeof *p));
  p = PIPE(po);
  p->reader = rdo;
  p->writer = wro;
  p->result = prom(ctx);
  p->signal = signal;
  p->prevent_close = pc;
  p->prevent_abort = pa;
  p->prevent_cancel = pcn;
  wb(po);
  res = obj(p->result);
  after(ctx, obj(RD(rdo)->closed), JK_PIPE_SRC_CLOSED, obj(po), js_undef());
  after(ctx, obj(WR(wro)->closed), JK_PIPE_DST_CLOSED, obj(po), js_undef());
  if (js_to_boolean(signal)) {
    js_value aborted = js_get(ctx, signal, "aborted");
    if (ctx->has_exc) {
      p_err(ctx, p->result, take_exc(ctx));
      gc_pop_scope();
      return res;
    }
    if (js_to_boolean(aborted)) {
      pipe_aborted(ctx, po);
      if (ctx->has_exc)
        p_err(ctx, p->result, take_exc(ctx));
      gc_pop_scope();
      return res;
    }
    {
      js_value add = js_get(ctx, signal, "addEventListener"), a[2];
      if (!ctx->has_exc && !js_is_callable(add))
        js_throw_type(ctx, "signal.addEventListener is not a function");
      else if (!ctx->has_exc) {
        a[0] = KEEP(js_mk_cstring(ctx, "abort"));
        a[1] = obj((js_object *)gc_keep(native_with(ctx, pipe_on_abort, obj(po))));
        js_call(ctx, add, signal, a, 2);
      }
      if (ctx->has_exc) {
        p_err(ctx, p->result, take_exc(ctx));
        gc_pop_scope();
        return res;
      }
    }
  }
  pipe_pump(ctx, po);
  gc_pop_scope();
  return res;
}

static js_value rs_pipe_to_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *ro = sk_of(thisv, SK_RS), *wo = sk_of(A(0), SK_WS);
  js_value r;
  if (!ro || !wo)
    return p_rejected_msg(ctx, "Illegal invocation");
  if (RS(ro)->reader)
    return p_rejected_msg(ctx, "ReadableStream is locked");
  if (WS(wo)->writer)
    return p_rejected_msg(ctx, "WritableStream is locked");
  r = pipe_start(ctx, thisv, A(0), A(1));
  if (ctx->has_exc) {
    gc_push_scope();
    r = js_promise_rejected(ctx, take_exc(ctx));
    gc_pop_scope();
  }
  return r;
}

static js_value rs_pipe_through_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *ro = sk_of(thisv, SK_RS);
  js_value pair = A(0), readable, writable;
  if (!ro)
    return js_throw_type(ctx, "Illegal invocation");
  if (!is_obj(pair))
    return js_throw_type(ctx, "transform must be an object");
  gc_push_scope();
  readable = KEEP(js_get(ctx, pair, "readable"));
  writable = ctx->has_exc ? js_undef() : KEEP(js_get(ctx, pair, "writable"));
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  if (!sk_of(readable, SK_RS) || !sk_of(writable, SK_WS)) {
    gc_pop_scope();
    return js_throw_type(ctx, "Illegal invocation");
  }
  if (RS(ro)->reader) {
    gc_pop_scope();
    return js_throw_type(ctx, "ReadableStream is locked");
  }
  if (WS(sk_of(writable, SK_WS))->writer) {
    gc_pop_scope();
    return js_throw_type(ctx, "WritableStream is locked");
  }
  pipe_start(ctx, thisv, writable, A(1));
  gc_pop_scope();
  return ctx->has_exc ? js_undef() : readable;
}

/* ---- job dispatch ------------------------------------------------------------ */

static void job_exec(js_ctx *ctx, int kind, js_value a, js_value b, int ok, js_value v)
{
  switch (kind) {
  case JK_JS: {
    js_value f = ok ? a : b;
    if (js_is_callable(f))
      js_call(ctx, f, js_undef(), &v, 1);
    ctx->has_exc = 0;
    break;
  }
  case JK_RS_START:
    if (ok) {
      RS(a.u.obj)->started = 1;
      rs_pull(ctx, a.u.obj);
    } else
      rs_error(ctx, a.u.obj, v);
    break;
  case JK_RS_PULL:
    if (ok) {
      struct rs *r = RS(a.u.obj);
      r->pulling = 0;
      if (r->pull_again) {
        r->pull_again = 0;
        rs_pull(ctx, a.u.obj);
      }
    } else
      rs_error(ctx, a.u.obj, v);
    break;
  case JK_WS_START:
    WS(a.u.obj)->started = 1;
    if (ok)
      ws_advance(ctx, a.u.obj);
    else
      ws_error(ctx, a.u.obj, v);
    break;
  case JK_WS_WRITE:
  case JK_WS_CLOSE: {
    struct ws *w = WS(a.u.obj);
    struct op op = w->inflight;
    gc_push_scope();
    gc_keep(op.d);
    KEEP(op.chunk);
    w->has_inflight = 0;
    memset(&w->inflight, 0, sizeof w->inflight);
    if (!ok) {
      p_err(ctx, op.d, v);
      ws_error(ctx, a.u.obj, v);
    } else if (kind == JK_WS_WRITE) {
      w->qsize -= op.size;
      p_ok(ctx, op.d, js_undef());
      ws_update_ready(ctx, a.u.obj);
      ws_advance(ctx, a.u.obj);
    } else {
      if (w->state == W_WRITABLE) {
        w->state = W_CLOSED;
        if (w->writer)
          p_ok(ctx, WR(w->writer)->closed, js_undef());
      }
      p_ok(ctx, op.d, js_undef());
    }
    gc_pop_scope();
    break;
  }
  case JK_TS_START: {
    struct ts *t = TS(a.u.obj);
    if (ok)
      p_ok(ctx, t->start_d, v);
    else
      p_err(ctx, t->start_d, v);
    break;
  }
  case JK_TS_WRAP:
    if (ok)
      p_ok(ctx, b.u.obj, v);
    else {
      ts_error(ctx, a.u.obj, v);
      p_err(ctx, b.u.obj, v);
    }
    break;
  case JK_TS_GO: {
    js_object *pair = b.u.obj;
    js_value chunk = pair->elems[0], d = pair->elems[1], r;
    if (!ok) {
      p_err(ctx, d.u.obj, v);
      break;
    }
    r = ts_go(ctx, a.u.obj, chunk);
    if (ctx->has_exc)
      p_err(ctx, d.u.obj, take_exc(ctx));
    else
      js_promise_resolve_with(ctx, d.u.obj, r);
    break;
  }
  case JK_TS_CLOSE: {
    js_value err = js_undef();
    if (!ok) {
      ts_error(ctx, a.u.obj, v);
      p_err(ctx, b.u.obj, v);
    } else if (ts_flush_ok(ctx, a.u.obj, &err))
      p_ok(ctx, b.u.obj, js_undef());
    else
      p_err(ctx, b.u.obj, err);
    break;
  }
  case JK_FWD:
    if (ok)
      p_ok(ctx, a.u.obj, js_undef());
    else
      p_err(ctx, a.u.obj, v);
    break;
  case JK_PIPE_SRC_CLOSED:
    if (!ok)
      pipe_source_failed(ctx, a.u.obj, v);
    break;
  case JK_PIPE_DST_CLOSED:
    if (ok)
      pipe_dest_failed(ctx, a.u.obj, KEEP(terr(ctx, "The destination closed before the pipe finished")));
    else
      pipe_dest_failed(ctx, a.u.obj, v);
    break;
  case JK_PIPE_READY: {
    struct pipe *p = PIPE(a.u.obj);
    if (!ok || p->shutting_down)
      break;
    {
      js_object *rdo = p->reader;
      js_value rp =
          RD(rdo)->stream ? rs_read(ctx, RD(rdo)->stream, rdo) : p_rejected_msg(ctx, "Reader has been released");
      KEEP(rp);
      after(ctx, rp, JK_PIPE_READ, a, js_undef());
    }
    break;
  }
  case JK_PIPE_READ: {
    struct pipe *p = PIPE(a.u.obj);
    js_value done, chunk;
    if (!ok || p->shutting_down)
      break;
    done = js_get(ctx, v, "done");
    if (js_to_boolean(done)) {
      pipe_shutdown(ctx, a.u.obj, p->prevent_close ? PA_NOOP : PA_WRITER_CLOSE, js_undef(), 0);
      break;
    }
    chunk = KEEP(js_get(ctx, v, "value"));
    wr_write_i(ctx, p->writer, chunk);
    pipe_pump(ctx, a.u.obj);
    break;
  }
  case JK_PIPE_FINISH: {
    struct pipe *p = PIPE(a.u.obj);
    if (ok)
      pipe_finish(ctx, a.u.obj, p->sd_err, p->sd_failed);
    else
      pipe_finish(ctx, a.u.obj, v, 1);
    break;
  }
  case JK_PIPE_ABORTED: {
    struct pipe *p = PIPE(a.u.obj);
    pipe_finish(ctx, a.u.obj, ok ? p->sd_err : v, 1);
    break;
  }
  }
}

/* ---- TransformStream ---------------------------------------------------------- */

static js_value ts_ctor_impl(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value tr = A(0), wstrategy = A(1), rstrategy = A(2), rt, wt, tmp, start_fn = js_undef(), rsize = js_undef();
  js_value wsize, started = js_undef();
  double rhwm, whwm;
  js_object *to, *ro, *wo;
  struct ts *t;
  struct rs *r;
  struct ws *w;
  if (nullish(tr))
    tr = obj((js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)));
  else if (!is_obj(tr))
    return js_throw_type(ctx, "transformer must be an object");
  {
    rt = js_get(ctx, tr, "readableType");
    if (ctx->has_exc)
      return js_undef();
    if (rt.type != JV_UNDEF)
      return js_throw_range(ctx, "Invalid type");
    wt = js_get(ctx, tr, "writableType");
    if (ctx->has_exc)
      return js_undef();
    if (wt.type != JV_UNDEF)
      return js_throw_range(ctx, "Invalid type");
    if (!get_method(ctx, tr, "transform", &tmp) || !get_method(ctx, tr, "flush", &tmp) ||
        !get_method(ctx, tr, "start", &start_fn))
      return js_undef();
  }
  if (!strategy_hwm(ctx, rstrategy, 0, &rhwm))
    return js_undef();
  if (is_obj(rstrategy)) {
    rsize = js_get(ctx, rstrategy, "size");
    if (ctx->has_exc)
      return js_undef();
  }
  if (rsize.type != JV_UNDEF && !js_is_callable(rsize))
    return js_throw_type(ctx, "size must be a function");
  if (!strategy_size(ctx, wstrategy, &wsize) || !strategy_hwm(ctx, wstrategy, 1, &whwm))
    return js_undef();
  gc_push_scope();
  KEEP(rsize);
  KEEP(wsize);
  to = (js_object *)gc_keep(host_alloc(ctx, thisv, PI_TS, SK_TS, sizeof *t));
  t = TS(to);
  t->tr = tr;
  t->start_d = prom(ctx);
  wb(to);
  ro = (js_object *)gc_keep(host_alloc(ctx, js_undef(), PI_RS, SK_RS, sizeof *r));
  r = RS(ro);
  dq_init(&r->q, sizeof(struct qe));
  r->hwm = rhwm;
  r->size_fn = rsize;
  r->ts = to;
  wb(ro);
  r->ctrl = (js_object *)gc_keep(ctrl_new(ctx, PI_RC, SK_RC, ro));
  wb(ro);
  after(ctx, obj(t->start_d), JK_RS_START, obj(ro), js_undef());
  wo = (js_object *)gc_keep(host_alloc(ctx, js_undef(), PI_WS, SK_WS, sizeof *w));
  w = WS(wo);
  dq_init(&w->ops, sizeof(struct op));
  w->hwm = whwm;
  w->size_fn = wsize;
  w->ts = to;
  wb(wo);
  w->ctrl = (js_object *)gc_keep(ctrl_new(ctx, PI_WC, SK_WC, wo));
  wb(wo);
  after(ctx, obj(t->start_d), JK_WS_START, obj(wo), js_undef());
  t->readable = ro;
  t->writable = wo;
  wb(to);
  ts_set_backpressure(ctx, to, 1);
  t->ctrl = (js_object *)gc_keep(ctrl_new(ctx, PI_TC, SK_TC, to));
  wb(to);
  if (js_is_callable(start_fn)) {
    js_value a = obj(t->ctrl);
    started = js_call(ctx, start_fn, tr, &a, 1);
    if (ctx->has_exc) {
      gc_pop_scope();
      return js_undef();
    }
  }
  KEEP(started);
  after(ctx, started, JK_TS_START, obj(to), js_undef());
  gc_pop_scope();
  return obj(to);
}

static js_value ts_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value r;
  gc_push_scope();
  r = ts_ctor_impl(ctx, thisv, args, argc);
  gc_pop_scope();
  return r;
}

static js_value ts_readable_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *to = sk_of(thisv, SK_TS);
  (void)args;
  (void)argc;
  return to ? obj(TS(to)->readable) : js_throw_type(ctx, "Illegal invocation");
}

static js_value ts_writable_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *to = sk_of(thisv, SK_TS);
  (void)args;
  (void)argc;
  return to ? obj(TS(to)->writable) : js_throw_type(ctx, "Illegal invocation");
}

static js_value tc_desired_get(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_TC);
  struct rs *r;
  (void)args;
  (void)argc;
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  r = RS(TS(RC_OWNER(co))->readable);
  return r->state == R_ERRORED ? js_null() : js_number(rs_desired(r));
}

static js_value tc_enqueue_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_TC);
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  ts_enqueue(ctx, RC_OWNER(co), A(0));
  return js_undef();
}

static js_value tc_error_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_TC);
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  ts_error(ctx, RC_OWNER(co), A(0));
  return js_undef();
}

static js_value tc_terminate_m(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *co = sk_of(thisv, SK_TC), *to;
  struct rs *r;
  (void)args;
  (void)argc;
  if (!co)
    return js_throw_type(ctx, "Illegal invocation");
  to = RC_OWNER(co);
  r = RS(TS(to)->readable);
  gc_push_scope();
  if (!r->close_requested && r->state == R_READABLE)
    rs_close_request(ctx, TS(to)->readable);
  ts_error_writable(ctx, to, KEEP(terr(ctx, "The transform stream has been terminated")));
  gc_pop_scope();
  return js_undef();
}

/* ---- installation ---------------------------------------------------------------- */

static js_value n_after(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  after(ctx, A(0), JK_JS, A(1), A(2));
  return js_undef();
}

struct method {
  const char *name;
  js_native fn;
  int arity;
};
struct klass {
  const char *name;
  js_native ctor;
  int arity;
  const struct method *methods, *getters;
};

static const struct method rs_methods[] = {{"cancel", rs_cancel_m, 1},
                                           {"getReader", rs_get_reader_m, 1},
                                           {"pipeThrough", rs_pipe_through_m, 2},
                                           {"pipeTo", rs_pipe_to_m, 2},
                                           {0, 0, 0}};
static const struct method rs_getters[] = {{"locked", rs_locked_get, 0}, {0, 0, 0}};
static const struct method rd_methods[] = {
    {"cancel", rd_cancel_m, 1}, {"read", rd_read_m, 0}, {"releaseLock", rd_release_m, 0}, {0, 0, 0}};
static const struct method rd_getters[] = {{"closed", rd_closed_get, 0}, {0, 0, 0}};
static const struct method rc_methods[] = {
    {"close", rc_close_m, 0}, {"enqueue", rc_enqueue_m, 1}, {"error", rc_error_m, 1}, {0, 0, 0}};
static const struct method rc_getters[] = {{"desiredSize", rc_desired_get, 0}, {0, 0, 0}};
static const struct method ws_methods[] = {
    {"abort", ws_abort_m, 1}, {"close", ws_close_m, 0}, {"getWriter", ws_get_writer_m, 0}, {0, 0, 0}};
static const struct method ws_getters[] = {{"locked", ws_locked_get, 0}, {0, 0, 0}};
static const struct method wr_methods[] = {{"abort", wr_abort_m, 1},
                                           {"close", wr_close_m, 0},
                                           {"releaseLock", wr_release_m, 0},
                                           {"write", wr_write_m, 1},
                                           {0, 0, 0}};
static const struct method wr_getters[] = {
    {"closed", wr_closed_get, 0}, {"desiredSize", wr_desired_get, 0}, {"ready", wr_ready_get, 0}, {0, 0, 0}};
static const struct method wc_methods[] = {{"error", wc_error_m, 1}, {0, 0, 0}};
static const struct method wc_getters[] = {{"signal", wc_signal_get, 0}, {0, 0, 0}};
static const struct method ts_getters[] = {
    {"readable", ts_readable_get, 0}, {"writable", ts_writable_get, 0}, {0, 0, 0}};
static const struct method tc_methods[] = {
    {"enqueue", tc_enqueue_m, 1}, {"error", tc_error_m, 1}, {"terminate", tc_terminate_m, 0}, {0, 0, 0}};
static const struct method tc_getters[] = {{"desiredSize", tc_desired_get, 0}, {0, 0, 0}};

static const struct klass k_classes[PI_N] = {
    [PI_RS] = {"ReadableStream", rs_ctor, 2, rs_methods, rs_getters},
    [PI_RD] = {"ReadableStreamDefaultReader", rd_ctor, 1, rd_methods, rd_getters},
    [PI_RC] = {"ReadableStreamDefaultController", illegal_ctor, 2, rc_methods, rc_getters},
    [PI_WS] = {"WritableStream", ws_ctor, 2, ws_methods, ws_getters},
    [PI_WR] = {"WritableStreamDefaultWriter", wr_ctor, 1, wr_methods, wr_getters},
    [PI_WC] = {"WritableStreamDefaultController", illegal_ctor, 2, wc_methods, wc_getters},
    [PI_TS] = {"TransformStream", ts_ctor, 3, 0, ts_getters},
    [PI_TC] = {"TransformStreamDefaultController", illegal_ctor, 2, tc_methods, tc_getters},
};

js_object *streams_core_install(js_ctx *ctx)
{
  js_object *n, *protos;
  int i;
  gc_push_scope();
  n = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  protos = (js_object *)gc_keep(js_new_array(ctx, 0));
  g_jq = 0;
  g_jhead = 0;
  g_jrunning = 0;
  for (i = 0; i < PI_N; i++) {
    const struct klass *k = &k_classes[i];
    const struct method *m;
    js_object *proto = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)), *ctor;
    g_proto[i] = proto;
    js_array_push(ctx, protos, obj(proto));
    ctor = (js_object *)gc_keep(js_new_native(ctx, k->ctor, k->name, k->arity));
    js_def(ctor, "prototype", obj(proto), 0);
    js_def(proto, "constructor", obj(ctor), P_WRITABLE | P_CONFIGURABLE);
    js_def(n, k->name, obj(ctor), ALL);
    for (m = k->methods; m && m->name; m++)
      js_def_native(ctx, proto, m->name, m->fn, m->arity);
    for (m = k->getters; m && m->name; m++) {
      js_object *get = (js_object *)gc_keep(js_new_native(ctx, m->fn, m->name, 0));
      js_def_accessor(proto, m->name, get, 0, P_CONFIGURABLE);
    }
  }
  js_def(ctx->global, "@sprotos", obj(protos), 0);
  js_def_native(ctx, n, "after", n_after, 3);
  js_host_trace = sk_trace;
  if (!js_host_finalizer)
    js_host_finalizer = streams_core_free;
  gc_pop_scope();
  return n;
}
