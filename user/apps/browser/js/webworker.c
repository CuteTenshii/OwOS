/* Dedicated Workers on OS threads. Each worker is a thread with its own GC heap and js_ctx; it shares no
 * object with the page. Messages cross as structured clones (js_clone_*) through mutex-guarded queues: the
 * page drains its side in js_workers_pump, the worker sleeps on a futex until something arrives or a timer
 * is due. A SharedArrayBuffer crosses by sharing its refcounted store. Everything that touches page state
 * (fetching scripts, the devtools console) is done on the page thread: the worker asks over a one-slot RPC and
 * waits for the answer. */

#include "base64.h"
#include "bindings.h"
#include "canvasjs.h"
#include "features/cors/cors.h"
#include "fetchint.h"
#include "mutex.h"
#include "websocket.h"

#define JS(...) #__VA_ARGS__

/* The JavaScript half of the worker global scope; each part is a function called as f(globalThis, N, L) */
static const char k_events_js[] =
#include "workerscope_prelude.inc"
    ;
static const char k_web_js[] =
#include "workerscope_web.inc"
    ;
static const char k_net_js[] =
#include "workerscope_net.inc"
    ;
static const char *const k_parts[] = {k_events_js, k_web_js, k_net_js};

/* WM_REQUEST: a worker asks the page thread to do something; WM_REPLY (level = request id) answers it */
/* WM_PORT / WM_PORT_CLOSE: traffic for a MessagePort one of whose ends moved to the other thread (level = port id)
 * Workers started by a worker: the page thread relays between parent and child. WM_CHILD_POST / WM_CHILD_TERM go from
 * the parent to the page (level = child id); WM_CHILD_DATA / WM_CHILD_ERROR go from the page to the parent. */
enum {
  WM_DATA,
  WM_ERROR,
  WM_LOG,
  WM_REQUEST,
  WM_REPLY,
  WM_PORT,
  WM_PORT_CLOSE,
  WM_CHILD_POST,
  WM_CHILD_TERM,
  WM_CHILD_DATA,
  WM_CHILD_ERROR,
  WM_BCAST,  /* a BroadcastChannel message (text = channel name), in either direction */
  WM_CONNECT /* a page connects to a shared worker: the clone carries the port for it */
};

struct wmsg {
  struct wmsg *next;
  int kind;
  int level; /* WM_LOG: JS_LOG_*; WM_REPLY: the request id */
  js_clone *clone;
  char *text;
};
struct wq {
  struct wmsg *head, *tail;
};

#define WORKER_MAX 16               /* thread stacks come from 32 shared slots */
#define WORKER_STACK (240 * 1024)   /* at most 63 pages per guarded thread stack */
#define WORKER_HEADROOM (40 * 1024) /* below this the engine throws instead of recursing */

struct wthread {
  int id;
  struct wthread *parent; /* the worker that started this one, or NULL for a page worker */
  int shared;             /* a SharedWorker: pages with the same URL and name connect to it */
  int worklet;            /* the AudioWorkletGlobalScope of a context: no script of its own, it renders audio */
  int tid;
  void *stack;
  char *stack_lo;
  mutex_t lock; /* guards both queues */
  struct wq in, out;
  unsigned seq; /* bumped whenever the worker has something new to look at */
  js_irq irq;   /* irq.stop: the page wants the worker gone, even mid-script */
  int closing;  /* the worker called close(): finish the current task, then end */
  int done;     /* the thread has finished running JS */
  unsigned rpc; /* 0 idle, 1 the worker is waiting for the page, 2 answered */
  char rpc_base[URL_MAX], rpc_spec[URL_MAX], rpc_url[URL_MAX];
  char *rpc_src;
  const char *rpc_err;
  char *src; /* the main script, NULL when it could not be fetched */
  int module;
  char url[URL_MAX], origin[URL_MAX], pathname[URL_MAX], host[256], name[128];
  int tls;
  js_arena **arenas; /* parsed scripts, kept for the thread's lifetime */
  int narenas;
};

static js_object *g_worker_proto;  /* Worker.prototype */
static js_object *g_sworker_proto; /* SharedWorker.prototype */
static js_object *g_wq;            /* MessagePort traffic: flat (port, data, ports) triples, drained by the pump */
static int g_next_wid;             /* worker ids, unique across the page and every worker (atomic) */

/* Live workers and their page-side objects, index for index; the objects are GC roots */
static struct wthread *g_workers[WORKER_MAX];
static js_object *g_wobjs[WORKER_MAX];
static int g_nworkers;

static __thread struct wthread *cur_wt;

int js_on_worker_thread(void) { return cur_wt != 0; }

/* The parts of the JavaScript scope run so far: the core always, URL/Blob/... and fetch/WebSocket/... on first use */
static __thread unsigned parts_loaded;
enum { PART_WEB = 1, PART_NET = 2 };
static bool wt_load_parts(js_ctx *ctx, unsigned upto);

/* Port ids are unique across the page and every worker */
static unsigned g_next_port_id;

/* The worker a message is being cloned for (or from), so a MessagePort inside it can be tied to that worker */
static __thread struct wthread *clone_peer;

/* The MessagePorts that structured clones made while being read, in order: the event.ports of the message being
 * delivered. Readers clear the list first and take it afterwards. */
static void ports_clear(js_ctx *ctx) { js_def(ctx->global, "@rports", js_obj_value(js_new_array(ctx, 0)), P_WRITABLE); }

static void ports_note(js_ctx *ctx, js_value port)
{
  js_value a = js_get(ctx, js_obj_value(ctx->global), "@rports");
  if (a.type == JV_OBJ)
    js_array_push(ctx, a.u.obj, port);
}

static js_value ports_take(js_ctx *ctx)
{
  js_value a = js_get(ctx, js_obj_value(ctx->global), "@rports");
  js_object *r;
  unsigned i;
  gc_push_scope();
  if (a.type == JV_OBJ)
    gc_keep(a.u.obj);
  r = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; a.type == JV_OBJ && i < js_array_len(a.u.obj); i++)
    js_array_push(ctx, r, a.u.obj->elems[i]);
  ports_clear(ctx);
  gc_pop_scope();
  return js_obj_value(r);
}

/* A local clone moves the MessagePorts inside it by identity: the clone carries each port's slot in "@lports" */
static __thread int g_local_clone;

static int ports_local_write(js_ctx *ctx, js_value port, char kind[16], unsigned char **bytes, int *len)
{
  js_value t = js_get(ctx, js_obj_value(ctx->global), "@lports");
  int idx;
  if (t.type != JV_OBJ)
    return 0;
  idx = (int)js_array_len(t.u.obj);
  *bytes = (unsigned char *)malloc(sizeof idx);
  if (!*bytes)
    return 0;
  memcpy(*bytes, &idx, sizeof idx);
  *len = (int)sizeof idx;
  strcpy(kind, "lport");
  js_array_push(ctx, t.u.obj, port);
  return 1;
}

static js_value ports_local_read(js_ctx *ctx, const unsigned char *bytes, int len)
{
  js_value t = js_get(ctx, js_obj_value(ctx->global), "@lports"), port;
  int idx;
  if (t.type != JV_OBJ || len != (int)sizeof idx)
    return js_undef();
  memcpy(&idx, bytes, sizeof idx);
  port = js_get_index(ctx, t, (unsigned)idx);
  ports_note(ctx, port);
  return port;
}

/* Clones `v` within this context, ports moving by identity; the clone's value and the ports it carries.
 * 0 with the exception pending. */
int js_local_clone(js_ctx *ctx, js_value v, js_value transfer, js_value *data, js_value *ports)
{
  js_clone *c;
  int ok = 0;
  gc_push_scope();
  js_def(ctx->global, "@lports", js_obj_value(js_new_array(ctx, 0)), P_WRITABLE);
  ports_clear(ctx);
  g_local_clone = 1;
  c = js_clone_write(ctx, v, transfer);
  if (c) {
    *data = js_clone_read(ctx, c);
    js_clone_free(c);
    ok = !ctx->has_exc;
  }
  g_local_clone = 0;
  if (ok) {
    if (data->type == JV_OBJ)
      gc_keep(data->u.obj);
    *ports = ports_take(ctx);
  }
  gc_pop_scope();
  return ok;
}

/* Page ports whose other end moved into a worker (or that arrived from one): id, owner, local port */
#define RPORT_MAX 64
static js_object *g_rport_objs[RPORT_MAX];
static int g_rport_ids[RPORT_MAX];
static struct wthread *g_rport_wt[RPORT_MAX];
static int g_nrports;

static js_value throw_err(js_ctx *ctx, const char *name, const char *msg)
{
  js_throw(ctx, strcmp(name, "TypeError") ? dom_exception(ctx, name, msg)
                                          : js_make_error(ctx, ctx->type_error_proto, name, msg));
  return js_undef();
}

/* blob: and data: URLs are absolute and must not be joined to the base */
static void script_url(const char *base, const char *spec, char *out, int cap)
{
  if (starts_ci(spec, "blob:") || starts_ci(spec, "data:"))
    snprintf(out, (unsigned)cap, "%s", spec);
  else
    resolve_url(base, spec, out, cap);
}

/* ------------------------------------------------------- queues ------------ */

static void wmsg_free(struct wmsg *m)
{
  js_clone_free(m->clone);
  free(m->text);
  free(m);
}

static void wq_put(struct wthread *wt, struct wq *q, struct wmsg *m)
{
  m->next = 0;
  mutex_lock(&wt->lock);
  if (q->tail)
    q->tail->next = m;
  else
    q->head = m;
  q->tail = m;
  mutex_unlock(&wt->lock);
}

static struct wmsg *wq_take_all(struct wthread *wt, struct wq *q)
{
  struct wmsg *m;
  mutex_lock(&wt->lock);
  m = q->head;
  q->head = q->tail = 0;
  mutex_unlock(&wt->lock);
  return m;
}

/* One message, or NULL; *seq is the wake counter read in the same critical section, so sleeping on it
 * cannot miss a message that arrives afterwards. */
static struct wmsg *wq_take_one(struct wthread *wt, struct wq *q, unsigned *seq)
{
  struct wmsg *m;
  mutex_lock(&wt->lock);
  m = q->head;
  if (m) {
    q->head = m->next;
    if (!q->head)
      q->tail = 0;
  }
  *seq = __atomic_load_n(&wt->seq, __ATOMIC_ACQUIRE);
  mutex_unlock(&wt->lock);
  return m;
}

static void wthread_wake(struct wthread *wt)
{
  __atomic_fetch_add(&wt->seq, 1, __ATOMIC_ACQ_REL);
  futex_wake(&wt->seq, 1);
}

/* Make the worker stop, even inside a script or a blocked Atomics.wait */
static void wthread_stop(struct wthread *wt)
{
  unsigned *waiting;
  __atomic_store_n(&wt->irq.stop, 1, __ATOMIC_RELEASE);
  wthread_wake(wt);
  futex_wake(&wt->rpc, 1);
  waiting = __atomic_load_n(&wt->irq.waiting, __ATOMIC_ACQUIRE);
  if (waiting)
    futex_wake(waiting, 1 << 30);
}

static void wmsg_post(struct wthread *wt, struct wq *q, int kind, int level, js_clone *clone, const char *text)
{
  struct wmsg *m = (struct wmsg *)calloc(1, sizeof *m);
  if (!m) {
    js_clone_free(clone);
    return;
  }
  m->kind = kind;
  m->level = level;
  m->clone = clone;
  if (text) {
    int n = (int)strlen(text);
    m->text = (char *)malloc((unsigned)n + 1);
    if (m->text)
      memcpy(m->text, text, (unsigned)n + 1);
  }
  wq_put(wt, q, m);
  if (q == &wt->in)
    wthread_wake(wt);
}

/* ------------------------------------------------------- worker thread ------ */

/* console.* from a worker goes to the page's devtools console through the out queue */
static void wt_print(js_ctx *ctx, int level, const char *s, int len)
{
  char text[1024];
  (void)ctx;
  if (len >= (int)sizeof text)
    len = (int)sizeof text - 1;
  memcpy(text, s, (unsigned)len);
  text[len] = 0;
  wmsg_post(cur_wt, &cur_wt->out, WM_LOG, level, 0, text);
}

