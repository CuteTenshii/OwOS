/* Web Audio: AudioContext, AudioWorklet, AudioWorkletNode and AudioParam. The classes are created the first time a
 * script touches one of the globals. Rendering happens on a hidden worker thread (audioworklet.c, started by
 * js_audio_thread); this side validates calls and forwards them as messages. State lives in hidden "@" properties of
 * the objects, so nothing here needs a GC root. */

#include "bindings.h"
#include "math.h"

static const char *const k_globals[] = {"AudioContext",     "webkitAudioContext",   "BaseAudioContext",
                                        "AudioNode",        "AudioDestinationNode", "AudioWorklet",
                                        "AudioWorkletNode", "AudioParam",           "AudioParamMap"};

enum { LOAD_NONE, LOAD_RUNNING, LOAD_OK, LOAD_FAILED };
static int g_load_state;

#define RATE 48000
#define PE (P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE)

/* ---- helpers --------------------------------------------------------------- */

static js_value global_of(js_ctx *ctx) { return js_obj_value(ctx->global); }

static const char *str_of(js_value v) { return v.type == JV_STR ? v.u.str->data : ""; }

static js_value audio_proto(js_ctx *ctx, const char *name)
{
  return js_get(ctx, js_get(ctx, global_of(ctx), "@audio"), name);
}

static js_value throw_dom(js_ctx *ctx, const char *name, const char *msg)
{
  js_throw(ctx, dom_exception(ctx, name, msg));
  return js_undef();
}

static js_value reject_dom(js_ctx *ctx, const char *name, const char *msg)
{
  return js_promise_rejected(ctx, dom_exception(ctx, name, msg));
}

static bool is_kind(js_value t, const char *kind)
{
  js_value k;
  if (t.type != JV_OBJ)
    return false;
  k = js_get(g_ctx, t, "@kind");
  return k.type == JV_STR && streq(k.u.str->data, kind);
}

static bool brand(js_ctx *ctx, js_value t, const char *kind)
{
  if (is_kind(t, kind))
    return true;
  js_throw_type(ctx, "Illegal invocation");
  return false;
}

