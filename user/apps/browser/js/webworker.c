/* Dedicated Workers. The worker script runs in the page's single JS context inside its own scope object bound
 * as `self`; postMessage in both directions goes through a queue drained by js_workers_pump. */

#include "bindings.h"
#include "features/cors/cors.h"
#include "fetchint.h"

enum { WQ_TO_WORKER, WQ_TO_PAGE, WQ_ERROR, WQ_TO_PORT };

static js_object *g_worker_proto; /* Worker.prototype */
static js_object *g_wscope_proto; /* DedicatedWorkerGlobalScope.prototype (`self`) */
static js_object *g_wq;           /* flat queue of (worker, kind, data) triples */
static int g_next_wid;

/* Module workers run against a scope env holding their globals; kept rooted for the page's lifetime */
#define WENV_MAX 32
static js_env *g_wenvs[WENV_MAX];
static int g_nwenvs;

static js_value throw_err(js_ctx *ctx, const char *name, const char *msg)
{
  js_throw(ctx, js_make_error(ctx, ctx->type_error_proto, name, msg));
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

static js_value wclone(js_ctx *ctx, js_value v)
{
  js_value f = js_get(ctx, js_obj_value(ctx->global), "structuredClone"), r;
  if (v.type != JV_OBJ || !js_is_callable(f))
    return v;
  r = js_call(ctx, f, js_undef(), &v, 1);
  return ctx->has_exc ? v : r;
}

static void wq_push(js_value w, int kind, js_value data)
{
  gc_push_scope();
  gc_keep(w.u.obj);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  js_array_push(g_ctx, g_wq, w);
  js_array_push(g_ctx, g_wq, js_number(kind));
  js_array_push(g_ctx, g_wq, data);
  gc_pop_scope();
}

/* `onmessage = fn` inside the worker script assigns a wrapper local; mirror it onto the scope */
static void sync_onmessage(js_ctx *ctx, js_value scope)
{
  js_value get = js_get(ctx, scope, "__getom"), envi = js_get(ctx, scope, "@envi"), cur = js_undef();
  if (envi.type == JV_NUM) {
    pentry *pe = js_env_lookup(g_wenvs[(int)envi.u.num], "onmessage");
    if (pe)
      cur = pe->value;
  } else if (js_is_callable(get)) {
    cur = js_call(ctx, get, js_undef(), 0, 0);
    if (ctx->has_exc) {
      report_exc(ctx);
      return;
    }
  }
  if (js_is_callable(cur) && !js_strict_eq(cur, js_get(ctx, scope, "@lom"))) {
    js_set(ctx, scope, "onmessage", cur);
    js_set(ctx, scope, "@lom", cur);
  }
}

static int worker_id(js_ctx *ctx, js_value w)
{
  js_value id = w.type == JV_OBJ ? js_get(ctx, w, "@id") : js_undef();
  return id.type == JV_NUM ? (int)id.u.num : 0;
}

static void wq_deliver(js_value w, int kind, js_value data)
{
  js_value target, ev;
  if (w.type != JV_OBJ || js_to_boolean(js_get(g_ctx, w, "@dead")) ||
      ((kind == WQ_TO_WORKER || kind == WQ_TO_PORT) && js_to_boolean(js_get(g_ctx, w, "@closed"))))
    return;
  gc_push_scope();
  gc_keep(w.u.obj);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  target = kind == WQ_TO_WORKER ? js_get(g_ctx, w, "@scope") : w;
  if (target.type == JV_OBJ) {
    gc_keep(target.u.obj);
    if (kind == WQ_TO_WORKER)
      sync_onmessage(g_ctx, target);
    if (kind == WQ_ERROR) {
      ev = make_event("error", 0, 0, js_undef());
      if (ev.type == JV_OBJ) {
        gc_keep(ev.u.obj);
        event_adopt_iface(ev, "ErrorEvent");
        js_def(ev.u.obj, "message", data, P_WRITABLE | P_ENUMERABLE);
        js_def(ev.u.obj, "filename", js_get(g_ctx, w, "@url"), P_WRITABLE | P_ENUMERABLE);
        js_def(ev.u.obj, "lineno", js_number(0), P_WRITABLE | P_ENUMERABLE);
        js_def(ev.u.obj, "colno", js_number(0), P_WRITABLE | P_ENUMERABLE);
        js_def(ev.u.obj, "error", js_null(), P_WRITABLE | P_ENUMERABLE);
      }
    } else
      ev = make_message_event(data);
    if (ev.type == JV_OBJ) {
      gc_keep(ev.u.obj);
      js_set(g_ctx, ev, "target", target);
      g_timer_owner = kind == WQ_TO_WORKER ? worker_id(g_ctx, w) : 0;
      ev_fire_on(target.u.obj, ev, 0);
      g_timer_owner = 0;
    }
  }
  gc_pop_scope();
}

int js_workers_pump(void)
{
  js_object *old;
  unsigned n, i;
  if (!g_wq || !g_ctx || !g_page_live || !js_array_len(g_wq))
    return 0;
  gc_push_scope();
  old = (js_object *)gc_keep(g_wq);
  g_wq = js_new_array(g_ctx, 0);
  n = js_array_len(old);
  for (i = 0; i + 2 < n; i += 3) {
    js_value w = js_get_index(g_ctx, js_obj_value(old), i);
    js_value k = js_get_index(g_ctx, js_obj_value(old), i + 1);
    js_value d = js_get_index(g_ctx, js_obj_value(old), i + 2);
    wq_deliver(w, (int)k.u.num, d);
  }
  gc_pop_scope();
  return (int)(n / 3);
}

void js_port_enqueue(js_value port, js_value data) { wq_push(port, WQ_TO_PORT, data); }

static js_value worker_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)argc;
  if (t.type == JV_OBJ && !js_to_boolean(js_get(ctx, t, "@dead")) && !js_to_boolean(js_get(ctx, t, "@closed")))
    wq_push(t, WQ_TO_WORKER, wclone(ctx, A(0)));
  return js_undef();
}