/* Send the pending exception to the page as an error event and clear it */
static void wt_report(js_ctx *ctx)
{
  js_value e = ctx->exc, text;
  ctx->has_exc = 0;
  ctx->exc_root = 0;
  gc_push_scope();
  if (e.type == JV_OBJ) {
    js_value m = js_get(ctx, e, "message");
    gc_keep(e.u.obj);
    if (m.type == JV_STR)
      e = m;
  }
  text = js_to_string(ctx, e);
  if (ctx->has_exc) {
    ctx->has_exc = 0;
    ctx->exc_root = 0;
  } else if (text.type == JV_STR) {
    static __thread bool reporting;
    bool handled = false;
    js_value n = js_get(ctx, js_obj_value(ctx->global), "@N"),
             hook = n.type == JV_OBJ ? js_get(ctx, n, "onerror") : js_undef();
    gc_keep(text.u.str);
    if (!reporting && js_is_callable(hook)) { /* self.onerror and error listeners may swallow it */
      js_value r;
      reporting = true;
      r = js_call(ctx, hook, n, &text, 1);
      handled = !ctx->has_exc && js_to_boolean(r);
      ctx->has_exc = 0;
      ctx->exc_root = 0;
      reporting = false;
    }
    if (!handled)
      wmsg_post(cur_wt, &cur_wt->out, WM_ERROR, 0, 0, text.u.str->data);
  }
  gc_pop_scope();
}

static void wt_keep_arena(struct wthread *wt, js_arena *ar)
{
  js_arena **na = (js_arena **)realloc(wt->arenas, (unsigned)(wt->narenas + 1) * sizeof *na);
  if (!na) {
    return; /* the AST then lives until the thread is torn down by exit */
  }
  wt->arenas = na;
  na[wt->narenas++] = ar;
}

/* Parse and run `src` in the worker's global scope (or as a module). A failure leaves its exception pending. */
static bool wt_eval(js_ctx *ctx, struct wthread *wt, const char *src, const char *label, js_object *module_exports,
                    js_value *result)
{
  js_arena *ar = (js_arena *)malloc(sizeof *ar);
  js_parse_error pe;
  js_node *prog;
  js_value r = js_undef();
  bool ok;
  if (!ar)
    return false;
  js_arena_init(ar);
  prog = js_parse(src, ar, &pe);
  if (!prog) {
    js_arena_free(ar);
    free(ar);
    throw_err(ctx, "SyntaxError", pe.msg ? pe.msg : "syntax error");
    return false;
  }
  wt_keep_arena(wt, ar);
  js_set_script(ctx, label);
  ok = module_exports ? js_run_module(ctx, prog, module_exports, label) == 0 : js_run(ctx, prog, &r) == 0;
  if (result)
    *result = r;
  return ok;
}

/* Ask the page thread to resolve `spec` against `base` and fetch it. On success *src is malloc'd and
 * wt->rpc_url holds the resolved URL; otherwise the error text is returned. */
static const char *wt_fetch(struct wthread *wt, const char *base, const char *spec, char **src)
{
  const char *err;
  snprintf(wt->rpc_base, sizeof wt->rpc_base, "%s", base);
  snprintf(wt->rpc_spec, sizeof wt->rpc_spec, "%s", spec);
  __atomic_store_n(&wt->rpc, 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&wt->rpc, __ATOMIC_ACQUIRE) == 1 && !__atomic_load_n(&wt->irq.stop, __ATOMIC_ACQUIRE))
    futex_wait(&wt->rpc, 1, 50);
  if (__atomic_load_n(&wt->rpc, __ATOMIC_ACQUIRE) != 2) {
    __atomic_store_n(&wt->rpc, 0, __ATOMIC_RELEASE);
    return "worker is closing";
  }
  *src = wt->rpc_src;
  err = wt->rpc_err;
  wt->rpc_src = 0;
  __atomic_store_n(&wt->rpc, 0, __ATOMIC_RELEASE);
  return err;
}

static js_value wt_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_clone *c = js_clone_write(ctx, A(0), A(1));
  (void)t;
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_DATA, 0, c, 0);
  return js_undef();
}

static js_value wt_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  __atomic_store_n(&cur_wt->closing, 1, __ATOMIC_RELEASE);
  return js_undef();
}

static js_value wt_import_scripts(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int i;
  (void)t;
  for (i = 0; i < argc && !ctx->has_exc; i++) {
    js_value s = js_to_string(ctx, args[i]);
    char spec[URL_MAX];
    char *src = 0;
    const char *err;
    if (s.type != JV_STR)
      break;
    snprintf(spec, sizeof spec, "%s", s.u.str->data);
    err = wt_fetch(cur_wt, cur_wt->url, spec, &src);
    if (err) {
      free(src);
      return throw_err(ctx, strcmp(err, "cross-origin") ? "TypeError" : "SecurityError", "importScripts failed");
    }
    wt_eval(ctx, cur_wt, src, cur_wt->url, 0, NULL);
    free(src);
  }
  return js_undef();
}

/* Timers: the table is plain C; each callback and its arguments sit in the hidden global __timers as an array
 * under "t<id>", which keeps them alive. */
struct wtimer {
  int id;
  unsigned due, period;
  bool repeat;
};
static __thread struct wtimer *timers;
static __thread int ntimers, captimers, next_timer_id;

static js_value wt_set_timer(js_ctx *ctx, js_value *args, int argc, bool repeat)
{
  js_value store = js_get(ctx, js_obj_value(ctx->global), "@timers");
  double delay = argc > 1 ? js_to_number(ctx, args[1]) : 0;
  struct wtimer *t;
  char key[24];
  int k;
  if (!js_is_callable(A(0)) || store.type != JV_OBJ)
    return js_number(0);
  if (ntimers == captimers) {
    int nc = captimers ? captimers * 2 : 16;
    struct wtimer *nt = (struct wtimer *)realloc(timers, (unsigned)nc * sizeof *nt);
    if (!nt)
      return js_number(0);
    timers = nt;
    captimers = nc;
  }
  t = &timers[ntimers++];
  t->id = ++next_timer_id;
  t->period = delay != delay || delay < 1 ? (repeat ? 1 : 0) : (unsigned)delay;
  t->due = msec() + t->period;
  t->repeat = repeat;
  snprintf(key, sizeof key, "t%d", t->id);
  gc_push_scope();
  {
    js_object *bundle = (js_object *)gc_keep(js_new_array(ctx, 0));
    js_array_push(ctx, bundle, args[0]);
    for (k = 2; k < argc; k++)
      js_array_push(ctx, bundle, args[k]);
    js_set(ctx, store, key, js_obj_value(bundle));
  }
  gc_pop_scope();
  return js_number(t->id);
}

static js_value wt_set_timeout(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return wt_set_timer(ctx, args, argc, false);
}

static js_value wt_set_interval(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return wt_set_timer(ctx, args, argc, true);
}

static void timer_remove(js_ctx *ctx, int id)
{
  js_value store = js_get(ctx, js_obj_value(ctx->global), "@timers");
  char key[24];
  int i;
  for (i = 0; i < ntimers; i++)
    if (timers[i].id == id) {
      timers[i] = timers[--ntimers];
      snprintf(key, sizeof key, "t%d", id);
      if (store.type == JV_OBJ)
        js_set(ctx, store, key, js_undef());
      return;
    }
}

static js_value wt_clear_timer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (argc > 0 && args[0].type == JV_NUM)
    timer_remove(ctx, (int)args[0].u.num);
  return js_undef();
}

static void wt_run_microtasks(js_ctx *ctx)
{
  js_value list = js_get(ctx, js_obj_value(ctx->global), "@micro");
  if (list.type != JV_OBJ)
    return;
  while (js_array_len(list.u.obj) && !js_irq_stopped(ctx->irq)) {
    js_value fn = js_get_index(ctx, list, 0), shift = js_get(ctx, list, "shift");
    gc_push_scope();
    if (fn.type == JV_OBJ)
      gc_keep(fn.u.obj);
    js_call(ctx, shift, list, 0, 0);
    if (js_is_callable(fn))
      js_call(ctx, fn, js_undef(), 0, 0);
    if (ctx->has_exc)
      wt_report(ctx);
    gc_pop_scope();
  }
}

/* Run the earliest due timer; false when none is due */
static bool fire_due_timer(js_ctx *ctx)
{
  unsigned now = msec();
  int best = -1, i;
  js_value store, bundle, argv[8];
  char key[24];
  unsigned n, k;
  int id;
  bool repeat;
  for (i = 0; i < ntimers; i++)
    if ((int)(timers[i].due - now) <= 0 && (best < 0 || (int)(timers[i].due - timers[best].due) < 0))
      best = i;
  if (best < 0)
    return false;
  id = timers[best].id;
  repeat = timers[best].repeat;
  snprintf(key, sizeof key, "t%d", id);
  store = js_get(ctx, js_obj_value(ctx->global), "@timers");
  bundle = js_get(ctx, store, key);
  gc_push_scope();
  if (bundle.type == JV_OBJ) {
    gc_keep(bundle.u.obj);
    n = js_array_len(bundle.u.obj);
    for (k = 1; k < n && k <= 8; k++)
      argv[k - 1] = js_get_index(ctx, bundle, k);
    if (repeat)
      timers[best].due = now + timers[best].period;
    else
      timer_remove(ctx, id);
    {
      js_value fn = js_get_index(ctx, bundle, 0);
      if (js_is_callable(fn)) {
        js_call(ctx, fn, js_obj_value(ctx->global), argv, n > 1 ? (int)(n - 1 > 8 ? 8 : n - 1) : 0);
        if (ctx->has_exc)
          wt_report(ctx);
        wt_run_microtasks(ctx);
      }
    }
  } else
    timer_remove(ctx, id);
  gc_pop_scope();
  return true;
}

/* queueMicrotask: callbacks wait in a list and run when the current task ends, before the next message or timer */
static js_value wt_queue_microtask(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value list = js_get(ctx, js_obj_value(ctx->global), "@micro");
  (void)t;
  if (!js_is_callable(A(0)))
    return js_throw_type(ctx, "queueMicrotask: callback must be a function");
  if (list.type == JV_OBJ)
    js_array_push(ctx, list.u.obj, args[0]);
  return js_undef();
}

static js_value wt_perf_now(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  static __thread unsigned origin;
  unsigned now = msec();
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  if (!origin)
    origin = now;
  return js_number((double)(now - origin));
}

static js_object *wt_load_module(js_ctx *ctx, const char *spec, void *ud)
{
  struct wthread *wt = (struct wthread *)ud;
  js_value cache = js_get(ctx, js_obj_value(ctx->global), "@modules"), hit;
  js_object *exports;
  char *src = 0;
  char url[URL_MAX];
  const char *err = wt_fetch(wt, ctx->cur_module_url ? ctx->cur_module_url : wt->url, spec, &src);
  if (err) {
    free(src);
    throw_err(ctx, "TypeError", "module load failed");
    return NULL;
  }
  snprintf(url, sizeof url, "%s", wt->rpc_url);
  hit = cache.type == JV_OBJ ? js_get(ctx, cache, url) : js_undef();
  if (hit.type == JV_OBJ) {
    free(src);
    return hit.u.obj;
  }
  exports = js_new_object(ctx, ctx->object_proto);
  gc_push_scope();
  gc_keep(exports);
  if (cache.type == JV_OBJ)
    js_set(ctx, cache, url, js_obj_value(exports));
  wt_eval(ctx, wt, src, wt->url, exports, NULL);
  gc_pop_scope();
  free(src);
  return ctx->has_exc ? NULL : exports;
}