static js_value illegal(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

/* A finite number, or false with a TypeError pending */
static bool finite(js_ctx *ctx, js_value v, double *out)
{
  double d = js_to_number(ctx, v);
  if (ctx->has_exc)
    return false;
  if (d != d || d - d != 0) {
    js_throw_type(ctx, "The provided float value is non-finite");
    return false;
  }
  *out = d;
  return true;
}

/* A time: finite and not negative */
static bool when(js_ctx *ctx, js_value v, double *out)
{
  if (!finite(ctx, v, out))
    return false;
  if (*out < 0) {
    js_throw_range(ctx, "The time value must be non-negative");
    return false;
  }
  return true;
}

/* A non-negative integer */
static bool count(js_ctx *ctx, js_value v, int *out)
{
  double d = js_to_number(ctx, v);
  if (ctx->has_exc)
    return false;
  if (!(d >= 0) || d != floor(d) || d > 1e9) {
    js_throw_type(ctx, "The provided value is not a valid count");
    return false;
  }
  *out = (int)d;
  return true;
}

static void set_num(js_ctx *ctx, js_value o, const char *key, double v)
{
  js_def(o.u.obj, key, js_number(v), PE);
  (void)ctx;
}

static js_value new_msg(js_ctx *ctx, const char *type)
{
  js_object *o = js_new_object(ctx, ctx->object_proto);
  js_def(o, "t", js_mk_cstring(ctx, type), PE);
  return js_obj_value(o);
}

static js_value constructed_this(js_ctx *ctx, js_value thisv, const char *proto)
{
  return thisv.type == JV_OBJ ? thisv : js_obj_value(js_new_object(ctx, audio_proto(ctx, proto).u.obj));
}

/* ---- the context ------------------------------------------------------------ */

static double now_of(js_ctx *ctx, js_value c)
{
  double acc = js_to_number(ctx, js_get(ctx, c, "@acc"));
  if (streq(str_of(js_get(ctx, c, "@state")), "running"))
    acc += (double)(unsigned)(msec() - (unsigned)js_to_number(ctx, js_get(ctx, c, "@t0"))) / 1000.0;
  return acc;
}

static js_value fire_statechange(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c = ctx->native_data, ev;
  (void)t;
  (void)args;
  (void)argc;
  if (c.type != JV_OBJ)
    return js_undef();
  ev = make_event("statechange", 0, 0, js_undef());
  gc_push_scope();
  gc_keep(ev.u.obj);
  js_set(ctx, ev, "bubbles", js_bool(0));
  js_set(ctx, ev, "cancelable", js_bool(0));
  js_set(ctx, ev, "composed", js_bool(0));
  js_set(ctx, ev, "target", c);
  ev_fire_on(c.u.obj, ev, 0);
  gc_pop_scope();
  return js_undef();
}

/* Change the state and tell the listeners from a timer, as a browser does */
static void announce(js_ctx *ctx, js_value c, const char *state)
{
  js_value timeout = js_get(ctx, global_of(ctx), "setTimeout"), argv[2];
  js_object *fn;
  if (streq(str_of(js_get(ctx, c, "@state")), state))
    return;
  js_set(ctx, c, "@acc", js_number(now_of(ctx, c)));
  js_set(ctx, c, "@state", js_mk_cstring(ctx, state));
  js_set(ctx, c, "@t0", js_number((double)msec()));
  if (!js_is_callable(timeout))
    return;
  gc_push_scope();
  fn = (js_object *)gc_keep(js_new_native(ctx, fire_statechange, "statechange", 0));
  fn->fn->data = c;
  gc_write_barrier(fn);
  argv[0] = js_obj_value(fn);
  argv[1] = js_number(0);
  js_call(ctx, timeout, js_undef(), argv, 2);
  gc_pop_scope();
}

static js_value thread_post(js_ctx *ctx, js_value c, js_value msg, js_value transfer)
{
  js_value w = js_get(ctx, c, "@thread"), argv[2];
  if (w.type != JV_OBJ)
    return js_undef();
  argv[0] = msg;
  argv[1] = transfer;
  return js_call(ctx, js_get(ctx, w, "postMessage"), w, argv, transfer.type == JV_OBJ ? 2 : 1);
}

static void post_state(js_ctx *ctx, js_value c, bool run, double time)
{
  js_value m = new_msg(ctx, "state");
  gc_push_scope();
  gc_keep(m.u.obj);
  js_def(m.u.obj, "run", js_bool(run), PE);
  set_num(ctx, m, "time", time);
  thread_post(ctx, c, m, js_undef());
  gc_pop_scope();
}

static js_value processor_error(js_ctx *ctx, js_value node, js_value message)
{
  js_value ev = make_event("processorerror", 0, 0, js_undef());
  gc_push_scope();
  gc_keep(ev.u.obj);
  event_adopt_iface(ev, "ErrorEvent");
  js_set(ctx, ev, "bubbles", js_bool(0));
  js_set(ctx, ev, "cancelable", js_bool(0));
  js_def(ev.u.obj, "message", message, PE);
  js_def(ev.u.obj, "filename", js_mk_cstring(ctx, ""), PE);
  js_def(ev.u.obj, "lineno", js_number(0), PE);
  js_def(ev.u.obj, "colno", js_number(0), PE);
  js_def(ev.u.obj, "error", js_null(), PE);
  js_set(ctx, ev, "target", node);
  ev_fire_on(node.u.obj, ev, 0);
  gc_pop_scope();
  return js_undef();
}

static js_value module_error(js_ctx *ctx, js_value m)
{
  const char *name = str_of(js_get(ctx, m, "name")), *msg = str_of(js_get(ctx, m, "message"));
  static const char *const plain[] = {"Error", "TypeError", "SyntaxError", "RangeError", "ReferenceError"};
  unsigned i;
  for (i = 0; i < sizeof plain / sizeof *plain; i++)
    if (streq(name, plain[i])) {
      js_value ctor = js_get(ctx, global_of(ctx), name), arg = js_mk_cstring(ctx, msg);
      return js_construct(ctx, ctor, &arg, 1);
    }
  return dom_exception(ctx, name, msg);
}

/* Messages from the worklet thread: module results and processor errors */
static js_value on_thread(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c = ctx->native_data, m = js_get(ctx, A(0), "data");
  const char *type;
  char key[16];
  (void)t;
  if (m.type != JV_OBJ || c.type != JV_OBJ)
    return js_undef();
  type = str_of(js_get(ctx, m, "t"));
  snprintf(key, sizeof key, "%d", (int)js_to_number(ctx, js_get(ctx, m, "id")));
  if (streq(type, "module")) {
    js_value jobs = js_get(ctx, c, "@jobs"), job = js_get(ctx, jobs, key), reg = js_get(ctx, m, "reg");
    js_value registry = js_get(ctx, c, "@registry");
    unsigned i;
    gc_push_scope();
    for (i = 0; reg.type == JV_OBJ && i < js_array_len(reg.u.obj); i++) {
      js_value r = js_get_index(ctx, reg, i);
      js_set(ctx, registry, str_of(js_get(ctx, r, "name")), js_get(ctx, r, "params"));
    }
    js_delete(jobs.u.obj, key);
    if (job.type == JV_OBJ) {
      if (js_to_boolean(js_get(ctx, m, "ok")))
        js_promise_settle(ctx, job.u.obj, 0, js_undef());
      else {
        js_value err = module_error(ctx, m);
        gc_keep(err.u.obj);
        js_promise_settle(ctx, job.u.obj, 1, err);
      }
    }
    gc_pop_scope();
  } else if (streq(type, "error")) {
    js_value node = js_get(ctx, js_get(ctx, c, "@nodes"), key);
    if (node.type == JV_OBJ)
      processor_error(ctx, node, js_get(ctx, m, "message"));
  }
  return js_undef();
}

/* The context's worklet thread, started on first use; it renders while the context runs */
static js_value thread_of(js_ctx *ctx, js_value c)
{
  js_value w = js_get(ctx, c, "@thread");
  js_object *fn;
  if (w.type == JV_OBJ)
    return w;
  w = js_audio_thread(ctx);
  if (ctx->has_exc)
    return js_undef();
  gc_push_scope();
  gc_keep(w.u.obj);
  js_def(c.u.obj, "@thread", w, P_WRITABLE);
  fn = (js_object *)gc_keep(js_new_native(ctx, on_thread, "onmessage", 1));
  fn->fn->data = c;
  gc_write_barrier(fn);
  js_set(ctx, w, "onmessage", js_obj_value(fn));
  post_state(ctx, c, streq(str_of(js_get(ctx, c, "@state")), "running"), now_of(ctx, c));
  gc_pop_scope();
  return w;
}

static js_value ctx_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value c, dest, worklet;
  if (argc > 0 && args[0].type != JV_UNDEF && args[0].type != JV_OBJ)
    return js_throw_type(ctx, "Failed to construct AudioContext: parameter 1 is not of type AudioContextOptions");
  gc_push_scope();
  c = constructed_this(ctx, thisv, "ctx");
  gc_keep(c.u.obj);
  js_def(c.u.obj, "@kind", js_mk_cstring(ctx, "ctx"), P_WRITABLE);
  js_def(c.u.obj, "@state", js_mk_cstring(ctx, "running"), P_WRITABLE);
  js_def(c.u.obj, "@acc", js_number(0), P_WRITABLE);
  js_def(c.u.obj, "@t0", js_number((double)msec()), P_WRITABLE);
  js_def(c.u.obj, "@registry", js_obj_value(js_new_object(ctx, ctx->object_proto)), P_WRITABLE);
  js_def(c.u.obj, "@loaded", js_obj_value(js_new_object(ctx, ctx->object_proto)), P_WRITABLE);
  js_def(c.u.obj, "@jobs", js_obj_value(js_new_object(ctx, ctx->object_proto)), P_WRITABLE);
  js_def(c.u.obj, "@nodes", js_obj_value(js_new_object(ctx, ctx->object_proto)), P_WRITABLE);
  js_def(c.u.obj, "@jobid", js_number(0), P_WRITABLE);
  js_def(c.u.obj, "@nodeid", js_number(0), P_WRITABLE);
  dest = js_obj_value(js_new_object(ctx, audio_proto(ctx, "dest").u.obj));
  gc_keep(dest.u.obj);
  js_def(dest.u.obj, "@kind", js_mk_cstring(ctx, "node"), P_WRITABLE);
  js_def(dest.u.obj, "@ctx", c, P_WRITABLE);
  js_def(dest.u.obj, "@id", js_number(0), P_WRITABLE);
  js_def(dest.u.obj, "@nin", js_number(1), P_WRITABLE);
  js_def(dest.u.obj, "@nout", js_number(0), P_WRITABLE);
  js_def(dest.u.obj, "@cc", js_number(2), P_WRITABLE);
  js_def(dest.u.obj, "@ccm", js_mk_cstring(ctx, "explicit"), P_WRITABLE);
  js_def(dest.u.obj, "@ci", js_mk_cstring(ctx, "speakers"), P_WRITABLE);
  js_def(c.u.obj, "@dest", dest, P_WRITABLE);
  worklet = js_obj_value(js_new_object(ctx, audio_proto(ctx, "worklet").u.obj));
  gc_keep(worklet.u.obj);
  js_def(worklet.u.obj, "@kind", js_mk_cstring(ctx, "worklet"), P_WRITABLE);
  js_def(worklet.u.obj, "@ctx", c, P_WRITABLE);
  js_def(c.u.obj, "@worklet", worklet, P_WRITABLE);
  js_def(c.u.obj, "onstatechange", js_null(), PE);
  gc_pop_scope();
  return c;
}