static js_value worker_terminate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ) {
    js_set(ctx, t, "@dead", js_bool(1));
    timers_drop_owner(worker_id(ctx, t));
  }
  return js_undef();
}

static js_value scope_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value w;
  (void)argc;
  w = t.type == JV_OBJ ? js_get(ctx, t, "@worker") : js_undef();
  if (w.type == JV_OBJ && !js_to_boolean(js_get(ctx, w, "@dead")))
    wq_push(w, WQ_TO_PAGE, wclone(ctx, A(0)));
  return js_undef();
}

static js_value scope_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value w;
  (void)args;
  (void)argc;
  w = t.type == JV_OBJ ? js_get(ctx, t, "@worker") : js_undef();
  if (w.type == JV_OBJ) {
    js_set(ctx, w, "@closed", js_bool(1));
    timers_drop_owner(worker_id(ctx, w));
  }
  return js_undef();
}

/* The script's bytes as a NUL-terminated string, or NULL */
static char *fetch_source(const char *url)
{
  unsigned char *body;
  int len;
  char *src;
  if (starts_ci(url, "data:")) {
    char ctype[64];
    body = decode_data_url(url, &len, ctype, sizeof ctype);
  } else {
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = "GET";
    body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
    len = rs.len;
    if (body && (rs.status < 200 || rs.status >= 300)) {
      free(body);
      return 0;
    }
  }
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

static js_value scope_import_scripts(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value loc = t.type == JV_OBJ ? js_get(ctx, t, "location") : js_undef();
  js_value href = loc.type == JV_OBJ ? js_get(ctx, loc, "href") : js_undef();
  js_value gself = js_get(ctx, js_obj_value(ctx->global), "self"), ret = js_undef();
  int i;
  js_set(ctx, js_obj_value(ctx->global), "self", t); /* imported code sees the worker as `self` */
  for (i = 0; i < argc && !ctx->has_exc; i++) {
    char url[URL_MAX];
    char *code;
    js_arena *ar;
    js_node *prog;
    js_parse_error pe;
    js_value r;
    script_url(href.type == JV_STR ? href.u.str->data : g_page_url_buf, arg_str(ctx, args[i]), url, sizeof url);
    if (!starts_ci(url, "blob:") && !starts_ci(url, "data:") && !cors_same_origin(g_page_url_buf, url)) {
      ret = throw_err(ctx, "SecurityError", "importScripts: cross-origin");
      break;
    }
    code = fetch_source(url);
    if (!code) {
      ret = js_throw_type(ctx, "importScripts failed");
      break;
    }
    ar = (js_arena *)malloc(sizeof *ar);
    if (!ar) {
      free(code);
      break;
    }
    js_arena_init(ar);
    prog = js_parse(code, ar, &pe);
    free(code);
    if (!prog || !arenas_push(ar)) {
      js_arena_free(ar);
      free(ar);
      ret = throw_err(ctx, "SyntaxError", "importScripts: parse error");
      break;
    }
    js_set_cur_script(url);
    js_run(ctx, prog, &r); /* a pending exception propagates to the caller */
  }
  js_set(ctx, js_obj_value(ctx->global), "self", gself);
  return ret;
}

static char *wrap_source(const char *src)
{
  static const char pre[] = "(function(self){"
                            "var onmessage=null,onerror=null,onmessageerror=null,name=self.name,location=self."
                            "location;"
                            "self.__getom=function(){return onmessage;};"
                            "function postMessage(){return self.postMessage.apply(self,arguments);}"
                            "function close(){return self.close();}"
                            "function importScripts(){return self.importScripts.apply(self,arguments);}"
                            "function addEventListener(){return self.addEventListener.apply(self,arguments);}"
                            "function removeEventListener(){return self.removeEventListener.apply(self,arguments);}"
                            "function dispatchEvent(e){return self.dispatchEvent(e);}\n";
  static const char post[] = "\n;})";
  int pl = (int)sizeof pre - 1, sl = (int)strlen(src), ql = (int)sizeof post - 1;
  char *w = (char *)malloc((unsigned)(pl + sl + ql) + 1);
  if (!w)
    return 0;
  memcpy(w, pre, (unsigned)pl);
  memcpy(w + pl, src, (unsigned)sl);
  memcpy(w + pl + sl, post, (unsigned)ql);
  w[pl + sl + ql] = 0;
  return w;
}

/* Run the worker script with `scope` as self. False if it failed to parse or threw. */
static bool run_in_scope(js_ctx *ctx, js_object *scope, const char *src, const char *label)
{
  char *w = wrap_source(src);
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_value fn, a;
  bool ok = false;
  if (!w)
    return false;
  ar = (js_arena *)malloc(sizeof *ar);
  if (!ar) {
    free(w);
    return false;
  }
  js_arena_init(ar);
  prog = js_parse(w, ar, &pe);
  free(w);
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return false;
  }
  js_set_cur_script(label);
  gc_push_scope();
  if (js_run(ctx, prog, &fn) == 0 && js_is_callable(fn)) {
    gc_keep(fn.u.obj);
    a = js_obj_value(scope);
    js_call(ctx, fn, a, &a, 1);
    ok = !ctx->has_exc;
  }
  if (!ok && ctx->has_exc)
    report_exc(ctx);
  gc_pop_scope();
  return ok;
}