/* A worker's Blob (or File) crosses by its bytes and type */
static int wt_clone_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta)
{
  js_value ctor, proto, data, type;
  js_object *p;
  const unsigned char *src;
  int n = 0;
  if (parts_loaded <= PART_WEB) /* no Blob can exist yet */
    return 0;
  ctor = js_get(ctx, js_obj_value(ctx->global), "Blob");
  proto = ctor.type == JV_OBJ ? js_get(ctx, ctor, "prototype") : js_undef();
  if (proto.type != JV_OBJ)
    return 0;
  for (p = v.u.obj->proto; p && p != proto.u.obj; p = p->proto)
    ;
  if (!p)
    return 0;
  data = js_get(ctx, v, "@bytes");
  type = js_get(ctx, v, "type");
  src = view_bytes(data, &n);
  *bytes = (unsigned char *)malloc((unsigned)n + 1);
  if (!src || !*bytes)
    return 0;
  memcpy(*bytes, src, (unsigned)n);
  *len = n;
  *meta = type.type == JV_STR ? strdup(type.u.str->data) : 0;
  strcpy(kind, "blob");
  return 1;
}

/* A MessagePort of this worker moving to the page: N.portTransfer tells the script side to route its peer */
static int wt_clone_write_port(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len)
{
  js_value n, ctor, proto, fn, r;
  js_object *p;
  int rid;
  if (parts_loaded <= PART_NET) /* no MessagePort can exist yet */
    return 0;
  n = js_get(ctx, js_obj_value(ctx->global), "@N");
  ctor = js_get(ctx, js_obj_value(ctx->global), "MessagePort");
  proto = ctor.type == JV_OBJ ? js_get(ctx, ctor, "prototype") : js_undef();
  if (proto.type != JV_OBJ || n.type != JV_OBJ)
    return 0;
  for (p = v.u.obj->proto; p && p != proto.u.obj; p = p->proto)
    ;
  fn = js_get(ctx, n, "portTransfer");
  if (p && g_local_clone)
    return ports_local_write(ctx, v, kind, bytes, len);
  if (!p || !js_is_callable(fn))
    return 0;
  r = js_call(ctx, fn, n, &v, 1);
  rid = r.type == JV_NUM ? (int)r.u.num : 0;
  if (ctx->has_exc || rid <= 0)
    return 0;
  *bytes = (unsigned char *)malloc(sizeof rid);
  if (!*bytes)
    return 0;
  memcpy(*bytes, &rid, sizeof rid);
  *len = (int)sizeof rid;
  strcpy(kind, "port");
  return 1;
}

static int wt_clone_write_any(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta)
{
  return wt_clone_write(ctx, v, kind, bytes, len, meta) || canvas_clone_write(ctx, v, kind, bytes, len, meta) ||
         wt_clone_write_port(ctx, v, kind, bytes, len);
}

static js_value wt_clone_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len, const char *meta)
{
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "Blob"), args[2], parts, r;
  js_object *opts;
  if (streq(kind, "ocanvas") || streq(kind, "bitmap"))
    return canvas_clone_read(ctx, kind, bytes, len);
  if (streq(kind, "lport"))
    return ports_local_read(ctx, bytes, len);
  if (streq(kind, "port")) { /* a port arrives: the script side builds it */
    js_value n, fn, id, port;
    if (!wt_load_parts(ctx, PART_NET))
      return js_undef();
    n = js_get(ctx, js_obj_value(ctx->global), "@N");
    fn = n.type == JV_OBJ ? js_get(ctx, n, "portReceive") : js_undef();
    if (!js_is_callable(fn) || len != (int)sizeof(int))
      return js_undef();
    memcpy(&len, bytes, sizeof len);
    id = js_number(len);
    port = js_call(ctx, fn, n, &id, 1);
    if (!ctx->has_exc && port.type == JV_OBJ)
      ports_note(ctx, port);
    return port;
  }
  if (!streq(kind, "blob") || !js_is_callable(ctor))
    return js_undef();
  gc_push_scope();
  args[0] = js_obj_value(gc_keep(js_new_array(ctx, 0)));
  parts = js_new_typedarray(ctx, TA_U8, len);
  gc_keep(parts.u.obj);
  if (len > 0)
    memcpy(((js_arraybuffer *)((js_typedarray *)parts.u.obj->host)->buffer->host)->data, bytes, (unsigned)len);
  js_array_push(ctx, args[0].u.obj, parts);
  opts = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(opts, "type", js_mk_cstring(ctx, meta ? meta : ""), P_ENUMERABLE);
  args[1] = js_obj_value(opts);
  r = js_construct(ctx, ctor, args, 2);
  gc_pop_scope();
  return r;
}

static void wt_def_location(js_ctx *ctx, struct wthread *wt)
{
  js_object *loc;
  gc_push_scope();
  loc = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(loc, "href", js_mk_cstring(ctx, wt->url), P_ENUMERABLE);
  js_def(loc, "origin", js_mk_cstring(ctx, wt->origin), P_ENUMERABLE);
  js_def(loc, "pathname", js_mk_cstring(ctx, wt->pathname), P_ENUMERABLE);
  js_def(loc, "protocol", js_mk_cstring(ctx, wt->tls ? "https:" : "http:"), P_ENUMERABLE);
  js_def(loc, "host", js_mk_cstring(ctx, wt->host), P_ENUMERABLE);
  js_def(loc, "hostname", js_mk_cstring(ctx, wt->host), P_ENUMERABLE);
  js_def(loc, "search", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(loc, "hash", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(ctx->global, "location", js_obj_value(loc), P_WRITABLE | P_ENUMERABLE);
  js_def(ctx->global, "name", js_mk_cstring(ctx, wt->name), P_WRITABLE | P_ENUMERABLE);
  gc_pop_scope();
}

static js_value wt_n_utf8encode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0)), r;
  (void)t;
  if (s.type != JV_STR)
    return js_undef();
  gc_push_scope();
  gc_keep(s.u.str);
  r = js_new_typedarray(ctx, TA_U8, s.u.str->len);
  if (r.type == JV_OBJ)
    memcpy(((js_arraybuffer *)((js_typedarray *)r.u.obj->host)->buffer->host)->data, s.u.str->data,
           (unsigned)s.u.str->len);
  gc_pop_scope();
  return r;
}

/* N.encodeInto(string, view): {read, written} */
static js_value wt_n_encode_into(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0)), r;
  int len = 0, nread = 0, nwritten = 0;
  const unsigned char *p = view_bytes(A(1), &len);
  js_object *ro;
  (void)t;
  if (s.type == JV_STR && p)
    text_encode_into(s.u.str->data, s.u.str->len, (unsigned char *)p, len, &nread, &nwritten);
  gc_push_scope();
  ro = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ro, "read", js_number(nread), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  js_def(ro, "written", js_number(nwritten), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  r = js_obj_value(ro);
  gc_pop_scope();
  return r;
}

static js_value wt_n_encoding_name(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0));
  (void)t;
  return s.type == JV_STR ? js_mk_cstring(ctx, text_decoder_name(text_decoder_kind(s.u.str->data))) : js_null();
}

/* N.decode(bytes, encodingName, ignoreBOM, fatal) */
static js_value wt_n_decode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value label = js_to_string(ctx, A(1));
  (void)t;
  if (label.type != JV_STR)
    return js_undef();
  return text_decode(ctx, A(0), text_decoder_kind(label.u.str->data), js_to_boolean(A(2)), js_to_boolean(A(3)));
}

static js_value wt_n_random_fill(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int len = 0;
  const unsigned char *p = view_bytes(A(0), &len);
  (void)ctx;
  (void)t;
  if (p && len > 0)
    rng_bytes((void *)p, len);
  return js_undef();
}

/* N.base64(bytes) */
static js_value wt_n_base64(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int len = 0;
  const unsigned char *p = view_bytes(A(0), &len);
  char *out;
  js_value r;
  (void)t;
  out = (char *)malloc((unsigned)base64_encoded_len(len) + 1);
  if (!out)
    return js_throw_range(ctx, "out of memory");
  base64_encode(p ? p : (const unsigned char *)"", p ? len : 0, out);
  r = js_mk_cstring(ctx, out);
  free(out);
  return r;
}

/* N.binaryString(bytes): one character per byte, U+0000 to U+00FF */
static js_value wt_n_binary_string(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int len = 0, i;
  const unsigned char *p = view_bytes(A(0), &len);
  sbuf sb = {0, 0, 0};
  js_value r;
  (void)t;
  for (i = 0; p && i < len; i++) {
    char tmp[4];
    sb_putn(&sb, tmp, utf8_encode(p[i], tmp));
  }
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}

/* N.createObjectURL(bytes, type, origin): a blob: URL for the page-wide blob store, which fetch() can read */
static js_value wt_n_create_object_url(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  static const char hx[] = "0123456789abcdef";
  js_value type = js_to_string(ctx, A(1)), origin;
  unsigned char b[16];
  char url[URL_MAX], id[37];
  const unsigned char *p;
  int len = 0, i, k = 0, n = 0;
  (void)t;
  p = view_bytes(A(0), &len);
  if (!p || type.type != JV_STR)
    return js_throw_type(ctx, "URL.createObjectURL: argument must be a Blob");
  gc_push_scope();
  gc_keep(type.u.str);
  origin = js_to_string(ctx, A(2));
  rng_bytes(b, 16);
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  for (i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      id[k++] = '-';
    id[k++] = hx[b[i] >> 4];
    id[k++] = hx[b[i] & 15];
  }
  id[k] = 0;
  sapp(url, &n, sizeof url, "blob:");
  if (origin.type == JV_STR && !streq(origin.u.str->data, "null"))
    sapp(url, &n, sizeof url, origin.u.str->data);
  sapp(url, &n, sizeof url, "/");
  sapp(url, &n, sizeof url, id);
  gc_pop_scope();
  if (!blobstore_put(url, p, len, type.u.str->data))
    return js_throw_type(ctx, "URL.createObjectURL: out of memory");
  return js_mk_cstring(ctx, url);
}

static js_value wt_n_revoke_object_url(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value url = js_to_string(ctx, A(0));
  (void)t;
  if (url.type == JV_STR)
    blobstore_del(url.u.str->data);
  return js_undef();
}

/* N.request(payload): hand a request ({op, id, ...}) to the page thread; its answer comes back as a reply */
static js_value wt_n_request(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_clone *c = js_clone_write(ctx, A(0), js_undef());
  (void)t;
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_REQUEST, 0, c, 0);
  return js_undef();
}

/* The first queued reply to request `id`, or NULL; *seq as for wq_take_one */
static struct wmsg *wq_take_reply(struct wthread *wt, int id, unsigned *seq)
{
  struct wmsg *m, *prev = 0;
  mutex_lock(&wt->lock);
  for (m = wt->in.head; m; prev = m, m = m->next)
    if (m->kind == WM_REPLY && m->level == id) {
      if (prev)
        prev->next = m->next;
      else
        wt->in.head = m->next;
      if (wt->in.tail == m)
        wt->in.tail = prev;
      break;
    }
  *seq = __atomic_load_n(&wt->seq, __ATOMIC_ACQUIRE);
  mutex_unlock(&wt->lock);
  return m;
}

/* N.waitReply(id): block for the answer to request `id` (synchronous XMLHttpRequest, permission checks) */
static js_value wt_n_wait_reply(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int id = (int)js_to_number(ctx, A(0));
  (void)t;
  for (;;) {
    unsigned seen;
    struct wmsg *m = wq_take_reply(cur_wt, id, &seen);
    if (m) {
      js_value data = js_clone_read(ctx, m->clone);
      wmsg_free(m);
      return data;
    }
    if (js_irq_stopped(ctx->irq))
      return js_undef();
    futex_wait(&cur_wt->seq, seen, 50);
  }
}

/* WebSockets live on the worker thread: the connect blocks it, as the page blocks on its own */
#define WS_MAX 16
static __thread ws_conn *wsconns[WS_MAX];
static __thread int nwsconns;