static js_value ctx_get_rate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_number(RATE) : js_undef();
}

static js_value ctx_get_time(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_number(now_of(ctx, t)) : js_undef();
}

static js_value ctx_get_state(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_get(ctx, t, "@state") : js_undef();
}

static js_value ctx_get_destination(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_get(ctx, t, "@dest") : js_undef();
}

static js_value ctx_get_worklet(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_get(ctx, t, "@worklet") : js_undef();
}

static js_value ctx_get_base_latency(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_number(512.0 / RATE) : js_undef();
}

static js_value ctx_get_output_latency(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "ctx") ? js_number(0) : js_undef();
}

static js_value ctx_resume(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (!is_kind(t, "ctx"))
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Illegal invocation"));
  if (streq(str_of(js_get(ctx, t, "@state")), "closed"))
    return reject_dom(ctx, "InvalidStateError", "Cannot resume a closed AudioContext");
  announce(ctx, t, "running");
  post_state(ctx, t, true, now_of(ctx, t));
  return js_promise_resolved(ctx, js_undef());
}

static js_value ctx_suspend(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (!is_kind(t, "ctx"))
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Illegal invocation"));
  if (streq(str_of(js_get(ctx, t, "@state")), "closed"))
    return reject_dom(ctx, "InvalidStateError", "Cannot suspend a closed AudioContext");
  announce(ctx, t, "suspended");
  post_state(ctx, t, false, now_of(ctx, t));
  return js_promise_resolved(ctx, js_undef());
}

static js_value ctx_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value w, jobs;
  (void)args;
  (void)argc;
  if (!is_kind(t, "ctx"))
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Illegal invocation"));
  if (streq(str_of(js_get(ctx, t, "@state")), "closed"))
    return reject_dom(ctx, "InvalidStateError", "Cannot close a closed AudioContext");
  announce(ctx, t, "closed");
  w = js_get(ctx, t, "@thread");
  if (w.type == JV_OBJ)
    js_call(ctx, js_get(ctx, w, "terminate"), w, 0, 0);
  jobs = js_get(ctx, t, "@jobs");
  gc_push_scope();
  js_set(ctx, t, "@jobs", js_obj_value(js_new_object(ctx, ctx->object_proto)));
  if (jobs.type == JV_OBJ) {
    js_value keys = js_call(ctx, js_get(ctx, js_get(ctx, global_of(ctx), "Object"), "keys"), js_undef(), &jobs, 1);
    unsigned i;
    gc_keep(keys.u.obj);
    for (i = 0; keys.type == JV_OBJ && i < js_array_len(keys.u.obj); i++) {
      js_value job = js_get(ctx, jobs, str_of(js_get_index(ctx, keys, i)));
      if (job.type == JV_OBJ)
        js_promise_settle(ctx, job.u.obj, 1, dom_exception(ctx, "AbortError", "The AudioContext was closed"));
    }
  }
  gc_pop_scope();
  return js_promise_resolved(ctx, js_undef());
}