static js_value bound(js_ctx *ctx, js_value scope, const char *name)
{
  js_value fn = js_get(ctx, scope, name), bind = js_get(ctx, fn, "bind");
  return js_call(ctx, bind, fn, &scope, 1);
}

/* Run a module worker: its globals live in an env spliced under the module scope, so bare
 * `postMessage` and `onmessage = f` reach this worker instead of the window. */
static bool run_module_in_scope(js_ctx *ctx, js_object *scope, const char *src, const char *url)
{
  static const char *const fns[] = {"postMessage",         "close",        "importScripts", "addEventListener",
                                    "removeEventListener", "dispatchEvent"};
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_object *exports;
  js_env *env, *saved;
  js_value sv = js_obj_value(scope);
  unsigned i;
  bool ok;
  if (g_nwenvs == WENV_MAX)
    return false;
  ar = (js_arena *)malloc(sizeof *ar);
  if (!ar)
    return false;
  js_arena_init(ar);
  prog = js_parse(src, ar, &pe);
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return false;
  }
  gc_push_scope();
  env = (js_env *)gc_keep(js_new_env(ctx, ctx->global_env, 1));
  g_wenvs[g_nwenvs] = env;
  js_set(ctx, sv, "@envi", js_number(g_nwenvs++));
  js_env_define(env, "self", sv, P_WRITABLE);
  js_env_define(env, "name", js_get(ctx, sv, "name"), P_WRITABLE);
  js_env_define(env, "location", js_get(ctx, sv, "location"), P_WRITABLE);
  js_env_define(env, "onmessage", js_null(), P_WRITABLE);
  js_env_define(env, "onerror", js_null(), P_WRITABLE);
  js_env_define(env, "onmessageerror", js_null(), P_WRITABLE);
  for (i = 0; i < sizeof fns / sizeof *fns; i++)
    js_env_define(env, fns[i], bound(ctx, sv, fns[i]), P_WRITABLE);
  exports = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  saved = ctx->global_env;
  ctx->global_env = env;
  js_set_cur_script(url);
  ok = js_run_module(ctx, prog, exports, url) == 0;
  ctx->global_env = saved;
  if (!ok)
    report_exc(ctx);
  gc_pop_scope();
  return ok;
}