static js_value wt_n_ws_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value url = js_to_string(ctx, A(0)), protos = js_to_string(ctx, A(1));
  char u[URL_MAX], p[256];
  ws_conn *c;
  int i;
  (void)t;
  if (url.type != JV_STR || protos.type != JV_STR)
    return js_number(-1);
  snprintf(u, sizeof u, "%s", url.u.str->data);
  snprintf(p, sizeof p, "%s", protos.u.str->data);
  for (i = 0; i < WS_MAX && wsconns[i]; i++)
    ;
  if (i == WS_MAX || !(c = ws_open(u, p[0] ? p : 0)))
    return js_number(-1);
  wsconns[i] = c;
  nwsconns++;
  return js_number(i);
}

static ws_conn *ws_slot(js_ctx *ctx, js_value v)
{
  double d = js_to_number(ctx, v);
  return d >= 0 && d < WS_MAX ? wsconns[(int)d] : 0;
}

static js_value wt_n_ws_protocol(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_slot(ctx, A(0));
  (void)t;
  return js_mk_cstring(ctx, c ? ws_protocol(c) : "");
}

static js_value wt_n_ws_send(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_slot(ctx, A(0));
  int len = 0;
  const unsigned char *bytes;
  (void)t;
  if (!c || ws_state(c) != WS_OPEN)
    return js_bool(0);
  if (A(1).type == JV_STR)
    return js_bool(ws_send(c, A(1).u.str->data, A(1).u.str->len, 1) == 0);
  bytes = view_bytes(A(1), &len);
  return js_bool(bytes && ws_send(c, bytes, len, 0) == 0);
}

static js_value wt_n_ws_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_slot(ctx, A(0));
  js_value reason = js_to_string(ctx, A(2));
  (void)t;
  if (c && ws_state(c) != WS_CLOSED)
    ws_close(c, (int)js_to_number(ctx, A(1)), reason.type == JV_STR ? reason.u.str->data : "");
  return js_undef();
}

/* Call N.<name>(...) in the worker, reporting a throw */
static void wt_hook(js_ctx *ctx, const char *name, js_value *args, int argc)
{
  js_value n = js_get(ctx, js_obj_value(ctx->global), "@N"), fn = n.type == JV_OBJ ? js_get(ctx, n, name) : js_undef();
  if (!js_is_callable(fn))
    return;
  js_call(ctx, fn, n, args, argc);
  if (ctx->has_exc)
    wt_report(ctx);
  wt_run_microtasks(ctx);
}

/* Drain every open socket; true if anything was delivered */
static bool wt_pump_ws(js_ctx *ctx)
{
  bool fired = false;
  int i;
  for (i = 0; i < WS_MAX; i++) {
    ws_conn *c = wsconns[i];
    while (c) {
      const uint8_t *data = 0;
      int len = 0, is_text = 0, ev = ws_poll(c, &data, &len, &is_text);
      js_value args[3];
      if (ev == WS_EV_NONE)
        break;
      gc_push_scope();
      args[0] = js_number(i);
      args[1] = js_mk_cstring(ctx, ev == WS_EV_MESSAGE ? "message" : ev == WS_EV_CLOSE ? "close" : "error");
      gc_keep(args[1].u.str);
      args[2] = js_undef();
      if (ev == WS_EV_MESSAGE) {
        args[2] = is_text ? js_mk_string(ctx, (const char *)data, len) : js_new_arraybuffer(ctx, data, len);
        gc_keep(args[2].type == JV_STR ? (void *)args[2].u.str : (void *)args[2].u.obj);
      } else {
        ws_free(c);
        wsconns[i] = 0;
        nwsconns--;
        c = 0;
      }
      wt_hook(ctx, "onws", args, 3);
      gc_pop_scope();
      fired = true;
      if (ev != WS_EV_MESSAGE)
        break;
    }
  }
  return fired;
}

/* A message for a port that lives here arrives from the page */
/* A page connected to this shared worker: its port arrives, and onconnect hears about it */
static void wt_dispatch_connect(js_ctx *ctx, struct wmsg *m)
{
  js_value ports;
  gc_push_scope();
  ports_clear(ctx);
  js_clone_read(ctx, m->clone);
  if (ctx->has_exc)
    wt_report(ctx);
  else {
    ports = ports_take(ctx);
    gc_keep(ports.u.obj);
    wt_hook(ctx, "onconnect", &ports, 1);
  }
  gc_pop_scope();
}

/* A BroadcastChannel message from the page */
static void wt_dispatch_bc(js_ctx *ctx, struct wmsg *m)
{
  js_value args[2];
  gc_push_scope();
  args[0] = js_mk_cstring(ctx, m->text);
  gc_keep(args[0].u.str);
  args[1] = js_clone_read(ctx, m->clone);
  if (args[1].type == JV_OBJ)
    gc_keep(args[1].u.obj);
  if (ctx->has_exc)
    wt_report(ctx);
  else
    wt_hook(ctx, "onbc", args, 2);
  gc_pop_scope();
}

/* A message or an error from a worker this one started */
static void wt_dispatch_child(js_ctx *ctx, struct wmsg *m)
{
  js_value args[4];
  gc_push_scope();
  args[0] = js_number(m->level);
  args[3] = js_undef();
  if (m->kind == WM_CHILD_ERROR) {
    args[1] = js_mk_cstring(ctx, "error");
    args[2] = js_mk_cstring(ctx, m->text ? m->text : "");
    gc_keep(args[2].u.str);
  } else {
    args[1] = js_mk_cstring(ctx, "message");
    ports_clear(ctx);
    args[2] = js_clone_read(ctx, m->clone);
    if (args[2].type == JV_OBJ)
      gc_keep(args[2].u.obj);
    if (!ctx->has_exc) {
      args[3] = ports_take(ctx);
      gc_keep(args[3].u.obj);
    }
  }
  gc_keep(args[1].u.str);
  if (ctx->has_exc)
    wt_report(ctx);
  else
    wt_hook(ctx, "onchild", args, 4);
  gc_pop_scope();
}

static void wt_dispatch_port(js_ctx *ctx, int rid, js_clone *c)
{
  js_value args[3];
  gc_push_scope();
  args[0] = js_number(rid);
  ports_clear(ctx);
  args[1] = js_clone_read(ctx, c);
  if (args[1].type == JV_OBJ)
    gc_keep(args[1].u.obj);
  if (ctx->has_exc)
    wt_report(ctx);
  else {
    args[2] = ports_take(ctx);
    gc_keep(args[2].u.obj);
    wt_hook(ctx, "onport", args, 3);
  }
  gc_pop_scope();
}

/* A page reply arrives: N.onreply(data) settles whatever was waiting for it */
static void wt_dispatch_reply(js_ctx *ctx, js_clone *c)
{
  js_value data = js_clone_read(ctx, c);
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  if (ctx->has_exc)
    wt_report(ctx);
  else
    wt_hook(ctx, "onreply", &data, 1);
  gc_pop_scope();
}

/* N.streams(): the Streams classes, compiled on first use (see the lazy globals in workerscope_prelude.inc) */
static js_value wt_n_streams(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_arena *ar = 0;
  js_value exports = js_streams_run(ctx, &ar);
  (void)t;
  (void)args;
  (void)argc;
  if (ar)
    wt_keep_arena(cur_wt, ar);
  if (exports.type != JV_OBJ && !ctx->has_exc)
    return js_throw_type(ctx, "Streams are unavailable");
  return exports;
}

static js_value wt_n_new_port_id(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number((double)__atomic_add_fetch(&g_next_port_id, 1, __ATOMIC_RELAXED));
}

/* N.portPost(id, data, transfer): a message for the far end of a port that lives on the page */
static js_value wt_n_port_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_clone *c = js_clone_write(ctx, A(1), A(2));
  (void)t;
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_PORT, (int)js_to_number(ctx, A(0)), c, 0);
  return js_undef();
}

/* N.bcPost(name, data): to the page, which hands it to the other channels of that name */
static js_value wt_n_bc_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_clone *c = js_clone_write(ctx, A(1), js_undef());
  (void)t;
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_BCAST, 0, c, arg_str(ctx, A(0)));
  return js_undef();
}

/* N.spawn(url, name, module) -> id: asks the page to start a worker for this one; the page reports trouble as an
 * error message to the parent */
static js_value wt_n_spawn(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], origin[URL_MAX];
  js_object *req;
  js_clone *c;
  int id;
  (void)t;
  script_url(cur_wt->url, arg_str(ctx, A(0)), url, sizeof url);
  if (!starts_ci(url, "blob:") && !starts_ci(url, "data:")) {
    cors_origin(url, origin, sizeof origin);
    if (!streq(origin, cur_wt->origin))
      return js_throw_error(ctx, ctx->error_proto, "SecurityError", "Worker script must be same-origin");
  }
  id = __atomic_add_fetch(&g_next_wid, 1, __ATOMIC_RELAXED);
  gc_push_scope();
  req = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(req, "op", js_mk_cstring(ctx, "spawn"), P_ENUMERABLE);
  js_def(req, "id", js_number(id), P_ENUMERABLE);
  js_def(req, "url", js_mk_cstring(ctx, url), P_ENUMERABLE);
  js_def(req, "name", A(1).type == JV_STR ? A(1) : js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(req, "module", js_bool(js_to_boolean(A(2))), P_ENUMERABLE);
  c = js_clone_write(ctx, js_obj_value(req), js_undef());
  gc_pop_scope();
  if (!c)
    return js_undef();
  wmsg_post(cur_wt, &cur_wt->out, WM_REQUEST, 0, c, 0);
  return js_number(id);
}

/* N.childPost(id, data, transfer): the page hands the message on to the worker with that id */
static js_value wt_n_child_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_clone *c = js_clone_write(ctx, A(1), A(2));
  (void)t;
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_CHILD_POST, (int)js_to_number(ctx, A(0)), c, 0);
  return js_undef();
}

static js_value wt_n_child_terminate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  wmsg_post(cur_wt, &cur_wt->out, WM_CHILD_TERM, (int)js_to_number(ctx, A(0)), 0, 0);
  return js_undef();
}

/* N.cloneLocal(value, transfer) -> {data, ports}: a structured clone inside this worker, ports moving by identity */
static js_value wt_n_clone_local(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value data = js_undef(), ports = js_undef();
  js_object *r;
  (void)t;
  if (!js_local_clone(ctx, A(0), A(1), &data, &ports))
    return js_undef();
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  gc_keep(ports.u.obj);
  r = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(r, "data", data, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  js_def(r, "ports", ports, P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return js_obj_value(r);
}

static js_value wt_n_port_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  wmsg_post(cur_wt, &cur_wt->out, WM_PORT_CLOSE, (int)js_to_number(ctx, A(0)), 0, 0);
  return js_undef();
}

/* Run parts up to and including `upto`; false (exception pending) if one fails */
/* ---- the audio worklet (audioworklet.c) ---- */

void js_worklet_post(js_ctx *ctx, js_value v)
{
  js_clone *c = js_clone_write(ctx, v, js_undef());
  if (c)
    wmsg_post(cur_wt, &cur_wt->out, WM_DATA, 0, c, 0);
}

/* Run a module script in the worklet scope: 0, or -1 with the exception left pending */
int js_worklet_add_module(js_ctx *ctx, const char *spec)
{
  js_object *exports;
  char *src = 0, url[URL_MAX];
  bool ok;
  if (wt_fetch(cur_wt, cur_wt->url, spec, &src)) {
    free(src);
    throw_err(ctx, "AbortError", "Unable to load the worklet module");
    return -1;
  }
  snprintf(url, sizeof url, "%s", cur_wt->rpc_url);
  gc_push_scope();
  exports = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  ok = wt_eval(ctx, cur_wt, src, url, exports, NULL);
  gc_pop_scope();
  free(src);
  return ok ? 0 : -1;
}

/* One quantum of the graph; true if one was rendered */
static bool worklet_render(js_ctx *ctx)
{
  bool rendered = js_worklet_render(ctx);
  if (ctx->has_exc)
    wt_report(ctx);
  wt_run_microtasks(ctx);
  return rendered;
}

static bool wt_load_parts(js_ctx *ctx, unsigned upto)
{
  js_value n = js_get(ctx, js_obj_value(ctx->global), "@N"), l = js_get(ctx, js_obj_value(ctx->global), "@L");
  bool ok = true;
  while (ok && parts_loaded <= upto && parts_loaded < sizeof k_parts / sizeof *k_parts) {
    js_value fn = js_undef(), args[3];
    unsigned i = parts_loaded++;
    ok = wt_eval(ctx, cur_wt, k_parts[i], cur_wt->url, 0, &fn) && js_is_callable(fn);
    if (ok) {
      args[0] = js_obj_value(ctx->global);
      args[1] = n;
      args[2] = l;
      js_call(ctx, fn, js_undef(), args, 3);
      ok = !ctx->has_exc;
    }
  }
  return ok;
}

/* N.cacheMode({cache, mode}): the validated cache mode name for a Request, or a TypeError */
static js_value wt_n_cache_mode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *err;
  int mode = fetch_cache_mode(ctx, js_undef(), A(0), &err);
  (void)t;
  return mode < 0 ? js_throw_type(ctx, err) : js_mk_cstring(ctx, fetch_cache_names[mode]);
}