/* ---- the worklet ------------------------------------------------------------- */

static js_value worklet_add_module(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c, key, loaded, job, m;
  js_object *p;
  char id[16];
  int n;
  if (!is_kind(t, "worklet"))
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Illegal invocation"));
  c = js_get(ctx, t, "@ctx");
  key = js_to_string(ctx, A(0));
  if (ctx->has_exc)
    return js_undef();
  if (streq(str_of(js_get(ctx, c, "@state")), "closed"))
    return reject_dom(ctx, "InvalidStateError", "The AudioContext is closed");
  loaded = js_get(ctx, c, "@loaded");
  job = js_get(ctx, loaded, str_of(key));
  if (job.type == JV_OBJ)
    return job;
  gc_push_scope();
  gc_keep(key.u.str);
  p = (js_object *)gc_keep(js_promise_new(ctx));
  n = (int)js_to_number(ctx, js_get(ctx, c, "@jobid")) + 1;
  js_set(ctx, c, "@jobid", js_number(n));
  snprintf(id, sizeof id, "%d", n);
  js_set(ctx, js_get(ctx, c, "@jobs"), id, js_obj_value(p));
  js_set(ctx, loaded, str_of(key), js_obj_value(p));
  if (js_is_undef(thread_of(ctx, c))) {
    js_value err = ctx->exc;
    ctx->has_exc = 0;
    ctx->exc_root = 0;
    gc_keep(err.u.obj);
    js_delete(js_get(ctx, c, "@jobs").u.obj, id);
    js_delete(loaded.u.obj, str_of(key));
    js_promise_settle(ctx, p, 1, err);
    gc_pop_scope();
    return js_obj_value(p);
  }
  m = new_msg(ctx, "module");
  gc_keep(m.u.obj);
  set_num(ctx, m, "id", n);
  js_def(m.u.obj, "url", key, PE);
  thread_post(ctx, c, m, js_undef());
  gc_pop_scope();
  return js_obj_value(p);
}

/* ---- nodes -------------------------------------------------------------------- */

static js_value node_get(js_ctx *ctx, js_value t, const char *key)
{
  return brand(ctx, t, "node") ? js_get(ctx, t, key) : js_undef();
}

static js_value node_get_context(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@ctx");
}

static js_value node_get_nin(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@nin");
}

static js_value node_get_nout(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@nout");
}

static js_value node_get_cc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@cc");
}

static js_value node_set_cc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int n;
  if (brand(ctx, t, "node") && count(ctx, A(0), &n))
    js_set(ctx, t, "@cc", js_number(n));
  return js_undef();
}

static js_value node_get_ccm(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@ccm");
}

static js_value node_set_ccm(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  if (brand(ctx, t, "node"))
    js_set(ctx, t, "@ccm", js_to_string(ctx, A(0)));
  return js_undef();
}

static js_value node_get_ci(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@ci");
}

static js_value node_set_ci(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  if (brand(ctx, t, "node"))
    js_set(ctx, t, "@ci", js_to_string(ctx, A(0)));
  return js_undef();
}

static js_value dest_get_max_channels(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return brand(ctx, t, "node") ? js_number(2) : js_undef();
}

static js_value wnode_get_port(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@port");
}

static js_value wnode_get_parameters(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return node_get(ctx, t, "@map");
}

static js_value node_connect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value dest = A(0), c, m;
  int out = 0, inp = 0;
  if (!brand(ctx, t, "node"))
    return js_undef();
  if (is_kind(dest, "param"))
    return throw_dom(ctx, "NotSupportedError", "Connecting a node to an AudioParam is not supported");
  if (!is_kind(dest, "node"))
    return js_throw_type(ctx, "Failed to execute connect on AudioNode: parameter 1 is not of type AudioNode");
  c = js_get(ctx, t, "@ctx");
  if (js_get(ctx, dest, "@ctx").u.obj != c.u.obj)
    return throw_dom(ctx, "InvalidAccessError", "Cannot connect to a destination of a different audio context");
  if (A(1).type != JV_UNDEF && !count(ctx, A(1), &out))
    return js_undef();
  if (A(2).type != JV_UNDEF && !count(ctx, A(2), &inp))
    return js_undef();
  if (out >= (int)js_to_number(ctx, js_get(ctx, t, "@nout")))
    return throw_dom(ctx, "IndexSizeError", "The output index exceeds the number of outputs");
  if (inp >= (int)js_to_number(ctx, js_get(ctx, dest, "@nin")))
    return throw_dom(ctx, "IndexSizeError", "The input index exceeds the number of inputs");
  thread_of(ctx, c);
  if (ctx->has_exc)
    return js_undef();
  gc_push_scope();
  m = new_msg(ctx, "conn");
  gc_keep(m.u.obj);
  js_def(m.u.obj, "from", js_get(ctx, t, "@id"), PE);
  set_num(ctx, m, "out", out);
  js_def(m.u.obj, "to", js_get(ctx, dest, "@id"), PE);
  set_num(ctx, m, "inp", inp);
  thread_post(ctx, c, m, js_undef());
  gc_pop_scope();
  return dest;
}