static void def_location(js_ctx *ctx, js_object *scope, const char *url)
{
  js_object *loc = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  char org[URL_MAX];
  struct purl pu;
  bool inl = starts_ci(url, "blob:") || starts_ci(url, "data:");
  cors_origin(g_page_url_buf, org, sizeof org);
  pu.host[0] = 0;
  pu.path[0] = 0;
  pu.tls = 0;
  parse_url(inl ? g_page_url_buf : url, &pu);
  js_def(loc, "href", js_mk_cstring(ctx, url), P_ENUMERABLE);
  js_def(loc, "origin", js_mk_cstring(ctx, org), P_ENUMERABLE);
  js_def(loc, "pathname", js_mk_cstring(ctx, pu.path), P_ENUMERABLE);
  js_def(loc, "protocol", js_mk_cstring(ctx, pu.tls ? "https:" : "http:"), P_ENUMERABLE);
  js_def(loc, "host", js_mk_cstring(ctx, pu.host), P_ENUMERABLE);
  js_def(loc, "hostname", js_mk_cstring(ctx, pu.host), P_ENUMERABLE);
  js_def(loc, "search", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(loc, "hash", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(scope, "location", js_obj_value(loc), P_ENUMERABLE);
}

static js_value worker_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  char url[URL_MAX];
  char *src;
  js_object *w, *scope;
  js_value opts = A(1), nm, ty, wv;
  bool module, ok;
  int prev_owner;
  (void)thisv;
  script_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  if (!starts_ci(url, "blob:") && !starts_ci(url, "data:") && !cors_same_origin(g_page_url_buf, url))
    return throw_err(ctx, "SecurityError", "Worker script must be same-origin");
  gc_push_scope();
  w = (js_object *)gc_keep(js_new_object(ctx, g_worker_proto));
  wv = js_obj_value(w);
  js_def(w, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(w, "@dead", js_bool(0), P_WRITABLE);
  js_def(w, "@url", js_mk_cstring(ctx, url), 0);
  js_def(w, "@id", js_number(++g_next_wid), 0);
  scope = (js_object *)gc_keep(js_new_object(ctx, g_wscope_proto));
  nm = opts.type == JV_OBJ ? js_get(ctx, opts, "name") : js_undef();
  ty = opts.type == JV_OBJ ? js_get(ctx, opts, "type") : js_undef();
  module = ty.type == JV_STR && streq(ty.u.str->data, "module");
  js_def(scope, "self", js_obj_value(scope), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "name", nm.type == JV_STR ? nm : js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(scope, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "@worker", wv, P_WRITABLE);
  js_def(w, "@scope", js_obj_value(scope), P_WRITABLE);
  def_location(ctx, scope, url);
  src = fetch_source(url);
  if (!src) {
    wq_push(wv, WQ_ERROR, js_mk_cstring(ctx, "Failed to load worker script"));
  } else {
    prev_owner = g_timer_owner;
    g_timer_owner = g_next_wid;
    ok = module ? run_module_in_scope(ctx, scope, src, url) : run_in_scope(ctx, scope, src, url);
    g_timer_owner = prev_owner;
    if (!ok)
      wq_push(wv, WQ_ERROR, js_mk_cstring(ctx, "Uncaught error in worker script"));
    free(src);
  }
  gc_pop_scope();
  return wv;
}

void js_worker_setup(js_ctx *ctx, js_object *win)
{
  js_object *ctor;
  g_worker_proto = js_new_object(ctx, ctx->object_proto);
  g_wscope_proto = js_new_object(ctx, ctx->object_proto);
  g_wq = js_new_array(ctx, 0);
  gc_add_root((void **)&g_worker_proto);
  gc_add_root((void **)&g_wscope_proto);
  gc_add_root((void **)&g_wq);
  g_nwenvs = 0;
  gc_add_root_range((void **)g_wenvs, &g_nwenvs);
  js_msgchannel_setup(ctx, win);
  def_method(g_worker_proto, "postMessage", worker_post, 1);
  def_method(g_worker_proto, "terminate", worker_terminate, 0);
  def_method(g_worker_proto, "addEventListener", el_add_listener, 3);
  def_method(g_worker_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_worker_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_wscope_proto, "postMessage", scope_post, 1);
  def_method(g_wscope_proto, "close", scope_close, 0);
  def_method(g_wscope_proto, "importScripts", scope_import_scripts, 1);
  def_method(g_wscope_proto, "addEventListener", el_add_listener, 3);
  def_method(g_wscope_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_wscope_proto, "dispatchEvent", el_dispatch, 1);
  ctor = js_new_native(ctx, worker_ctor, "Worker", 1);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(g_worker_proto), 0);
  js_def(g_worker_proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "Worker", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

void js_worker_teardown(void)
{
  js_msgchannel_teardown();
  gc_remove_root((void **)&g_worker_proto);
  gc_remove_root((void **)&g_wscope_proto);
  gc_remove_root((void **)&g_wq);
  gc_remove_root_range((void **)g_wenvs);
  g_nwenvs = 0;
  g_worker_proto = g_wscope_proto = g_wq = 0;
}