/* N.urlClasses(): define URL and URLSearchParams (the source the page runs too) and return them */
static js_value wt_n_url_classes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value fn = js_undef(), a[2];
  (void)t;
  (void)args;
  (void)argc;
  if (!wt_eval(ctx, cur_wt, url_classes_js, "url", 0, &fn) || !js_is_callable(fn))
    return js_undef();
  a[0] = js_obj_value(ctx->global);
  a[1] = js_get(ctx, a[0], "@N");
  return js_call(ctx, fn, js_undef(), a, 2);
}

/* N.headers(): define Headers (the source the page runs too) and return it */
static js_value wt_n_headers(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value fn = js_undef(), g = js_obj_value(ctx->global);
  (void)t;
  (void)args;
  (void)argc;
  if (!wt_eval(ctx, cur_wt, fetch_headers_js, "headers", 0, &fn) || !js_is_callable(fn))
    return js_undef();
  return js_call(ctx, fn, js_undef(), &g, 1);
}

static js_value wt_n_load_part(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  wt_load_parts(ctx, (unsigned)js_to_int_sat(ctx, A(0)));
  return js_undef();
}

/* Set up N and L, then run the core part; false (exception pending) if it fails */
static bool wt_run_parts(js_ctx *ctx, struct wthread *wt)
{
  js_object *n, *l;
  bool ok;
  (void)wt;
  parts_loaded = 0;
  gc_push_scope();
  n = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  l = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ctx->global, "@L", js_obj_value(l), 0);
  js_def_native(ctx, n, "loadPart", wt_n_load_part, 1);
  js_def_native(ctx, n, "cacheMode", wt_n_cache_mode, 1);
  js_def_native(ctx, n, "headers", wt_n_headers, 0);
  js_def_native(ctx, n, "utf8encode", wt_n_utf8encode, 1);
  js_def_native(ctx, n, "encodingName", wt_n_encoding_name, 1);
  js_def_native(ctx, n, "encodeInto", wt_n_encode_into, 2);
  js_def_native(ctx, n, "decode", wt_n_decode, 4);
  js_def_native(ctx, n, "randomFill", wt_n_random_fill, 1);
  url_natives_install(ctx, n);
  js_def_native(ctx, n, "urlClasses", wt_n_url_classes, 0);
  js_def_native(ctx, n, "base64", wt_n_base64, 1);
  js_def_native(ctx, n, "binaryString", wt_n_binary_string, 1);
  js_def_native(ctx, n, "request", wt_n_request, 1);
  js_def_native(ctx, n, "streams", wt_n_streams, 0);
  js_def_native(ctx, n, "newPortId", wt_n_new_port_id, 0);
  js_def_native(ctx, n, "portPost", wt_n_port_post, 3);
  js_def_native(ctx, n, "portClose", wt_n_port_close, 1);
  js_def_native(ctx, n, "cloneLocal", wt_n_clone_local, 2);
  sw_cache_worker_setup(ctx, n, wt->origin);
  js_def_native(ctx, n, "bcPost", wt_n_bc_post, 2);
  js_def_native(ctx, n, "spawn", wt_n_spawn, 3);
  js_def_native(ctx, n, "childPost", wt_n_child_post, 3);
  js_def_native(ctx, n, "childTerminate", wt_n_child_terminate, 1);
  js_def_native(ctx, n, "waitReply", wt_n_wait_reply, 1);
  js_def_native(ctx, n, "wsOpen", wt_n_ws_open, 2);
  js_def_native(ctx, n, "wsProtocol", wt_n_ws_protocol, 1);
  js_def_native(ctx, n, "wsSend", wt_n_ws_send, 2);
  js_def_native(ctx, n, "wsClose", wt_n_ws_close, 3);
  js_def(ctx->global, "@N", js_obj_value(n), 0);
  js_def_native(ctx, n, "createObjectURL", wt_n_create_object_url, 3);
  js_def_native(ctx, n, "revokeObjectURL", wt_n_revoke_object_url, 1);
  ok = wt_load_parts(ctx, 0);
  gc_pop_scope();
  return ok;
}

static void wt_setup(js_ctx *ctx, struct wthread *wt)
{
  js_object *g = ctx->global, *perf, *store;
  gc_push_scope();
  js_def_native(ctx, g, "postMessage", wt_post, 1);
  js_def_native(ctx, g, "close", wt_close, 0);
  js_def_native(ctx, g, "importScripts", wt_import_scripts, 1);
  js_def_native(ctx, g, "setTimeout", wt_set_timeout, 2);
  js_def_native(ctx, g, "setInterval", wt_set_interval, 2);
  js_def_native(ctx, g, "clearTimeout", wt_clear_timer, 1);
  js_def_native(ctx, g, "clearInterval", wt_clear_timer, 1);
  js_def_native(ctx, g, "queueMicrotask", wt_queue_microtask, 1);
  js_def(g, "@micro", js_obj_value(js_new_array(ctx, 0)), 0);
  perf = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, perf, "now", wt_perf_now, 0);
  js_def(g, "performance", js_obj_value(perf), P_WRITABLE | P_CONFIGURABLE);
  store = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(g, "@timers", js_obj_value(store), 0);
  store = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(g, "@modules", js_obj_value(store), 0);
  wt_def_location(ctx, wt);
  ctx->load_module = wt_load_module;
  ctx->load_module_ud = wt;
  gc_pop_scope();
}

/* Call one handler of the global with the event, reporting a throw */
static void wt_call_handler(js_ctx *ctx, js_value fn, js_value ev)
{
  js_value g = js_obj_value(ctx->global);
  if (js_is_callable(fn))
    js_call(ctx, fn, g, &ev, 1);
  else if (fn.type == JV_OBJ) {
    js_value handle = js_get(ctx, fn, "handleEvent");
    if (js_is_callable(handle))
      js_call(ctx, handle, fn, &ev, 1);
  }
  if (ctx->has_exc)
    wt_report(ctx);
}

/* A MessageEvent for `data`, built here instead of by the script-side constructor, then handed to onmessage and to
 * the listeners the prelude recorded under "@listeners" (the same order dispatchEvent would use) */
static void wt_fire_message(js_ctx *ctx, js_value data, js_value ports)
{
  js_value g = js_obj_value(ctx->global), ctor = js_get(ctx, g, "MessageEvent"), proto, ls, list;
  js_object *ev;
  unsigned n, i;
  const unsigned char flags = P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE;
  proto = ctor.type == JV_OBJ ? js_get(ctx, ctor, "prototype") : js_undef();
  if (proto.type != JV_OBJ)
    return;
  gc_push_scope();
  ev = (js_object *)gc_keep(js_new_object(ctx, proto.u.obj));
  js_def(ev, "type", js_mk_cstring(ctx, "message"), flags);
  js_def(ev, "bubbles", js_bool(0), flags);
  js_def(ev, "cancelable", js_bool(0), flags);
  js_def(ev, "composed", js_bool(0), flags);
  js_def(ev, "defaultPrevented", js_bool(0), flags);
  js_def(ev, "target", g, flags);
  js_def(ev, "currentTarget", g, flags);
  js_def(ev, "eventPhase", js_number(0), flags);
  js_def(ev, "isTrusted", js_bool(0), flags);
  js_def(ev, "timeStamp", wt_perf_now(ctx, js_undef(), 0, 0), flags);
  js_def(ev, "data", data, flags);
  js_def(ev, "origin", js_mk_cstring(ctx, ""), flags);
  js_def(ev, "lastEventId", js_mk_cstring(ctx, ""), flags);
  js_def(ev, "source", js_null(), flags);
  js_def(ev, "ports", ports, flags);
  js_def(ev, "@stop", js_bool(0), P_WRITABLE | P_CONFIGURABLE);
  wt_call_handler(ctx, js_get(ctx, g, "onmessage"), js_obj_value(ev));
  ls = js_get(ctx, g, "@listeners");
  list = ls.type == JV_OBJ ? js_get(ctx, ls, "message") : js_undef();
  if (list.type == JV_OBJ) {
    js_object *snapshot = (js_object *)gc_keep(js_new_array(ctx, 0));
    n = js_array_len(list.u.obj);
    for (i = 0; i < n; i++) /* listeners added during dispatch wait for the next event */
      js_array_push(ctx, snapshot, js_get_index(ctx, list, i));
    for (i = 0; i < n && !js_to_boolean(js_get(ctx, js_obj_value(ev), "@stop")); i++) {
      js_value rec = js_get_index(ctx, js_obj_value(snapshot), i), fn = js_get(ctx, rec, "fn");
      if (js_to_boolean(js_get(ctx, rec, "once"))) {
        js_value rm[2], remove = js_get(ctx, g, "removeEventListener");
        rm[0] = js_mk_cstring(ctx, "message");
        rm[1] = fn;
        js_call(ctx, remove, g, rm, 2);
      }
      wt_call_handler(ctx, fn, js_obj_value(ev));
    }
  }
  js_set(ctx, js_obj_value(ev), "currentTarget", js_null());
  gc_pop_scope();
}

static void wt_deliver(js_ctx *ctx, js_clone *c)
{
  js_value data;
  ports_clear(ctx);
  data = js_clone_read(ctx, c);
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  if (ctx->has_exc)
    wt_report(ctx);
  else {
    js_value ports = ports_take(ctx);
    gc_keep(ports.u.obj);
    wt_fire_message(ctx, data, ports);
    wt_run_microtasks(ctx);
  }
  gc_pop_scope();
}

static void wt_loop(js_ctx *ctx, struct wthread *wt)
{
  while (!__atomic_load_n(&wt->irq.stop, __ATOMIC_ACQUIRE) && !__atomic_load_n(&wt->closing, __ATOMIC_ACQUIRE)) {
    unsigned seen, wait = 0;
    struct wmsg *m = wq_take_one(wt, &wt->in, &seen);
    int i;
    if (m) {
      if (m->kind == WM_REPLY)
        wt_dispatch_reply(ctx, m->clone);
      else if (m->kind == WM_PORT)
        wt_dispatch_port(ctx, m->level, m->clone);
      else if (m->kind == WM_PORT_CLOSE) {
        js_value id = js_number(m->level);
        wt_hook(ctx, "onportclose", &id, 1);
      } else if (m->kind == WM_CONNECT) {
        wt_dispatch_connect(ctx, m);
      } else if (m->kind == WM_BCAST) {
        if (m->clone && m->text && parts_loaded > PART_NET)
          wt_dispatch_bc(ctx, m);
      } else if (m->kind == WM_CHILD_DATA || m->kind == WM_CHILD_ERROR)
        wt_dispatch_child(ctx, m);
      else if (m->clone)
        wt_deliver(ctx, m->clone);
      wmsg_free(m);
      continue;
    }
    if (nwsconns && wt_pump_ws(ctx))
      continue;
    if (fire_due_timer(ctx))
      continue;
    if (wt->worklet && worklet_render(ctx))
      continue;
    if (ntimers) { /* sleep until the nearest timer; futex time 0 means forever */
      unsigned now = msec();
      int best = (int)(timers[0].due - now);
      for (i = 1; i < ntimers; i++)
        if ((int)(timers[i].due - now) < best)
          best = (int)(timers[i].due - now);
      wait = best < 1 ? 1 : (unsigned)best;
    }
    if (nwsconns && (wait == 0 || wait > 10))
      wait = 10; /* sockets have no wake-up of their own: look again soon */
    futex_wait(&wt->seq, seen, wait);
  }
}