static js_value node_disconnect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c, m, a = A(0);
  if (!brand(ctx, t, "node"))
    return js_undef();
  c = js_get(ctx, t, "@ctx");
  if (js_get(ctx, c, "@thread").type != JV_OBJ)
    return js_undef();
  gc_push_scope();
  m = new_msg(ctx, "disc");
  gc_keep(m.u.obj);
  js_def(m.u.obj, "from", js_get(ctx, t, "@id"), PE);
  js_def(m.u.obj, "out", js_null(), PE);
  js_def(m.u.obj, "to", js_null(), PE);
  js_def(m.u.obj, "inp", js_null(), PE);
  if (a.type == JV_NUM) {
    if (a.u.num >= js_to_number(ctx, js_get(ctx, t, "@nout"))) {
      gc_pop_scope();
      return throw_dom(ctx, "IndexSizeError", "The output index exceeds the number of outputs");
    }
    js_set(ctx, m, "out", a);
  } else if (a.type != JV_UNDEF) {
    int n;
    if (!is_kind(a, "node")) {
      gc_pop_scope();
      return js_throw_type(ctx, "Failed to execute disconnect on AudioNode: parameter 1 is not of type AudioNode");
    }
    js_set(ctx, m, "to", js_get(ctx, a, "@id"));
    if (A(1).type != JV_UNDEF && count(ctx, A(1), &n))
      set_num(ctx, m, "out", n);
    if (A(2).type != JV_UNDEF && count(ctx, A(2), &n))
      set_num(ctx, m, "inp", n);
  }
  thread_post(ctx, c, m, js_undef());
  gc_pop_scope();
  return js_undef();
}

/* ---- parameters ----------------------------------------------------------------- */

static js_value make_param(js_ctx *ctx, js_value c, int id, js_value d, double value)
{
  js_object *p = js_new_object(ctx, audio_proto(ctx, "param").u.obj);
  js_def(p, "@kind", js_mk_cstring(ctx, "param"), P_WRITABLE);
  js_def(p, "@ctx", c, P_WRITABLE);
  js_def(p, "@id", js_number(id), P_WRITABLE);
  js_def(p, "@name", js_get(ctx, d, "name"), P_WRITABLE);
  js_def(p, "@v", js_number(value), P_WRITABLE);
  js_def(p, "@def", js_get(ctx, d, "defaultValue"), P_WRITABLE);
  js_def(p, "@min", js_get(ctx, d, "minValue"), P_WRITABLE);
  js_def(p, "@max", js_get(ctx, d, "maxValue"), P_WRITABLE);
  js_def(p, "@rate", js_get(ctx, d, "automationRate"), P_WRITABLE);
  return js_obj_value(p);
}

static void param_post(js_ctx *ctx, js_value p, const char *op, js_value a, double b, double c)
{
  js_value m = new_msg(ctx, "p");
  gc_push_scope();
  gc_keep(m.u.obj);
  js_def(m.u.obj, "id", js_get(ctx, p, "@id"), PE);
  js_def(m.u.obj, "name", js_get(ctx, p, "@name"), PE);
  js_def(m.u.obj, "op", js_mk_cstring(ctx, op), PE);
  js_def(m.u.obj, "a", a, PE);
  set_num(ctx, m, "b", b);
  set_num(ctx, m, "c", c);
  thread_post(ctx, js_get(ctx, p, "@ctx"), m, js_undef());
  gc_pop_scope();
}

static js_value param_get(js_ctx *ctx, js_value t, const char *key)
{
  return brand(ctx, t, "param") ? js_get(ctx, t, key) : js_undef();
}

static js_value param_get_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return param_get(ctx, t, "@v");
}

static js_value param_set_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double v;
  if (brand(ctx, t, "param") && finite(ctx, A(0), &v)) {
    js_set(ctx, t, "@v", js_number(v));
    param_post(ctx, t, "value", js_number(v), 0, 0);
  }
  return js_undef();
}

static js_value param_get_def(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return param_get(ctx, t, "@def");
}

static js_value param_get_min(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return param_get(ctx, t, "@min");
}

static js_value param_get_max(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return param_get(ctx, t, "@max");
}

static js_value param_get_rate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return param_get(ctx, t, "@rate");
}

static js_value param_set_rate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = js_to_string(ctx, A(0));
  if (!brand(ctx, t, "param") || ctx->has_exc || !(streq(str_of(v), "a-rate") || streq(str_of(v), "k-rate")))
    return js_undef();
  gc_push_scope();
  gc_keep(v.u.str);
  js_set(ctx, t, "@rate", v);
  param_post(ctx, t, "rate", v, 0, 0);
  gc_pop_scope();
  return js_undef();
}

static js_value param_set_value_at(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double v, when_;
  if (!brand(ctx, t, "param") || !finite(ctx, A(0), &v) || !when(ctx, A(1), &when_))
    return js_undef();
  if (when_ <= now_of(ctx, js_get(ctx, t, "@ctx")))
    js_set(ctx, t, "@v", js_number(v));
  param_post(ctx, t, "set", js_number(v), when_, 0);
  return t;
}

static js_value param_ramp(js_ctx *ctx, js_value t, js_value *args, int argc, const char *op)
{
  double v, when_;
  if (!brand(ctx, t, "param") || !finite(ctx, A(0), &v) || !when(ctx, A(1), &when_))
    return js_undef();
  if (streq(op, "exp") && v == 0)
    return js_throw_range(ctx, "The float value to ramp to cannot be zero");
  param_post(ctx, t, op, js_number(v), when_, 0);
  return t;
}

static js_value param_linear(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return param_ramp(ctx, t, args, argc, "lin");
}

static js_value param_exponential(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return param_ramp(ctx, t, args, argc, "exp");
}