static void wt_run(js_ctx *ctx, struct wthread *wt)
{
  js_object *exports = 0;
  ctx->now_ms = now_ms;
  ctx->can_block = 1;
  ctx->irq = &wt->irq;
  ctx->clone_write_ext = wt_clone_write_any;
  ctx->clone_read_ext = wt_clone_read;
  js_set_stack_limit(ctx, wt->stack_lo + WORKER_HEADROOM);
  wt_setup(ctx, wt);
  canvas_js_setup_worker(ctx, ctx->global);
  if (!wt_run_parts(ctx, wt)) {
    wt_report(ctx);
    return;
  }
  if (wt->worklet) {
    if (wt_load_parts(ctx, PART_NET)) {
      js_worklet_install(ctx);
      wt_loop(ctx, wt);
    } else
      wt_report(ctx);
    return;
  }
  if (!wt->src) {
    wmsg_post(wt, &wt->out, WM_ERROR, 0, 0, "Failed to load worker script");
    return;
  }
  gc_push_scope();
  if (wt->module)
    exports = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (!wt_eval(ctx, wt, wt->src, wt->url, exports, NULL))
    wt_report(ctx);
  wt_run_microtasks(ctx);
  gc_pop_scope();
  free(wt->src);
  wt->src = 0;
  wt_loop(ctx, wt);
}

/* An allocation this worker cannot survive lands here and unwinds to worker_main, which frees the heap */
static __thread jmp_buf oom_env;

static void wt_oom(void *unused)
{
  (void)unused;
  longjmp(oom_env, 1);
}

static void worker_main(void *arg)
{
  struct wthread *wt = (struct wthread *)arg;
  gc_heap *heap = gc_heap_new();
  js_ctx *volatile ctx = 0;
  int i;
  cur_wt = wt;
  if (heap) {
    gc_heap_attach(heap);
    gc_heap_on_oom(heap, wt_oom, 0);
    if (setjmp(oom_env) == 0) {
      ctx = js_ctx_new(wt_print);
      if (ctx) {
        wt_run(ctx, wt);
        js_ctx_free(ctx);
      }
    } else {
      /* memory is gone: report, then drop the heap without running more JS or collecting */
      wmsg_post(wt, &wt->out, WM_ERROR, 0, 0, "Worker ran out of memory");
      if (ctx) {
        js_regex_cache_free(ctx);
        free(ctx);
      }
    }
    canvas_page_end(ctx);
    gc_heap_detach();
    gc_heap_free(heap);
  }
  for (i = 0; i < wt->narenas; i++) {
    js_arena_free(wt->arenas[i]);
    free(wt->arenas[i]);
  }
  for (i = 0; i < WS_MAX; i++)
    ws_free(wsconns[i]);
  if (wt->worklet)
    js_worklet_end();
  free(wt->arenas);
  wt->arenas = 0;
  free(wt->src);
  wt->src = 0;
  free(timers);
  timers = 0;
  ntimers = captimers = 0;
  __atomic_store_n(&wt->done, 1, __ATOMIC_RELEASE);
  thread_exit();
}

/* ------------------------------------------------------- page side ----------- */

/* The script's bytes as a NUL-terminated string, or NULL */
static char *fetch_source(const char *url)
{
  int len = 0;
  char *src;
  unsigned char *body = fetch_get(url, &len);
  if (!body)
    return 0;
  src = (char *)malloc((unsigned)len + 1);
  if (src) {
    memcpy(src, body, (unsigned)len);
    src[len] = 0;
  }
  free(body);
  return src;
}

/* Answer a worker's script request on its behalf */
static void serve_rpc(struct wthread *wt)
{
  char *src = 0;
  const char *err = 0;
  script_url(wt->rpc_base, wt->rpc_spec, wt->rpc_url, sizeof wt->rpc_url);
  if (!starts_ci(wt->rpc_url, "blob:") && !starts_ci(wt->rpc_url, "data:") &&
      !cors_same_origin(g_page_url_buf, wt->rpc_url))
    err = "cross-origin";
  else if (!(src = fetch_source(wt->rpc_url)))
    err = "fetch failed";
  wt->rpc_src = src;
  wt->rpc_err = err;
  __atomic_store_n(&wt->rpc, 2, __ATOMIC_RELEASE);
  futex_wake(&wt->rpc, 1);
}

static js_value reply_error(js_ctx *ctx, int id, const char *message)
{
  js_object *o = js_new_object(ctx, ctx->object_proto);
  js_value ov = js_obj_value(o);
  gc_push_scope();
  gc_keep(o);
  js_def(o, "id", js_number(id), P_ENUMERABLE);
  js_def(o, "ok", js_bool(0), P_ENUMERABLE);
  js_def(o, "error", js_mk_cstring(ctx, message), P_ENUMERABLE);
  gc_pop_scope();
  return ov;
}

/* A worker's fetch(): {url, method, headers:[[k,v]], body:ArrayBuffer|null, credentials, cache} -> the response */
static js_value request_fetch(js_ctx *ctx, int id, js_value req)
{
  struct fetch_req rq = {.method = "GET"};
  struct fetch_res *rs;
  js_value v, headers, body;
  js_object *reply;
  const char *cache_err;
  unsigned i, n;
  int rc;
  v = js_get(ctx, req, "url");
  snprintf(rq.url, sizeof rq.url, "%s", v.type == JV_STR ? v.u.str->data : "");
  if (!rq.url[0])
    return reply_error(ctx, id, "Failed to parse URL");
  v = js_get(ctx, req, "method");
  if (v.type == JV_STR)
    snprintf(rq.method, sizeof rq.method, "%s", v.u.str->data);
  v = js_get(ctx, req, "credentials");
  rq.with_creds = v.type == JV_STR && streq(v.u.str->data, "include");
  rq.cache_mode = fetch_cache_mode(ctx, js_undef(), req, &cache_err);
  if (rq.cache_mode < 0)
    return reply_error(ctx, id, cache_err);
  if (!fetch_csp(rq.url, sizeof rq.url, "connection to"))
    return reply_error(ctx, id, "Refused to connect (Content-Security-Policy)");
  headers = js_get(ctx, req, "headers");
  n = headers.type == JV_OBJ ? js_array_len(headers.u.obj) : 0;
  for (i = 0; i < n; i++) {
    js_value pair = js_get_index(ctx, headers, i), k = js_to_string(ctx, js_get_index(ctx, pair, 0)),
             val = js_to_string(ctx, js_get_index(ctx, pair, 1));
    if (k.type != JV_STR || val.type != JV_STR)
      continue;
    if (fetch_req_header(&rq, k.u.str->data, val.u.str->data) < 0) {
      fetch_req_free(&rq);
      return reply_error(ctx, id, "Out of memory");
    }
  }
  body = js_get(ctx, req, "body");
  if (body.type == JV_OBJ)
    rq.body = view_bytes(body, &rq.body_len);
  rs = malloc(sizeof *rs);
  if (!rs) {
    fetch_req_free(&rq);
    return reply_error(ctx, id, "Out of memory");
  }
  rc = fetch_run(&rq, rs);
  fetch_req_free(&rq);
  if (rc != FETCH_OK) {
    free(rs);
    return reply_error(ctx, id, rc == FETCH_ABORTED ? "The user aborted a request." : "Failed to fetch");
  }
  gc_push_scope();
  reply = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(reply, "id", js_number(id), P_ENUMERABLE);
  js_def(reply, "ok", js_bool(1), P_ENUMERABLE);
  js_def(reply, "status", js_number(rs->status), P_ENUMERABLE);
  js_def(reply, "statusText", js_mk_cstring(ctx, rs->status_text), P_ENUMERABLE);
  js_def(reply, "url", js_mk_cstring(ctx, rs->final_url[0] ? rs->final_url : rq.url), P_ENUMERABLE);
  js_def(reply, "headers", parse_header_block(ctx, rs->hdr), P_ENUMERABLE);
  js_def(reply, "body", js_new_arraybuffer(ctx, rs->body, rs->len), P_ENUMERABLE);
  free(rs->body);
  free(rs);
  v = js_obj_value(reply);
  gc_pop_scope();
  return v;
}

/* Answer one WM_REQUEST from a worker on the page thread */
static struct wthread *worker_create(js_ctx *ctx, int id, const char *url, const char *name, int module,
                                     struct wthread *parent, js_object **out, const char **ename, const char **emsg);

static void serve_request(struct wthread *wt, struct wmsg *m)
{
  js_value req = js_clone_read(g_ctx, m->clone), reply = js_undef(), op;
  js_clone *c;
  int id = 0;
  gc_push_scope();
  if (g_ctx->has_exc) {
    g_ctx->has_exc = 0;
    g_ctx->exc_root = 0;
  } else if (req.type == JV_OBJ) {
    gc_keep(req.u.obj);
    id = js_to_int_sat(g_ctx, js_get(g_ctx, req, "id"));
    op = js_get(g_ctx, req, "op");
    if (op.type == JV_STR && streq(op.u.str->data, "fetch"))
      reply = request_fetch(g_ctx, id, req);
    else if (op.type == JV_STR && streq(op.u.str->data, "connect")) { /* may a socket be opened to this URL? */
      js_value u = js_get(g_ctx, req, "url");
      int ok = u.type != JV_STR || fetch_csp_ws(u.u.str->data);
      if (ok) {
        js_object *o = js_new_object(g_ctx, g_ctx->object_proto);
        gc_keep(o);
        js_def(o, "id", js_number(id), P_ENUMERABLE);
        js_def(o, "ok", js_bool(1), P_ENUMERABLE);
        reply = js_obj_value(o);
      } else
        reply = reply_error(g_ctx, id, "Refused to connect (Content-Security-Policy)");
    } else if (op.type == JV_STR &&
               streq(op.u.str->data, "spawn")) { /* a worker starting a worker: no reply, errors go to its parent */
      js_value u = js_get(g_ctx, req, "url"), n = js_get(g_ctx, req, "name");
      js_object *child;
      const char *ename, *emsg;
      if (u.type != JV_STR || !worker_create(g_ctx, id, u.u.str->data, n.type == JV_STR ? n.u.str->data : "",
                                             js_to_boolean(js_get(g_ctx, req, "module")), wt, &child, &ename, &emsg))
        wmsg_post(wt, &wt->in, WM_CHILD_ERROR, id, 0, u.type == JV_STR ? emsg : "Bad worker URL");
    } else
      reply = reply_error(g_ctx, id, "Unknown request");
  }
  if (reply.type == JV_OBJ) {
    js_value body = js_get(g_ctx, reply, "body"), list = js_undef();
    gc_keep(reply.u.obj);
    if (body.type == JV_OBJ) { /* move the response bytes instead of copying them */
      list = js_obj_value(js_new_array(g_ctx, 0));
      gc_keep(list.u.obj);
      js_array_push(g_ctx, list.u.obj, body);
    }
    c = js_clone_write(g_ctx, reply, list);
    if (c)
      wmsg_post(wt, &wt->in, WM_REPLY, id, c, 0);
    else if (g_ctx->has_exc) {
      g_ctx->has_exc = 0;
      g_ctx->exc_root = 0;
    }
  }
  gc_pop_scope();
}

static struct wthread *worker_of(js_ctx *ctx, js_value w)
{
  js_value id = w.type == JV_OBJ ? js_get(ctx, w, "@id") : js_undef();
  int i;
  if (id.type != JV_NUM)
    return 0;
  for (i = 0; i < g_nworkers; i++)
    if (g_workers[i]->id == (int)id.u.num)
      return g_workers[i];
  return 0;
}

/* A BroadcastChannel message to every channel of that name except the sender's own: the page's channels (each gets
 * its own clone, delivered later like any message) and every worker's. `sender` is the posting page channel and
 * `from` the posting worker, if any. */