static js_value param_set_target(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double target, start, tc;
  if (!brand(ctx, t, "param") || !finite(ctx, A(0), &target) || !when(ctx, A(1), &start) || !finite(ctx, A(2), &tc))
    return js_undef();
  param_post(ctx, t, "tgt", js_number(target), start, tc);
  return t;
}

/* The curve is played as linear segments between its values, which is what the spec's interpolation comes to */
static js_value param_set_curve(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double start, duration, v;
  js_value values = A(0);
  unsigned n, k;
  if (!brand(ctx, t, "param") || !when(ctx, A(1), &start) || !finite(ctx, A(2), &duration))
    return js_undef();
  if (values.type != JV_OBJ || js_get(ctx, values, "length").type != JV_NUM)
    return js_throw_type(ctx, "The curve is not a sequence of numbers");
  n = (unsigned)js_to_number(ctx, js_get(ctx, values, "length"));
  if (n < 2)
    return throw_dom(ctx, "InvalidStateError", "The curve must have at least two values");
  if (duration <= 0)
    return js_throw_range(ctx, "The curve duration must be positive");
  if (!finite(ctx, js_get_index(ctx, values, 0), &v))
    return js_undef();
  param_post(ctx, t, "set", js_number(v), start, 0);
  for (k = 1; k < n; k++) {
    if (!finite(ctx, js_get_index(ctx, values, k), &v))
      return js_undef();
    param_post(ctx, t, "lin", js_number(v), start + (double)k * duration / (double)(n - 1), 0);
  }
  return t;
}

static js_value param_cancel(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double start;
  if (!brand(ctx, t, "param") || !when(ctx, A(0), &start))
    return js_undef();
  param_post(ctx, t, "cancel", js_number(start), 0, 0);
  return t;
}

static js_value param_hold(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  double start;
  if (!brand(ctx, t, "param") || !when(ctx, A(0), &start))
    return js_undef();
  param_post(ctx, t, "hold", js_number(start), 0, 0);
  return t;
}

/* ---- AudioWorkletNode -------------------------------------------------------------- */

static js_value wnode_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value c, name, opts, reg, ochan = js_undef(), pdata, popts, map, mc, port1, port2, m, transfer, node;
  int nin = 1, nout = 1, i, id, automatic;
  unsigned np;
  char key[16];
  if (argc < 2)
    return js_throw_type(ctx, "Failed to construct AudioWorkletNode: 2 arguments required");
  c = args[0];
  if (!is_kind(c, "ctx"))
    return js_throw_type(ctx, "Failed to construct AudioWorkletNode: parameter 1 is not of type BaseAudioContext");
  name = js_to_string(ctx, args[1]);
  if (ctx->has_exc)
    return js_undef();
  opts = argc > 2 && args[2].type != JV_NULL ? args[2] : js_undef();
  if (opts.type != JV_UNDEF && opts.type != JV_OBJ)
    return js_throw_type(ctx, "Failed to construct AudioWorkletNode: parameter 3 is not an object");
  reg = js_get(ctx, js_get(ctx, c, "@registry"), str_of(name));
  if (reg.type != JV_OBJ)
    return throw_dom(ctx, "InvalidStateError", "The node name is not defined in AudioWorkletGlobalScope");
  if (opts.type == JV_OBJ) {
    js_value v = js_get(ctx, opts, "numberOfInputs");
    if (v.type != JV_UNDEF && !count(ctx, v, &nin))
      return js_undef();
    v = js_get(ctx, opts, "numberOfOutputs");
    if (v.type != JV_UNDEF && !count(ctx, v, &nout))
      return js_undef();
    ochan = js_get(ctx, opts, "outputChannelCount");
  }
  if (nin == 0 && nout == 0)
    return throw_dom(ctx, "NotSupportedError", "Cannot create a node with zero inputs and zero outputs");
  if (nin > 32 || nout > 32)
    return throw_dom(ctx, "NotSupportedError", "The number of inputs or outputs is out of range");
  gc_push_scope();
  gc_keep(name.u.str);
  popts = js_obj_value(gc_keep(js_new_object(ctx, ctx->object_proto)));
  pdata = js_obj_value(gc_keep(js_new_object(ctx, ctx->object_proto)));
  transfer = js_obj_value(gc_keep(js_new_array(ctx, 0)));
  if (ochan.type != JV_UNDEF) {
    js_value copy = js_obj_value(gc_keep(js_new_array(ctx, 0)));
    unsigned len = ochan.type == JV_OBJ ? (unsigned)js_to_number(ctx, js_get(ctx, ochan, "length")) : 0;
    if ((int)len != nout) {
      gc_pop_scope();
      return throw_dom(ctx, "IndexSizeError", "The length of outputChannelCount must equal numberOfOutputs");
    }
    for (i = 0; i < nout; i++) {
      int ch;
      if (!count(ctx, js_get_index(ctx, ochan, (unsigned)i), &ch)) {
        gc_pop_scope();
        return js_undef();
      }
      if (ch < 1 || ch > 32) {
        gc_pop_scope();
        return throw_dom(ctx, "NotSupportedError", "An outputChannelCount entry is out of range");
      }
      js_array_push(ctx, copy.u.obj, js_number(ch));
    }
    ochan = copy;
    js_def(popts.u.obj, "outputChannelCount", ochan, PE);
  } else {
    ochan = js_obj_value(gc_keep(js_new_array(ctx, 0)));
    for (i = 0; i < nout; i++)
      js_array_push(ctx, ochan.u.obj, js_number(1));
  }
  automatic =
      ochan.type == JV_OBJ && js_get(ctx, popts, "outputChannelCount").type == JV_UNDEF && nin == 1 && nout == 1;
  np = js_array_len(reg.u.obj);
  for (i = 0; opts.type == JV_OBJ && i < (int)np; i++) {
    js_value d = js_get_index(ctx, reg, (unsigned)i), pd = js_get(ctx, opts, "parameterData"), v;
    double num;
    if (pd.type != JV_OBJ)
      break;
    v = js_get(ctx, pd, str_of(js_get(ctx, d, "name")));
    if (v.type == JV_UNDEF)
      continue;
    if (!finite(ctx, v, &num)) {
      gc_pop_scope();
      return js_undef();
    }
    js_def(pdata.u.obj, str_of(js_get(ctx, d, "name")), js_number(num), PE);
  }
  set_num(ctx, popts, "numberOfInputs", nin);
  set_num(ctx, popts, "numberOfOutputs", nout);
  js_def(popts.u.obj, "parameterData", pdata, PE);
  if (opts.type == JV_OBJ && js_get(ctx, opts, "processorOptions").type != JV_UNDEF)
    js_def(popts.u.obj, "processorOptions", js_get(ctx, opts, "processorOptions"), PE);
  id = (int)js_to_number(ctx, js_get(ctx, c, "@nodeid")) + 1;
  mc = js_construct(ctx, js_get(ctx, global_of(ctx), "MessageChannel"), 0, 0);
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  gc_keep(mc.u.obj);
  port1 = js_get(ctx, mc, "port1");
  port2 = js_get(ctx, mc, "port2");
  js_array_push(ctx, transfer.u.obj, port2);
  thread_of(ctx, c);
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  m = new_msg(ctx, "node");
  gc_keep(m.u.obj);
  set_num(ctx, m, "id", id);
  js_def(m.u.obj, "name", name, PE);
  set_num(ctx, m, "nin", nin);
  set_num(ctx, m, "nout", nout);
  js_def(m.u.obj, "ochan", ochan, PE);
  js_def(m.u.obj, "auto", js_bool(automatic), PE);
  js_def(m.u.obj, "options", popts, PE);
  js_def(m.u.obj, "pdata", pdata, PE);
  js_def(m.u.obj, "port", port2, PE);
  thread_post(ctx, c, m, transfer);
  if (ctx->has_exc) {
    gc_pop_scope();
    return js_undef();
  }
  js_set(ctx, c, "@nodeid", js_number(id));
  node = constructed_this(ctx, thisv, "wnode");
  gc_keep(node.u.obj);
  map = js_construct(ctx, js_get(ctx, global_of(ctx), "Map"), 0, 0);
  gc_keep(map.u.obj);
  map.u.obj->proto = audio_proto(ctx, "map").u.obj;
  gc_write_barrier(map.u.obj);
  js_def(node.u.obj, "@kind", js_mk_cstring(ctx, "node"), P_WRITABLE);
  js_def(node.u.obj, "@ctx", c, P_WRITABLE);
  js_def(node.u.obj, "@id", js_number(id), P_WRITABLE);
  js_def(node.u.obj, "@nin", js_number(nin), P_WRITABLE);
  js_def(node.u.obj, "@nout", js_number(nout), P_WRITABLE);
  js_def(node.u.obj, "@port", port1, P_WRITABLE);
  js_def(node.u.obj, "@map", map, P_WRITABLE);
  js_def(node.u.obj, "@cc", js_number(2), P_WRITABLE);
  js_def(node.u.obj, "@ccm", js_mk_cstring(ctx, "max"), P_WRITABLE);
  js_def(node.u.obj, "@ci", js_mk_cstring(ctx, "speakers"), P_WRITABLE);
  for (i = 0; i < (int)np; i++) {
    js_value d = js_get_index(ctx, reg, (unsigned)i), init = js_get(ctx, pdata, str_of(js_get(ctx, d, "name"))), p,
             argv[2];
    p = make_param(ctx, c, id, d, init.type == JV_NUM ? init.u.num : js_to_number(ctx, js_get(ctx, d, "defaultValue")));
    gc_keep(p.u.obj);
    argv[0] = js_get(ctx, d, "name");
    argv[1] = p;
    js_call(ctx, js_get(ctx, js_obj_value(ctx->map_proto), "set"), map, argv, 2);
  }
  snprintf(key, sizeof key, "%d", id);
  js_set(ctx, js_get(ctx, c, "@nodes"), key, node);
  gc_pop_scope();
  return node;
}

/* ---- loading ------------------------------------------------------------------------ */

static js_object *make_class(js_ctx *ctx, js_object *exports, const char *name, js_native ctor_fn, js_object *parent)
{
  js_object *ctor = (js_object *)gc_keep(js_new_native(ctx, ctor_fn, name, 0));
  js_object *proto = (js_object *)gc_keep(js_new_object(ctx, parent));
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(exports, name, js_obj_value(ctor), PE);
  return proto;
}

static void acc(js_object *proto, const char *name, js_native get, js_native set) { def_acc(proto, name, get, set); }

static js_object *proto_of(js_ctx *ctx, const char *global, const char *key)
{
  js_value v = js_get(ctx, js_get(ctx, global_of(ctx), global), key);
  return v.type == JV_OBJ ? v.u.obj : ctx->object_proto;
}