void js_bc_dispatch(const char *name, js_clone *c, js_object *sender, struct wthread *from)
{
  int i;
  unsigned k, n = g_bchannels ? js_array_len(g_bchannels) : 0;
  for (k = 0; k < n; k++) {
    js_value cv = js_get_index(g_ctx, js_obj_value(g_bchannels), k), nm, data;
    if (cv.type != JV_OBJ || cv.u.obj == sender || js_to_boolean(js_get(g_ctx, cv, "@closed")))
      continue;
    nm = js_get(g_ctx, cv, "name");
    if (nm.type != JV_STR || !streq(nm.u.str->data, name))
      continue;
    gc_push_scope();
    gc_keep(cv.u.obj);
    ports_clear(g_ctx);
    data = js_clone_read(g_ctx, c);
    if (g_ctx->has_exc) {
      g_ctx->has_exc = 0;
      g_ctx->exc_root = 0;
    } else {
      if (data.type == JV_OBJ)
        gc_keep(data.u.obj);
      js_port_enqueue(cv, data, ports_take(g_ctx));
    }
    gc_pop_scope();
  }
  for (i = 0; i < g_nworkers; i++)
    if (g_workers[i] != from && !__atomic_load_n(&g_workers[i]->irq.stop, __ATOMIC_ACQUIRE))
      wmsg_post(g_workers[i], &g_workers[i]->in, WM_BCAST, 0, js_clone_dup(c), name);
}

/* The worker `parent` started with this id, or NULL */
static struct wthread *child_of(struct wthread *parent, int id)
{
  int i;
  for (i = 0; i < g_nworkers; i++)
    if (g_workers[i]->parent == parent && g_workers[i]->id == id)
      return g_workers[i];
  return 0;
}

static int rport_find(int rid)
{
  int i;
  for (i = 0; i < g_nrports; i++)
    if (g_rport_ids[i] == rid)
      return i;
  return -1;
}

static void rport_drop(int i)
{
  g_nrports--;
  g_rport_objs[i] = g_rport_objs[g_nrports];
  g_rport_ids[i] = g_rport_ids[g_nrports];
  g_rport_wt[i] = g_rport_wt[g_nrports];
  g_rport_objs[g_nrports] = 0;
}

static void rport_drop_worker(struct wthread *wt)
{
  int i;
  for (i = 0; i < g_nrports;)
    if (g_rport_wt[i] == wt)
      rport_drop(i);
    else
      i++;
}

/* The page end of a channel is being posted to a worker: its peer becomes the local stand-in for the far end */
int worker_port_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len)
{
  js_value peer;
  int rid;
  if (g_local_clone && js_is_port(v))
    return ports_local_write(ctx, v, kind, bytes, len);
  if (!clone_peer || !js_is_port(v) || g_nrports == RPORT_MAX)
    return 0;
  peer = js_get(ctx, v, "@peer");
  if (peer.type != JV_OBJ || js_get(ctx, v, "@remote").type == JV_NUM)
    return 0;
  *bytes = (unsigned char *)malloc(sizeof rid);
  if (!*bytes)
    return 0;
  rid = (int)__atomic_add_fetch(&g_next_port_id, 1, __ATOMIC_RELAXED);
  memcpy(*bytes, &rid, sizeof rid);
  *len = (int)sizeof rid;
  strcpy(kind, "port");
  js_set(ctx, peer, "@remote", js_number(rid));
  js_set(ctx, peer, "@peer", js_undef());
  js_set(ctx, v, "@closed", js_bool(1));
  g_rport_ids[g_nrports] = rid;
  g_rport_wt[g_nrports] = clone_peer;
  g_rport_objs[g_nrports++] = peer.u.obj;
  return 1;
}

/* A port arrives from a worker: a page port that forwards everything to the far end */
js_value worker_port_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len)
{
  js_object *port;
  int rid;
  if (streq(kind, "lport"))
    return ports_local_read(ctx, bytes, len);
  if (!clone_peer || !streq(kind, "port") || len != (int)sizeof rid || g_nrports == RPORT_MAX)
    return js_undef();
  memcpy(&rid, bytes, sizeof rid);
  port = js_new_port(ctx);
  gc_push_scope();
  gc_keep(port);
  js_def(port, "@remote", js_number(rid), P_WRITABLE);
  g_rport_ids[g_nrports] = rid;
  g_rport_wt[g_nrports] = clone_peer;
  g_rport_objs[g_nrports++] = port;
  ports_note(ctx, js_obj_value(port));
  gc_pop_scope();
  return js_obj_value(port);
}

void js_port_remote_post(js_ctx *ctx, js_value port, js_value data, js_value transfer)
{
  int i = rport_find((int)js_to_number(ctx, js_get(ctx, port, "@remote")));
  js_clone *c;
  if (i < 0)
    return;
  clone_peer = g_rport_wt[i];
  c = js_clone_write(ctx, data, transfer);
  clone_peer = 0;
  if (c)
    wmsg_post(g_rport_wt[i], &g_rport_wt[i]->in, WM_PORT, g_rport_ids[i], c, 0);
}

void js_port_remote_close(js_ctx *ctx, js_value port)
{
  int i = rport_find((int)js_to_number(ctx, js_get(ctx, port, "@remote")));
  if (i < 0)
    return;
  wmsg_post(g_rport_wt[i], &g_rport_wt[i]->in, WM_PORT_CLOSE, g_rport_ids[i], 0, 0);
  rport_drop(i);
}

static void worker_fire(struct wthread *wt, js_value w, struct wmsg *m)
{
  js_value ev, clients = js_undef();
  unsigned nc = 1, ci;
  gc_push_scope();
  gc_keep(w.u.obj);
  if (wt->shared) { /* a shared worker's trouble goes to every page object connected to it, and it has no one to talk to
                       otherwise */
    clients = js_get(g_ctx, w, "@clients");
    if (m->kind != WM_ERROR || clients.type != JV_OBJ) {
      gc_pop_scope();
      return;
    }
    gc_keep(clients.u.obj);
    nc = js_array_len(clients.u.obj);
  }
  if (m->kind == WM_ERROR) {
    ev = make_event("error", 0, 0, js_undef());
    if (ev.type == JV_OBJ) {
      gc_keep(ev.u.obj);
      event_adopt_iface(ev, "ErrorEvent");
      js_def(ev.u.obj, "message", js_mk_cstring(g_ctx, m->text ? m->text : ""), P_WRITABLE | P_ENUMERABLE);
      js_def(ev.u.obj, "filename", js_get(g_ctx, w, "@url"), P_WRITABLE | P_ENUMERABLE);
      js_def(ev.u.obj, "lineno", js_number(0), P_WRITABLE | P_ENUMERABLE);
      js_def(ev.u.obj, "colno", js_number(0), P_WRITABLE | P_ENUMERABLE);
      js_def(ev.u.obj, "error", js_null(), P_WRITABLE | P_ENUMERABLE);
    }
  } else {
    js_value data, ports;
    clone_peer = wt;
    ports_clear(g_ctx);
    data = js_clone_read(g_ctx, m->clone);
    clone_peer = 0;
    if (g_ctx->has_exc) {
      g_ctx->has_exc = 0;
      g_ctx->exc_root = 0;
      gc_pop_scope();
      return;
    }
    if (data.type == JV_OBJ)
      gc_keep(data.u.obj);
    else if (data.type == JV_STR)
      gc_keep(data.u.str);
    ports = ports_take(g_ctx);
    gc_keep(ports.u.obj);
    ev = make_message_event(data, ports);
  }
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    for (ci = 0; ci < nc; ci++) {
      js_value target = wt->shared ? js_get_index(g_ctx, clients, ci) : w;
      if (target.type != JV_OBJ)
        continue;
      js_set(g_ctx, ev, "target", target);
      ev_fire_on(target.u.obj, ev, 0);
    }
  }
  gc_pop_scope();
}

/* Free a finished worker once its thread has been joined */
static bool worker_reap(int i)
{
  struct wthread *wt = g_workers[i];
  int status;
  if (!__atomic_load_n(&wt->done, __ATOMIC_ACQUIRE) || !waitpid_nb(wt->tid, &status))
    return false;
  rport_drop_worker(wt);
  for (int k = 0; k < g_nworkers; k++)
    if (g_workers[k]->parent == wt)
      g_workers[k]->parent = 0;
  thread_stack_free(wt->stack);
  while (wt->in.head) {
    struct wmsg *m = wt->in.head;
    wt->in.head = m->next;
    wmsg_free(m);
  }
  free(wt->rpc_src);
  free(wt);
  g_workers[i] = g_workers[g_nworkers - 1];
  g_wobjs[i] = g_wobjs[g_nworkers - 1];
  g_wobjs[g_nworkers - 1] = 0;
  g_nworkers--;
  return true;
}

static void wq_deliver_port(js_value port, js_value data, js_value ports)
{
  js_value ev;
  if (port.type != JV_OBJ || js_to_boolean(js_get(g_ctx, port, "@dead")) ||
      js_to_boolean(js_get(g_ctx, port, "@closed")))
    return;
  gc_push_scope();
  gc_keep(port.u.obj);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  if (ports.type == JV_OBJ)
    gc_keep(ports.u.obj);
  ev = make_message_event(data, ports);
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", port);
    ev_fire_on(port.u.obj, ev, 0);
  }
  gc_pop_scope();
}

int js_workers_pump(void)
{
  int delivered = 0, i;
  if (!g_ctx || !g_page_live)
    return 0;
  if (g_wq && js_array_len(g_wq)) {
    js_object *old;
    unsigned n, k;
    gc_push_scope();
    old = (js_object *)gc_keep(g_wq);
    g_wq = js_new_array(g_ctx, 0);
    n = js_array_len(old);
    for (k = 0; k + 2 < n; k += 3)
      wq_deliver_port(js_get_index(g_ctx, js_obj_value(old), k), js_get_index(g_ctx, js_obj_value(old), k + 1),
                      js_get_index(g_ctx, js_obj_value(old), k + 2));
    gc_pop_scope();
    delivered += (int)(n / 3);
  }
  for (i = 0; i < g_nworkers; i++) {
    struct wthread *wt = g_workers[i];
    struct wmsg *m, *next;
    js_value w = js_obj_value(g_wobjs[i]);
    if (__atomic_load_n(&wt->rpc, __ATOMIC_ACQUIRE) == 1)
      serve_rpc(wt);
    if (wt->parent && (__atomic_load_n(&wt->parent->done, __ATOMIC_ACQUIRE) ||
                       __atomic_load_n(&wt->parent->irq.stop, __ATOMIC_ACQUIRE) ||
                       __atomic_load_n(&wt->parent->closing, __ATOMIC_ACQUIRE)))
      wthread_stop(wt); /* a worker does not outlive the one that started it */
    for (m = wq_take_all(wt, &wt->out); m; m = next) {
      next = m->next;
      if (m->kind == WM_BCAST) {
        if (m->clone && m->text)
          js_bc_dispatch(m->text, m->clone, 0, wt);
      } else if (m->kind == WM_CHILD_POST || m->kind == WM_CHILD_TERM) {
        struct wthread *child = child_of(wt, m->level);
        if (child && m->kind == WM_CHILD_TERM)
          wthread_stop(child);
        else if (child && !__atomic_load_n(&child->irq.stop, __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&child->closing, __ATOMIC_ACQUIRE)) {
          wmsg_post(child, &child->in, WM_DATA, 0, m->clone, 0);
          m->clone = 0;
        }
      } else if (wt->parent && (m->kind == WM_DATA || m->kind == WM_ERROR)) {
        wmsg_post(wt->parent, &wt->parent->in, m->kind == WM_DATA ? WM_CHILD_DATA : WM_CHILD_ERROR, wt->id, m->clone,
                  m->text);
        m->clone = 0;
      } else if (m->kind == WM_PORT || m->kind == WM_PORT_CLOSE) {
        int pi = rport_find(m->level);
        if (pi >= 0 && g_rport_wt[pi] == wt && m->kind == WM_PORT) {
          js_value port = js_obj_value(g_rport_objs[pi]), data;
          clone_peer = wt;
          ports_clear(g_ctx);
          data = js_clone_read(g_ctx, m->clone);
          clone_peer = 0;
          if (g_ctx->has_exc) {
            g_ctx->has_exc = 0;
            g_ctx->exc_root = 0;
          } else {
            gc_push_scope();
            if (data.type == JV_OBJ)
              gc_keep(data.u.obj);
            js_port_enqueue(port, data, ports_take(g_ctx));
            gc_pop_scope();
          }
        } else if (pi >= 0 && g_rport_wt[pi] == wt) {
          js_set(g_ctx, js_obj_value(g_rport_objs[pi]), "@closed", js_bool(1));
          rport_drop(pi);
        }
      } else if (m->kind == WM_REQUEST)
        serve_request(wt, m);
      else if (m->kind == WM_LOG)
        devtools_console_append(m->level == JS_LOG_ERROR  ? DT_ERROR
                                : m->level == JS_LOG_WARN ? DT_WARN
                                                          : DT_LOG,
                                m->text ? m->text : "", m->text ? (int)strlen(m->text) : 0);
      else if (!js_to_boolean(js_get(g_ctx, w, "@dead")))
        worker_fire(wt, w, m);
      delivered++;
      wmsg_free(m);
    }
  }
  for (i = 0; i < g_nworkers;)
    if (!worker_reap(i))
      i++;
  return delivered;
}

void js_port_enqueue(js_value port, js_value data, js_value ports)
{
  gc_push_scope();
  gc_keep(port.u.obj);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  gc_keep(ports.u.obj);
  js_array_push(g_ctx, g_wq, port);
  js_array_push(g_ctx, g_wq, data);
  js_array_push(g_ctx, g_wq, ports);
  gc_pop_scope();
}

static js_value worker_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct wthread *wt = worker_of(ctx, t);
  js_clone *c;
  if (!wt || __atomic_load_n(&wt->irq.stop, __ATOMIC_ACQUIRE) || __atomic_load_n(&wt->closing, __ATOMIC_ACQUIRE))
    return js_undef();
  clone_peer = wt;
  c = js_clone_write(ctx, A(0), A(1));
  clone_peer = 0;
  if (c)
    wmsg_post(wt, &wt->in, WM_DATA, 0, c, 0);
  return js_undef();
}

static js_value worker_terminate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct wthread *wt = worker_of(ctx, t);
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ)
    js_set(ctx, t, "@dead", js_bool(1));
  if (wt)
    wthread_stop(wt);
  return js_undef();
}

/* Starts a worker thread for `url` and the page-side object that tracks it. On failure returns NULL with the
 * error's name and message. `parent` is the worker that asked for it, if any. */
static struct wthread *worker_create(js_ctx *ctx, int id, const char *url, const char *name, int module,
                                     struct wthread *parent, js_object **out, const char **ename, const char **emsg)
{
  js_object *w;
  struct wthread *wt;
  struct purl pu;
  bool inl;
  if (g_nworkers == WORKER_MAX) {
    *ename = "QuotaExceededError";
    *emsg = "Too many workers";
    return 0;
  }
  wt = (struct wthread *)calloc(1, sizeof *wt);
  if (!wt) {
    *ename = "Error";
    *emsg = "Out of memory";
    return 0;
  }
  wt->stack = thread_stack_alloc(WORKER_STACK);
  if (!wt->stack) {
    free(wt);
    *ename = "Error";
    *emsg = "Could not allocate a worker stack";
    return 0;
  }
  wt->stack_lo = (char *)wt->stack - WORKER_STACK;
  wt->id = id;
  wt->parent = parent;
  snprintf(wt->name, sizeof wt->name, "%s", name);
  wt->module = module;
  wt->worklet = url == 0;
  if (!url)
    url = g_page_url_buf;
  snprintf(wt->url, sizeof wt->url, "%s", url);
  inl = starts_ci(url, "blob:") || starts_ci(url, "data:");
  cors_origin(g_page_url_buf, wt->origin, sizeof wt->origin);
  pu.host[0] = 0;
  pu.path[0] = 0;
  pu.tls = 0;
  parse_url(inl ? g_page_url_buf : url, &pu);
  snprintf(wt->pathname, sizeof wt->pathname, "%s", pu.path);
  snprintf(wt->host, sizeof wt->host, "%s", pu.host);
  wt->tls = pu.tls;
  wt->src = wt->worklet ? 0 : fetch_source(url);
  gc_push_scope();
  w = (js_object *)gc_keep(js_new_object(ctx, g_worker_proto));
  js_def(w, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "@dead", js_bool(0), P_WRITABLE);
  js_def(w, "@url", js_mk_cstring(ctx, url), 0);
  js_def(w, "@id", js_number(wt->id), 0);
  wt->tid = thread_create_arg(worker_main, wt, wt->stack);
  if (wt->tid < 0) {
    thread_stack_free(wt->stack);
    free(wt->src);
    free(wt);
    gc_pop_scope();
    *ename = "Error";
    *emsg = "Could not start the worker thread";
    return 0;
  }
  g_workers[g_nworkers] = wt;
  g_wobjs[g_nworkers++] = w;
  *out = w;
  gc_pop_scope();
  return wt;
}

/* A hidden worker running the AudioWorkletGlobalScope of one context; the page talks to it through the object */
js_value js_audio_thread(js_ctx *ctx)
{
  js_object *w = 0;
  const char *ename, *emsg;
  if (!worker_create(ctx, __atomic_add_fetch(&g_next_wid, 1, __ATOMIC_RELAXED), 0, "", 0, 0, &w, &ename, &emsg))
    return throw_err(ctx, ename, emsg);
  return js_obj_value(w);
}

static js_value worker_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  char url[URL_MAX];
  js_object *w = 0;
  js_value opts = A(1), nm, ty;
  const char *ename, *emsg;
  (void)thisv;
  script_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  if (!starts_ci(url, "blob:") && !starts_ci(url, "data:") && !cors_same_origin(g_page_url_buf, url))
    return throw_err(ctx, "SecurityError", "Worker script must be same-origin");
  nm = opts.type == JV_OBJ ? js_get(ctx, opts, "name") : js_undef();
  ty = opts.type == JV_OBJ ? js_get(ctx, opts, "type") : js_undef();
  if (!worker_create(ctx, __atomic_add_fetch(&g_next_wid, 1, __ATOMIC_RELAXED), url,
                     nm.type == JV_STR ? nm.u.str->data : "", ty.type == JV_STR && streq(ty.u.str->data, "module"), 0,
                     &w, &ename, &emsg))
    return throw_err(ctx, ename, emsg);
  return js_obj_value(w);
}

/* new SharedWorker(url, name | {name, type}): the worker for that URL and name, started on the first connection;
 * each connection gets a MessageChannel whose far port goes to the worker in a connect event */
static js_value sworker_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  char url[URL_MAX];
  js_value opts = A(1), nm, mc, p1, p2, msg, list;
  const char *name = "", *ename, *emsg;
  struct wthread *wt = 0;
  js_object *hub = 0, *sw, *o, *clients;
  js_clone *c;
  int i, module = 0;
  (void)thisv;
  script_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  if (!starts_ci(url, "blob:") && !starts_ci(url, "data:") && !cors_same_origin(g_page_url_buf, url))
    return throw_err(ctx, "SecurityError", "SharedWorker script must be same-origin");
  if (opts.type == JV_STR)
    name = opts.u.str->data;
  else if (opts.type == JV_OBJ) {
    nm = js_get(ctx, opts, "name");
    if (nm.type == JV_STR)
      name = nm.u.str->data;
    nm = js_get(ctx, opts, "type");
    module = nm.type == JV_STR && streq(nm.u.str->data, "module");
  }
  for (i = 0; i < g_nworkers; i++)
    if (g_workers[i]->shared && !g_workers[i]->parent && streq(g_workers[i]->url, url) &&
        streq(g_workers[i]->name, name) && !__atomic_load_n(&g_workers[i]->irq.stop, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_workers[i]->closing, __ATOMIC_ACQUIRE)) {
      wt = g_workers[i];
      hub = g_wobjs[i];
    }
  gc_push_scope();
  if (!wt) {
    wt = worker_create(ctx, __atomic_add_fetch(&g_next_wid, 1, __ATOMIC_RELAXED), url, name, module, 0, &hub, &ename,
                       &emsg);
    if (!wt) {
      gc_pop_scope();
      return throw_err(ctx, ename, emsg);
    }
    wt->shared = 1;
    gc_keep(hub);
    js_def(hub, "@clients", js_obj_value(js_new_array(ctx, 0)), 0);
  }
  gc_keep(hub);
  mc = js_construct(ctx, js_get(ctx, js_obj_value(ctx->global), "MessageChannel"), 0, 0);
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(mc.u.obj);
  p1 = js_get(ctx, mc, "port1");
  p2 = js_get(ctx, mc, "port2");
  sw = (js_object *)gc_keep(js_new_object(ctx, g_sworker_proto));
  js_def(sw, "port", p1, P_ENUMERABLE);
  js_def(sw, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  clients = js_get(ctx, js_obj_value(hub), "@clients").u.obj;
  js_array_push(ctx, clients, js_obj_value(sw));
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "port", p2, P_ENUMERABLE);
  list = js_obj_value(gc_keep(js_new_array(ctx, 0)));
  js_array_push(ctx, list.u.obj, p2);
  msg = js_obj_value(o);
  clone_peer = wt;
  c = js_clone_write(ctx, msg, list);
  clone_peer = 0;
  if (c)
    wmsg_post(wt, &wt->in, WM_CONNECT, 0, c, 0);
  gc_pop_scope();
  return ctx->has_exc ? js_undef() : js_obj_value(sw);
}

void js_worker_setup(js_ctx *ctx, js_object *win)
{
  js_object *ctor;
  g_worker_proto = js_new_object(ctx, ctx->object_proto);
  g_wq = js_new_array(ctx, 0);
  gc_add_root((void **)&g_worker_proto);
  gc_add_root((void **)&g_wq);
  g_nworkers = 0;
  gc_add_root_range((void **)g_wobjs, &g_nworkers);
  g_nrports = 0;
  gc_add_root_range((void **)g_rport_objs, &g_nrports);
  js_msgchannel_setup(ctx, win);
  def_method(g_worker_proto, "postMessage", worker_post, 1);
  def_method(g_worker_proto, "terminate", worker_terminate, 0);
  def_method(g_worker_proto, "addEventListener", el_add_listener, 3);
  def_method(g_worker_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_worker_proto, "dispatchEvent", el_dispatch, 1);
  ctor = js_new_native(ctx, worker_ctor, "Worker", 1);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(g_worker_proto), 0);
  js_def(g_worker_proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "Worker", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  g_sworker_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_sworker_proto);
  def_method(g_sworker_proto, "addEventListener", el_add_listener, 3);
  def_method(g_sworker_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_sworker_proto, "dispatchEvent", el_dispatch, 1);
  ctor = js_new_native(ctx, sworker_ctor, "SharedWorker", 1);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(g_sworker_proto), 0);
  js_def(g_sworker_proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "SharedWorker", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

/* Stop every worker and join what finishes in time. A thread stuck in a long script is left behind. */
void js_worker_teardown(void)
{
  int i, waited;
  for (i = 0; i < g_nworkers; i++)
    wthread_stop(g_workers[i]);
  for (waited = 0; g_nworkers && waited < 1000; waited += 5) {
    for (i = 0; i < g_nworkers;)
      if (!worker_reap(i))
        i++;
    if (g_nworkers)
      msleep(5);
  }
  g_nworkers = 0;
  js_msgchannel_teardown();
  gc_remove_root((void **)&g_worker_proto);
  gc_remove_root((void **)&g_sworker_proto);
  gc_remove_root((void **)&g_wq);
  gc_remove_root_range((void **)g_wobjs);
  gc_remove_root_range((void **)g_rport_objs);
  g_nrports = 0;
  g_worker_proto = g_sworker_proto = g_wq = 0;
}