static void build(js_ctx *ctx, js_object *ex, js_object *audio)
{
  js_object *et = proto_of(ctx, "EventTarget", "prototype"), *base, *actx, *node, *dest, *wnode, *worklet, *param, *map;
  base = make_class(ctx, ex, "BaseAudioContext", illegal, et);
  actx = make_class(ctx, ex, "AudioContext", ctx_ctor, base);
  js_def(ex, "webkitAudioContext", js_get(ctx, js_obj_value(ex), "AudioContext"), PE);
  node = make_class(ctx, ex, "AudioNode", illegal, et);
  dest = make_class(ctx, ex, "AudioDestinationNode", illegal, node);
  wnode = make_class(ctx, ex, "AudioWorkletNode", wnode_ctor, node);
  worklet = make_class(ctx, ex, "AudioWorklet", illegal, ctx->object_proto);
  param = make_class(ctx, ex, "AudioParam", illegal, ctx->object_proto);
  map = make_class(ctx, ex, "AudioParamMap", illegal, ctx->map_proto);
  js_def(audio, "ctx", js_obj_value(actx), 0);
  js_def(audio, "dest", js_obj_value(dest), 0);
  js_def(audio, "wnode", js_obj_value(wnode), 0);
  js_def(audio, "worklet", js_obj_value(worklet), 0);
  js_def(audio, "param", js_obj_value(param), 0);
  js_def(audio, "map", js_obj_value(map), 0);

  acc(actx, "sampleRate", ctx_get_rate, 0);
  acc(actx, "currentTime", ctx_get_time, 0);
  acc(actx, "state", ctx_get_state, 0);
  acc(actx, "destination", ctx_get_destination, 0);
  acc(actx, "audioWorklet", ctx_get_worklet, 0);
  acc(actx, "baseLatency", ctx_get_base_latency, 0);
  acc(actx, "outputLatency", ctx_get_output_latency, 0);
  def_method(actx, "resume", ctx_resume, 0);
  def_method(actx, "suspend", ctx_suspend, 0);
  def_method(actx, "close", ctx_close, 0);

  def_method(worklet, "addModule", worklet_add_module, 1);

  acc(node, "context", node_get_context, 0);
  acc(node, "numberOfInputs", node_get_nin, 0);
  acc(node, "numberOfOutputs", node_get_nout, 0);
  acc(node, "channelCount", node_get_cc, node_set_cc);
  acc(node, "channelCountMode", node_get_ccm, node_set_ccm);
  acc(node, "channelInterpretation", node_get_ci, node_set_ci);
  def_method(node, "connect", node_connect, 1);
  def_method(node, "disconnect", node_disconnect, 0);
  acc(dest, "maxChannelCount", dest_get_max_channels, 0);
  acc(wnode, "port", wnode_get_port, 0);
  acc(wnode, "parameters", wnode_get_parameters, 0);

  acc(param, "value", param_get_value, param_set_value);
  acc(param, "defaultValue", param_get_def, 0);
  acc(param, "minValue", param_get_min, 0);
  acc(param, "maxValue", param_get_max, 0);
  acc(param, "automationRate", param_get_rate, param_set_rate);
  def_method(param, "setValueAtTime", param_set_value_at, 2);
  def_method(param, "linearRampToValueAtTime", param_linear, 2);
  def_method(param, "exponentialRampToValueAtTime", param_exponential, 2);
  def_method(param, "setTargetAtTime", param_set_target, 3);
  def_method(param, "setValueCurveAtTime", param_set_curve, 3);
  def_method(param, "cancelScheduledValues", param_cancel, 1);
  def_method(param, "cancelAndHoldAtTime", param_hold, 1);
  /* readonly maplike: the Map's mutators are hidden */
  js_def(map, "set", js_undef(), PE);
  js_def(map, "delete", js_undef(), PE);
  js_def(map, "clear", js_undef(), PE);
}

static int audio_load(js_ctx *ctx)
{
  js_object *ex, *audio;
  unsigned i;
  if (g_load_state != LOAD_NONE)
    return g_load_state == LOAD_OK;
  g_load_state = LOAD_RUNNING;
  gc_push_scope();
  ex = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  audio = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ctx->global, "@audio", js_obj_value(audio), 0);
  build(ctx, ex, audio);
  for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++)
    js_def(ctx->global, k_globals[i], js_get(ctx, js_obj_value(ex), k_globals[i]), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  g_load_state = LOAD_OK;
  return 1;
}

static js_value lazy_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = ctx->native_data;
  (void)t;
  (void)args;
  (void)argc;
  if (name.type != JV_STR || !audio_load(ctx))
    return js_undef();
  return js_get(ctx, js_obj_value(ctx->global), name.u.str->data);
}

static js_value lazy_set(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = ctx->native_data;
  (void)t;
  if (name.type == JV_STR)
    js_def(ctx->global, name.u.str->data, A(0), P_WRITABLE | P_CONFIGURABLE);
  return js_undef();
}

void js_webaudio_setup(js_ctx *ctx, js_object *win)
{
  unsigned i;
  g_load_state = LOAD_NONE;
  for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++) {
    js_object *get, *set;
    gc_push_scope();
    get = gc_keep(js_new_native(ctx, lazy_get, k_globals[i], 0));
    set = gc_keep(js_new_native(ctx, lazy_set, k_globals[i], 1));
    get->fn->data = js_mk_cstring(ctx, k_globals[i]);
    gc_write_barrier(get);
    set->fn->data = js_mk_cstring(ctx, k_globals[i]);
    gc_write_barrier(set);
    js_def_accessor(win, k_globals[i], get, set, P_CONFIGURABLE);
    gc_pop_scope();
  }
}
