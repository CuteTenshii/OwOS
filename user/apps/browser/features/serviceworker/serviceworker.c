/* js_serviceworker.c — Service Workers + the Cache API for the browser.
 *
 * Registrations and the Cache store are PROCESS-persistent C state (plain
 * malloc, not the JS heap) so a worker registered on one page controls the next
 * and a cached response survives a reload; the JS-side prototypes are
 * per-navigation, rebuilt by sw_install and dropped by sw_teardown.
 * sw_intercept_fetch lets the core fetch() consult a controlling worker. Split
 * out of js.c; shares the page engine context and the DOM/fetch/notification
 * helpers via jsint.h. */
#include <stdbool.h>

#include "features/cors/cors.h" /* cors_origin / cors_same_origin: SW scope + cache keys */
#include "jsint.h"
#include "notify.h" /* notify_send: a service worker's showNotification()    */

/* ------------------------------------------------------- service workers --- *
 * Registrations and the Cache API store are PROCESS-persistent C state (plain
 * malloc, not the JS heap): they must outlive the per-navigation js_ctx so a SW
 * registered on one page still controls the next, and a cached response
 * survives a reload — the whole point of offline support. Each new page
 * (setup_dom) checks this store, re-instantiates the controlling worker into
 * the fresh context, and rebuilds the navigator.serviceWorker / caches surface
 * over it. The JS-side prototype objects below are per-context (rebuilt +
 * re-rooted every navigation, reclaimed by sw_teardown()). See the "SERVICE
 * WORKERS" section for the model. */

/* One cached (request -> response). Bodies are raw bytes so any content
 * survives. */
struct sw_c_entry {
  char *method, *url, *statustext, *ctype;
  unsigned char *body;
  int blen, status;
};
/* A named Cache, scoped to an origin (CacheStorage is per-origin per spec). */
struct sw_cache {
  int id; /* stable across the process (JS wrapper key) */
  char *origin, *name;
  struct sw_c_entry *e;
  int n, cap;
};
static struct sw_cache **g_swc; /* every open Cache, all origins             */
static int g_swc_n, g_swc_cap, g_swc_nextid = 1;

/* One service-worker registration: its scope, script URL and the script SOURCE
 * (kept so the worker can be re-run in each new context without a re-fetch). */
struct sw_reg {
  char *origin, *scope, *script_url, *src;
  int slen;
  bool installed; /* install has fired once for this src        */
};
static struct sw_reg **g_swr;
static int g_swr_n, g_swr_cap;

/* Per-context (per-navigation) JS objects — rebuilt + rooted in sw_install,
 * dropped in sw_teardown. */
static js_object *g_swcontainer_proto;  /* navigator.serviceWorker (an EventTarget)   */
static js_object *g_swreg_proto;        /* ServiceWorkerRegistration.prototype */
static js_object *g_sw_proto;           /* ServiceWorker.prototype (page-side handle)  */
static js_object *g_swscope_proto;      /* ServiceWorkerGlobalScope.prototype (`self`) */
static js_object *g_clients_proto;      /* Clients.prototype (self.clients) */
static js_object *g_client_proto;       /* Client.prototype (a controlled page) */
static js_object *g_cachestorage_proto; /* CacheStorage.prototype (the `caches` global)*/
static js_object *g_cache_proto;        /* Cache.prototype */
static js_object *g_request_proto;      /* Request.prototype */
static js_object *g_sw_container;       /* the live navigator.serviceWorker object */
static js_object *g_sw_active_scope;    /* the controlling worker's global scope, or 0 */
static js_object *g_sw_exec_holder;     /* transient: receives a Promise's resolvers   */
static int g_sw_in_fetch;               /* dispatching a fetch event: don't re-enter   */

/* ======================================================= SERVICE WORKERS === *
 * A working Service Worker implementation over the single-context engine.
 *
 * The engine keeps one live js_ctx (torn down per navigation) and settles
 * promises synchronously, so a worker cannot run in a separate realm or thread.
 * Instead the worker script runs in THIS context inside its own function scope
 * — a ServiceWorkerGlobalScope object bound as `self` — with caches/clients/
 * registration/skipWaiting/addEventListener/fetch shadowed to the scope, while
 * everything else (Promise/Response/JSON/…) resolves up the scope chain to the
 * window global. Registrations and the Cache store are persistent C state, so a
 * worker registered on one page controls the next and cached bodies survive a
 * reload — real offline support. fetch() interception, install/activate,
 * respondWith/waitUntil, client<->worker postMessage and the Cache API are all
 * real; every step settles synchronously, exactly as fetch/XHR/WebSocket do. */

static js_value sw_resolve(js_ctx *ctx, js_value v);
static js_value sw_make_request(js_ctx *ctx, const char *url, const char *method);
static js_value sw_make_cachestorage(js_ctx *ctx);
static js_value sw_make_clients(js_ctx *ctx);
static js_value sw_make_client(js_ctx *ctx);
static js_value sw_make_worker(js_ctx *ctx, const char *script, const char *state);
static js_value sw_make_registration(js_ctx *ctx, struct sw_reg *reg, js_value worker);
static js_value sw_instantiate(js_ctx *ctx, struct sw_reg *reg, bool do_install, bool do_activate);
static void sw_set_controller(js_ctx *ctx, js_value worker);
static void sw_fire_lifecycle(js_ctx *ctx, js_object *scope, const char *type);
static bool sw_run_scope(js_ctx *ctx, js_object *scope, const char *src, const char *label);
static struct sw_reg *sw_reg_for_url(const char *page_url);
static void sw_input_url(js_ctx *ctx, js_value v, char *out, int cap);
static void sw_input_method(js_ctx *ctx, js_value v, char *out, int cap);
static js_value sw_new_pending(js_ctx *ctx, js_value *resolve_out);

/* ---- small utilities ---- */
static char *sw_strdup(const char *s)
{
  int n = s ? (int)strlen(s) : 0;
  char *d = (char *)malloc((unsigned)n + 1);
  if (d) {
    if (s && n)
      memcpy(d, s, (unsigned)n);
    d[n] = 0;
  }
  return d;
}
/* A resolved Promise of `v`, keeping a freshly-built object alive across the
 * promise allocation (the common return shape for the async-looking SW
 * methods). */
static js_value sw_resolve(js_ctx *ctx, js_value v)
{
  js_value rv;
  gc_push_scope();
  if (v.type == JV_OBJ)
    gc_keep(v.u.obj);
  rv = js_promise_resolved(ctx, v);
  gc_pop_scope();
  return rv;
}

/* ---- persistent C store: caches ---- */
static void sw_entry_free(struct sw_c_entry *e)
{
  free(e->method);
  free(e->url);
  free(e->statustext);
  free(e->ctype);
  free(e->body);
}
static struct sw_cache *sw_cache_find(const char *origin, const char *name, bool create)
{
  int i;
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, origin) && streq(g_swc[i]->name, name))
      return g_swc[i];
  if (!create)
    return 0;
  if (g_swc_n == g_swc_cap) {
    int nc = g_swc_cap ? g_swc_cap * 2 : 8;
    struct sw_cache **na = (struct sw_cache **)realloc(g_swc, (unsigned)nc * sizeof *na);
    if (!na)
      return 0;
    g_swc = na;
    g_swc_cap = nc;
  }
  {
    struct sw_cache *c = (struct sw_cache *)calloc(1, sizeof *c);
    if (!c)
      return 0;
    c->id = g_swc_nextid++;
    c->origin = sw_strdup(origin);
    c->name = sw_strdup(name);
    if (!c->origin || !c->name) {
      free(c->origin);
      free(c->name);
      free(c);
      return 0;
    }
    g_swc[g_swc_n++] = c;
    return c;
  }
}
static struct sw_cache *sw_cache_by_id(int id)
{
  int i;
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && g_swc[i]->id == id)
      return g_swc[i];
  return 0;
}
static int sw_cache_index(struct sw_cache *c, const char *url)
{
  int i;
  for (i = 0; i < c->n; i++)
    if (c->e[i].url && streq(c->e[i].url, url))
      return i;
  return -1;
}
static void sw_cache_store(struct sw_cache *c, const char *method, const char *url, int status, const char *st,
                           const char *ct, const char *body, int blen)
{
  struct sw_c_entry *e;
  int i = sw_cache_index(c, url);
  if (i >= 0) {
    e = &c->e[i];
    sw_entry_free(e);
  } else {
    if (c->n == c->cap) {
      int nc = c->cap ? c->cap * 2 : 8;
      struct sw_c_entry *na = (struct sw_c_entry *)realloc(c->e, (unsigned)nc * sizeof *na);
      if (!na)
        return;
      c->e = na;
      c->cap = nc;
    }
    e = &c->e[c->n++];
  }
  memset(e, 0, sizeof *e);
  e->method = sw_strdup(method && method[0] ? method : "GET");
  e->url = sw_strdup(url);
  e->status = status ? status : 200;
  e->statustext = sw_strdup(st ? st : "");
  e->ctype = sw_strdup(ct ? ct : "");
  e->blen = blen > 0 ? blen : 0;
  e->body = (unsigned char *)malloc((unsigned)(e->blen ? e->blen : 1));
  if (e->body && e->blen)
    memcpy(e->body, body, (unsigned)e->blen);
}

/* ---- persistent C store: registrations ---- */
static struct sw_reg *sw_reg_find_scope(const char *origin, const char *scope)
{
  int i;
  for (i = 0; i < g_swr_n; i++)
    if (g_swr[i] && streq(g_swr[i]->origin, origin) && streq(g_swr[i]->scope, scope))
      return g_swr[i];
  return 0;
}
static void sw_reg_set_src(struct sw_reg *r, const char *b, int n)
{
  char *src = (char *)malloc((unsigned)(n > 0 ? n : 0) + 1);
  if (src) {
    if (n > 0)
      memcpy(src, b, (unsigned)n);
    src[n > 0 ? n : 0] = 0;
    free(r->src);
    r->src = src;
    r->slen = n > 0 ? n : 0;
  }
}
static struct sw_reg *sw_reg_add(const char *origin, const char *scope, const char *script_url)
{
  struct sw_reg *r = sw_reg_find_scope(origin, scope);
  if (r) {
    char *url = sw_strdup(script_url);
    if (!url)
      return 0;
    free(r->script_url);
    r->script_url = url;
    return r;
  }
  if (g_swr_n == g_swr_cap) {
    int nc = g_swr_cap ? g_swr_cap * 2 : 8;
    struct sw_reg **na = (struct sw_reg **)realloc(g_swr, (unsigned)nc * sizeof *na);
    if (!na)
      return 0;
    g_swr = na;
    g_swr_cap = nc;
  }
  r = (struct sw_reg *)calloc(1, sizeof *r);
  if (!r)
    return 0;
  r->origin = sw_strdup(origin);
  r->scope = sw_strdup(scope);
  r->script_url = sw_strdup(script_url);
  if (!r->origin || !r->scope || !r->script_url) {
    free(r->origin);
    free(r->scope);
    free(r->script_url);
    free(r);
    return 0;
  }
  g_swr[g_swr_n++] = r;
  return r;
}
static bool sw_reg_remove(const char *origin, const char *scope)
{
  int i;
  for (i = 0; i < g_swr_n; i++)
    if (g_swr[i] && streq(g_swr[i]->origin, origin) && streq(g_swr[i]->scope, scope)) {
      free(g_swr[i]->origin);
      free(g_swr[i]->scope);
      free(g_swr[i]->script_url);
      free(g_swr[i]->src);
      free(g_swr[i]);
      g_swr[i] = g_swr[--g_swr_n];
      return true;
    }
  return false;
}
/* The registration (if any) that controls `page_url`: same origin and the
 * longest scope path that is a prefix of the page's path. */
static struct sw_reg *sw_reg_for_url(const char *page_url)
{
  char org[URL_MAX];
  struct purl pp;
  struct sw_reg *best = 0;
  int bestlen = -1, i;
  cors_origin(page_url, org, sizeof org);
  if (!parse_url(page_url, &pp))
    return 0;
  for (i = 0; i < g_swr_n; i++) {
    struct sw_reg *r = g_swr[i];
    struct purl ps;
    int sl;
    if (!r || !streq(r->origin, org))
      continue;
    if (!parse_url(r->scope, &ps))
      continue;
    sl = (int)strlen(ps.path);
    if (memcmp(pp.path, ps.path, (unsigned)sl) == 0 && sl > bestlen) {
      best = r;
      bestlen = sl;
    }
  }
  return best;
}
/* Default scope = the directory of the script URL (through its last '/'). */
static void sw_default_scope(const char *script, char *out, int cap)
{
  int i, last = -1, n;
  strlcpy(out, script, cap);
  n = (int)strlen(out);
  for (i = 0; i < n; i++)
    if (out[i] == '?' || out[i] == '#') {
      out[i] = 0;
      n = i;
      break;
    }
  for (i = 0; i < n; i++)
    if (out[i] == '/')
      last = i;
  if (last >= 0)
    out[last + 1] = 0;
}

/* ---- Request / Response objects ---- */
static void sw_input_url(js_ctx *ctx, js_value v, char *out, int cap)
{
  const char *s;
  if (v.type == JV_OBJ) {
    js_value u = js_get(ctx, v, "url");
    s = (u.type == JV_STR) ? u.u.str->data : arg_str(ctx, v);
  } else
    s = arg_str(ctx, v);
  resolve_url(g_page_url_buf, s, out, cap);
}
static void sw_input_method(js_ctx *ctx, js_value v, char *out, int cap)
{
  if (v.type == JV_OBJ) {
    js_value m = js_get(ctx, v, "method");
    if (m.type == JV_STR) {
      strlcpy(out, m.u.str->data, cap);
      return;
    }
  }
  strlcpy(out, "GET", cap);
}
static js_value sw_make_request(js_ctx *ctx, const char *url, const char *method)
{
  js_object *o, *h;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_request_proto));
  js_def(o, "url", js_mk_cstring(ctx, url ? url : ""), P_ENUMERABLE);
  js_def(o, "method", js_mk_cstring(ctx, method && method[0] ? method : "GET"), P_ENUMERABLE);
  js_def(o, "mode", js_mk_cstring(ctx, "cors"), P_ENUMERABLE);
  js_def(o, "credentials", js_mk_cstring(ctx, "same-origin"), P_ENUMERABLE);
  js_def(o, "cache", js_mk_cstring(ctx, "default"), P_ENUMERABLE);
  js_def(o, "redirect", js_mk_cstring(ctx, "follow"), P_ENUMERABLE);
  js_def(o, "referrer", js_mk_cstring(ctx, "about:client"), P_ENUMERABLE);
  js_def(o, "destination", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  h = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(h, "@ctype", js_mk_cstring(ctx, ""), 0);
  js_def_native(ctx, h, "get", hdr_get, 1);
  js_def_native(ctx, h, "has", hdr_has, 1);
  js_def(o, "headers", js_obj_value(h), P_ENUMERABLE);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}
static js_value req_clone(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char m[16];
  js_value u = js_get(ctx, t, "url"), mv = js_get(ctx, t, "method");
  (void)args;
  (void)argc;
  strlcpy(m, mv.type == JV_STR ? mv.u.str->data : "GET", sizeof m);
  return sw_make_request(ctx, u.type == JV_STR ? u.u.str->data : "", m);
}
static js_value request_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  char url[URL_MAX], method[16];
  js_value in = A(0);
  const char *s;
  (void)thisv;
  method[0] = 0;
  if (in.type == JV_OBJ) {
    js_value u = js_get(ctx, in, "url"), m = js_get(ctx, in, "method");
    s = (u.type == JV_STR) ? u.u.str->data : arg_str(ctx, in);
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
  } else
    s = arg_str(ctx, in);
  resolve_url(g_page_url_buf, s, url, sizeof url);
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value m = js_get(ctx, A(1), "method");
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
  }
  return sw_make_request(ctx, url, method);
}
static js_value response_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  int status = 200;
  char st[64], ct[160];
  js_value body = A(0), rv;
  (void)thisv;
  strlcpy(st, "", sizeof st);
  strlcpy(ct, "text/plain;charset=UTF-8", sizeof ct);
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value s = js_get(ctx, A(1), "status");
    js_value stt = js_get(ctx, A(1), "statusText");
    js_value h = js_get(ctx, A(1), "headers");
    if (s.type == JV_NUM)
      status = (int)s.u.num;
    if (stt.type == JV_STR)
      strlcpy(st, stt.u.str->data, sizeof st);
    if (h.type == JV_OBJ) {
      js_value c = js_get(ctx, h, "content-type");
      js_value cc = js_get(ctx, h, "@ctype");
      if (c.type == JV_STR)
        strlcpy(ct, c.u.str->data, sizeof ct);
      if (cc.type == JV_STR && cc.u.str->len)
        strlcpy(ct, cc.u.str->data, sizeof ct);
    }
  }
  gc_push_scope();
  {
    js_value bstr = (body.type == JV_STR)                             ? body
                    : (body.type == JV_UNDEF || body.type == JV_NULL) ? js_mk_cstring(ctx, "")
                                                                      : js_to_string(ctx, body);
    js_object *resp;
    if (bstr.type == JV_STR)
      gc_keep(bstr.u.str);
    resp = (js_object *)gc_keep(make_response(ctx, "", status, st, ct, bstr.type == JV_STR ? bstr.u.str->data : "",
                                              bstr.type == JV_STR ? bstr.u.str->len : 0));
    rv = js_obj_value(resp);
  }
  gc_pop_scope();
  return rv;
}

/* ---- Cache ---- */
static struct sw_cache *cache_of(js_ctx *ctx, js_value t)
{
  js_value id = js_get(ctx, t, "@cid");
  return id.type == JV_NUM ? sw_cache_by_id((int)id.u.num) : 0;
}
static js_value cache_match(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  char url[URL_MAX];
  int i;
  (void)args;
  (void)argc;
  if (!c)
    return sw_resolve(ctx, js_undef());
  sw_input_url(ctx, A(0), url, sizeof url);
  i = sw_cache_index(c, url);
  if (i < 0)
    return sw_resolve(ctx, js_undef());
  {
    struct sw_c_entry *e = &c->e[i];
    js_object *r = make_response(ctx, e->url, e->status, e->statustext, e->ctype, (const char *)e->body, e->blen);
    return sw_resolve(ctx, js_obj_value(r));
  }
}
static js_value cache_matchall(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  char url[URL_MAX];
  js_object *arr;
  js_value rv;
  int i;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (c) {
    sw_input_url(ctx, A(0), url, sizeof url);
    i = sw_cache_index(c, url);
    if (i >= 0) {
      struct sw_c_entry *e = &c->e[i];
      js_array_push(
          ctx, arr,
          js_obj_value(make_response(ctx, e->url, e->status, e->statustext, e->ctype, (const char *)e->body, e->blen)));
    }
  }
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value cache_put(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  char url[URL_MAX], method[16];
  js_value resp = A(1), settled;
  int st;
  (void)argc;
  if (!c)
    return sw_resolve(ctx, js_undef());
  sw_input_url(ctx, A(0), url, sizeof url);
  sw_input_method(ctx, A(0), method, sizeof method);
  st = js_promise_inspect(ctx, resp, &settled);
  if (st == 1)
    resp = settled;
  else if (st == 0 || st == 2)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.put: response not available"));
  if (resp.type != JV_OBJ)
    return js_promise_rejected(ctx,
                               js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.put: not a Response"));
  {
    js_value b = js_get(ctx, resp, "@body");
    js_value sv = js_get(ctx, resp, "status");
    js_value sx = js_get(ctx, resp, "statusText");
    js_value h = js_get(ctx, resp, "headers");
    const char *ct = "";
    if (h.type == JV_OBJ) {
      js_value cc = js_get(ctx, h, "@ctype");
      if (cc.type == JV_STR)
        ct = cc.u.str->data;
    }
    sw_cache_store(c, method, url, sv.type == JV_NUM ? (int)sv.u.num : 200, sx.type == JV_STR ? sx.u.str->data : "", ct,
                   b.type == JV_STR ? b.u.str->data : "", b.type == JV_STR ? b.u.str->len : 0);
  }
  return sw_resolve(ctx, js_undef());
}
static js_value cache_add(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  char url[URL_MAX], method[16];
  struct http_req rq = {0};
  struct http_resp rs = {0};
  unsigned char *body;
  (void)argc;
  if (!c)
    return sw_resolve(ctx, js_undef());
  sw_input_url(ctx, A(0), url, sizeof url);
  sw_input_method(ctx, A(0), method, sizeof method);
  rq.url = url;
  rq.method = method[0] ? method : "GET";
  body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
  if (!body || rs.status < 200 || rs.status >= 300) {
    if (body)
      free(body);
    return js_promise_rejected(ctx,
                               js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.add: request failed"));
  }
  sw_cache_store(c, method, url, rs.status, rs.status == 200 ? "OK" : "", rs.ctype, (const char *)body, rs.len);
  free(body);
  return sw_resolve(ctx, js_undef());
}
static js_value cache_addall(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  js_value list = A(0);
  unsigned n, i;
  (void)argc;
  if (!c || list.type != JV_OBJ)
    return sw_resolve(ctx, js_undef());
  n = js_array_len(list.u.obj);
  for (i = 0; i < n; i++) {
    char url[URL_MAX], method[16];
    js_value it = js_get_index(ctx, list, i);
    struct http_req rq = {0};
    struct http_resp rs = {0};
    unsigned char *body;
    sw_input_url(ctx, it, url, sizeof url);
    sw_input_method(ctx, it, method, sizeof method);
    rq.url = url;
    rq.method = method[0] ? method : "GET";
    body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
    if (!body || rs.status < 200 || rs.status >= 300) {
      if (body)
        free(body);
      return js_promise_rejected(
          ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.addAll: request failed"));
    }
    sw_cache_store(c, method, url, rs.status, rs.status == 200 ? "OK" : "", rs.ctype, (const char *)body, rs.len);
    free(body);
  }
  return sw_resolve(ctx, js_undef());
}
static js_value cache_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  char url[URL_MAX];
  int i;
  (void)args;
  (void)argc;
  if (!c)
    return sw_resolve(ctx, js_bool(0));
  sw_input_url(ctx, A(0), url, sizeof url);
  i = sw_cache_index(c, url);
  if (i < 0)
    return sw_resolve(ctx, js_bool(0));
  sw_entry_free(&c->e[i]);
  c->e[i] = c->e[--c->n];
  return sw_resolve(ctx, js_bool(1));
}
static js_value cache_keys(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c = cache_of(ctx, t);
  js_object *arr;
  js_value rv;
  int i;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (c)
    for (i = 0; i < c->n; i++)
      js_array_push(ctx, arr, sw_make_request(ctx, c->e[i].url, c->e[i].method));
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value sw_make_cache_wrapper(js_ctx *ctx, int id)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_cache_proto));
  js_def(o, "@cid", js_number(id), 0);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- CacheStorage (the `caches` global) ---- */
static js_value caches_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  struct sw_cache *c;
  (void)t;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  c = sw_cache_find(org, arg_str(ctx, A(0)), true);
  if (!c)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "Error", "caches.open failed"));
  return sw_resolve(ctx, sw_make_cache_wrapper(ctx, c->id));
}
static js_value caches_match(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX], url[URL_MAX];
  int i;
  (void)t;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  sw_input_url(ctx, A(0), url, sizeof url);
  for (i = 0; i < g_swc_n; i++) {
    struct sw_cache *c = g_swc[i];
    int k;
    if (!c || !streq(c->origin, org))
      continue;
    k = sw_cache_index(c, url);
    if (k >= 0) {
      struct sw_c_entry *e = &c->e[k];
      js_object *r = make_response(ctx, e->url, e->status, e->statustext, e->ctype, (const char *)e->body, e->blen);
      return sw_resolve(ctx, js_obj_value(r));
    }
  }
  return sw_resolve(ctx, js_undef());
}
static js_value caches_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  (void)t;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  return sw_resolve(ctx, js_bool(sw_cache_find(org, arg_str(ctx, A(0)), false) != 0));
}
static js_value caches_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  const char *name;
  int i;
  (void)t;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  name = arg_str(ctx, A(0));
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, org) && streq(g_swc[i]->name, name)) {
      struct sw_cache *c = g_swc[i];
      int k;
      for (k = 0; k < c->n; k++)
        sw_entry_free(&c->e[k]);
      free(c->e);
      free(c->origin);
      free(c->name);
      free(c);
      g_swc[i] = g_swc[--g_swc_n];
      return sw_resolve(ctx, js_bool(1));
    }
  return sw_resolve(ctx, js_bool(0));
}
static js_value caches_keys(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  js_object *arr;
  js_value rv;
  int i;
  (void)t;
  (void)args;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, org))
      js_array_push(ctx, arr, js_mk_cstring(ctx, g_swc[i]->name));
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value sw_make_cachestorage(js_ctx *ctx)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_cachestorage_proto));
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- Clients / Client (SW <-> page messaging) ---- */
static void sw_deliver_message(js_ctx *ctx, js_object *target, js_value data, js_value source)
{
  js_value ev;
  if (!target)
    return;
  gc_push_scope();
  gc_keep(target);
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  if (source.type == JV_OBJ)
    gc_keep(source.u.obj);
  ev = make_message_event(data);
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "source", source);
    js_set(ctx, ev, "target", js_obj_value(target));
    js_set(ctx, ev, "currentTarget", js_obj_value(target));
    ev_fire_on(target, ev, 0);
  }
  gc_pop_scope();
}
static js_value sw_make_client(js_ctx *ctx)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_client_proto));
  js_def(o, "id", js_mk_cstring(ctx, "owos-client-0"), P_ENUMERABLE);
  js_def(o, "url", js_mk_cstring(ctx, g_page_url_buf), P_ENUMERABLE);
  js_def(o, "type", js_mk_cstring(ctx, "window"), P_ENUMERABLE);
  js_def(o, "frameType", js_mk_cstring(ctx, "top-level"), P_ENUMERABLE);
  js_def(o, "focused", js_bool(1), P_ENUMERABLE);
  js_def(o, "visibilityState", js_mk_cstring(ctx, "visible"), P_ENUMERABLE);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}
static js_value client_postmessage(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value src = g_sw_active_scope ? js_get(ctx, js_obj_value(g_sw_active_scope), "@worker") : js_null();
  (void)t;
  (void)argc;
  if (g_sw_container)
    sw_deliver_message(ctx, g_sw_container, A(0), src);
  return js_undef();
}
static js_value clients_matchall(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, arr, sw_make_client(ctx));
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value clients_claim(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  if (g_sw_active_scope)
    sw_set_controller(ctx, js_get(ctx, js_obj_value(g_sw_active_scope), "@worker"));
  return sw_resolve(ctx, js_undef());
}
static js_value clients_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  if (streq(arg_str(ctx, A(0)), "owos-client-0"))
    return sw_resolve(ctx, sw_make_client(ctx));
  return sw_resolve(ctx, js_undef());
}
static js_value clients_openwindow(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return sw_resolve(ctx, js_null());
}
static js_value sw_make_clients(js_ctx *ctx)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_clients_proto));
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- ServiceWorker (page-side handle) ---- */
static js_value worker_postmessage(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  if (g_sw_active_scope)
    sw_deliver_message(ctx, g_sw_active_scope, A(0), sw_make_client(ctx));
  return js_undef();
}
static js_value sw_make_worker(js_ctx *ctx, const char *script, const char *state)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_sw_proto));
  js_def(o, "scriptURL", js_mk_cstring(ctx, script), P_ENUMERABLE);
  js_def(o, "state", js_mk_cstring(ctx, state), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- ServiceWorkerRegistration (page-side) ---- */
static js_value reg_unregister(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  js_value sc = js_get(ctx, t, "@scope");
  bool removed;
  (void)args;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  removed = sw_reg_remove(org, sc.type == JV_STR ? sc.u.str->data : "");
  if (removed) {
    g_sw_active_scope = 0;
    if (g_sw_container) {
      js_def(g_sw_container, "controller", js_null(), P_WRITABLE | P_ENUMERABLE);
      js_delete(g_sw_container, "@reg");
    }
  }
  return sw_resolve(ctx, js_bool(removed));
}
static js_value reg_update(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  js_value sc = js_get(ctx, t, "@scope");
  struct sw_reg *r;
  struct http_req rq = {0};
  struct http_resp rs = {0};
  unsigned char *body;
  (void)args;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  r = sw_reg_find_scope(org, sc.type == JV_STR ? sc.u.str->data : "");
  if (!r)
    return sw_resolve(ctx, t);
  rq.url = r->script_url;
  rq.method = "GET";
  body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
  if (body) {
    bool changed = !(r->src && r->slen == rs.len && memcmp(r->src, body, (unsigned)rs.len) == 0);
    if (changed) {
      sw_reg_set_src(r, (char *)body, rs.len);
      r->installed = false;
    }
    free(body);
    if (changed) {
      js_value nr = sw_instantiate(ctx, r, true, true);
      if (nr.type == JV_OBJ)
        return sw_resolve(ctx, nr);
    }
  }
  return sw_resolve(ctx, t);
}
static js_value reg_show_notification(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *title = arg_str(ctx, A(0));
  const char *bodys = "";
  char app[64];
  (void)t;
  (void)argc;
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value b = js_get(ctx, A(1), "body");
    if (b.type == JV_STR)
      bodys = b.u.str->data;
  }
  if (perm_state_now(PERM_NOTIFICATIONS) == PERM_GRANTED && title[0]) {
    struct notification n = {0};
    notif_app_name(app, sizeof app);
    n.app = app;
    n.title = title;
    n.body = bodys[0] ? bodys : 0;
    n.image = 0;
    n.nbuttons = 0;
    notify_send(&n);
  }
  return sw_resolve(ctx, js_undef());
}
static js_value reg_get_notifications(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value sw_make_registration(js_ctx *ctx, struct sw_reg *reg, js_value worker)
{
  js_object *o;
  js_value rv;
  gc_push_scope();
  if (worker.type == JV_OBJ)
    gc_keep(worker.u.obj);
  o = (js_object *)gc_keep(js_new_object(ctx, g_swreg_proto));
  js_def(o, "scope", js_mk_cstring(ctx, reg->scope), P_ENUMERABLE);
  js_def(o, "active", worker, P_WRITABLE | P_ENUMERABLE);
  js_def(o, "installing", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "waiting", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "updateViaCache", js_mk_cstring(ctx, "imports"), P_ENUMERABLE);
  js_def(o, "onupdatefound", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "@scope", js_mk_cstring(ctx, reg->scope), 0);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* ---- lifecycle + FetchEvent ---- */
static js_value ee_wait_until(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  /* Every promise settles synchronously here, so waitUntil has nothing to
   * extend; it exists so handlers that call it don't throw. */
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
static js_value fe_respond_with(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = A(0), settled;
  int st;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_undef();
  st = js_promise_inspect(ctx, v, &settled);
  if (st == 1)
    v = settled; /* fulfilled promise -> its value */
  else if (st == 2) {
    js_set(ctx, t, "@responderr", js_bool(1));
    js_set(ctx, t, "@responded", js_bool(1));
    return js_undef();
  } else if (st == 0)
    return js_undef(); /* pending: can't await -> network fallback */
  if (v.type == JV_OBJ)
    js_set(ctx, t, "@resp", v);
  js_set(ctx, t, "@responded", js_bool(1));
  return js_undef();
}
static void sw_fire_lifecycle(js_ctx *ctx, js_object *scope, const char *type)
{
  js_value ev;
  gc_push_scope();
  gc_keep(scope);
  ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_def(ev.u.obj, "@responded", js_bool(0), P_WRITABLE);
    js_def_native(ctx, ev.u.obj, "waitUntil", ee_wait_until, 1);
    js_set(ctx, ev, "target", js_obj_value(scope));
    ev_fire_on(scope, ev, 0);
  }
  gc_pop_scope();
}
static bool sw_has_listener(js_object *scope, const char *type)
{
  char onname[24];
  pentry *pe;
  if (!scope)
    return false;
  pe = pmap_find_pub(scope, "@ev");
  if (pe && pe->value.type == JV_OBJ) {
    js_object *list = pe->value.u.obj;
    unsigned i, n = js_array_len(list);
    for (i = 0; i < n; i++) {
      js_value e = js_get_index(g_ctx, js_obj_value(list), i);
      js_value ty;
      if (e.type != JV_OBJ)
        continue;
      ty = js_get(g_ctx, e, "type");
      if (ty.type == JV_STR && streq(ty.u.str->data, type))
        return true;
    }
  }
  onname[0] = 'o';
  onname[1] = 'n';
  strlcpy(onname + 2, type, (int)sizeof onname - 2);
  return js_is_callable(js_get(g_ctx, js_obj_value(scope), onname));
}
int sw_intercept_fetch(js_ctx *ctx, const char *url, const char *method, js_value *out)
{
  js_value ev, r;
  bool handled = false;
  if (!g_sw_active_scope || g_sw_in_fetch)
    return 0;
  if (!sw_has_listener(g_sw_active_scope, "fetch"))
    return 0;
  g_sw_in_fetch = 1;
  gc_push_scope();
  ev = make_event("fetch", 0, 0, js_undef());
  if (ev.type != JV_OBJ) {
    gc_pop_scope();
    g_sw_in_fetch = 0;
    return 0;
  }
  gc_keep(ev.u.obj);
  {
    js_value rq = sw_make_request(ctx, url, method);
    if (rq.type == JV_OBJ)
      gc_keep(rq.u.obj);
    js_def(ev.u.obj, "request", rq, P_ENUMERABLE);
  }
  js_def(ev.u.obj, "clientId", js_mk_cstring(ctx, "owos-client-0"), P_ENUMERABLE);
  js_def(ev.u.obj, "resultingClientId", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(ev.u.obj, "isReload", js_bool(0), P_ENUMERABLE);
  js_def(ev.u.obj, "preloadResponse", js_promise_resolved(ctx, js_undef()), P_ENUMERABLE);
  js_def(ev.u.obj, "@responded", js_bool(0), P_WRITABLE);
  js_def(ev.u.obj, "@resp", js_undef(), P_WRITABLE);
  js_def_native(ctx, ev.u.obj, "respondWith", fe_respond_with, 1);
  js_def_native(ctx, ev.u.obj, "waitUntil", ee_wait_until, 1);
  js_set(ctx, ev, "target", js_obj_value(g_sw_active_scope));
  ev_fire_on(g_sw_active_scope, ev, 0);
  if (js_to_boolean(js_get(ctx, ev, "@responded"))) {
    if (js_to_boolean(js_get(ctx, ev, "@responderr"))) {
      *out = js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Failed to fetch"));
      handled = true;
    } else {
      r = js_get(ctx, ev, "@resp");
      if (r.type == JV_OBJ) {
        gc_keep(r.u.obj);
        *out = js_promise_resolved(ctx, r);
        handled = true;
      }
    }
  }
  gc_pop_scope();
  g_sw_in_fetch = 0;
  return handled;
}

/* ---- running the worker script in its own scope ---- */
static char *sw_wrap_src(const char *src)
{
  static const char pre[] = "(function(self){"
                            "var "
                            "caches=self.caches,clients=self.clients,registration=self.registration,"
                            "location=self."
                            "location;"
                            "function addEventListener(){return "
                            "self.addEventListener.apply(self,arguments);}"
                            "function removeEventListener(){return "
                            "self.removeEventListener.apply(self,arguments);}"
                            "function dispatchEvent(e){return self.dispatchEvent(e);}"
                            "function skipWaiting(){return self.skipWaiting();}"
                            "function importScripts(){return "
                            "self.importScripts.apply(self,arguments);}"
                            "function fetch(){return self.fetch.apply(self,arguments);}\n";
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
static bool sw_run_scope(js_ctx *ctx, js_object *scope, const char *src, const char *label)
{
  char *w = sw_wrap_src(src);
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_value res, fn, a;
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
  if (!prog) {
    js_arena_free(ar);
    free(ar);
    return false;
  }
  if (!arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    return false;
  }
  js_set_cur_script(label);
  gc_push_scope();
  if (js_run(ctx, prog, &res) != 0) {
    report_exc(ctx);
    gc_pop_scope();
    return false;
  }
  fn = res;
  if (!js_is_callable(fn)) {
    gc_pop_scope();
    return false;
  }
  gc_keep(fn.u.obj);
  a = js_obj_value(scope);
  js_call(ctx, fn, a, &a, 1);
  if (ctx->has_exc) {
    report_exc(ctx);
    gc_pop_scope();
    return false;
  }
  gc_pop_scope();
  return true;
}
static js_value swg_import_scripts(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int i;
  (void)t;
  for (i = 0; i < argc; i++) {
    char url[URL_MAX];
    js_value locv = g_sw_active_scope ? js_get(ctx, js_obj_value(g_sw_active_scope), "location") : js_undef();
    js_value href = locv.type == JV_OBJ ? js_get(ctx, locv, "href") : js_undef();
    struct http_req rq = {0};
    struct http_resp rs = {0};
    unsigned char *body;
    resolve_url(href.type == JV_STR ? href.u.str->data : g_page_url_buf, arg_str(ctx, args[i]), url, sizeof url);
    rq.url = url;
    rq.method = "GET";
    body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
    if (!body || rs.status < 200 || rs.status >= 300) {
      if (body)
        free(body);
      return js_throw_type(ctx, "importScripts failed");
    }
    {
      char *code = (char *)malloc((unsigned)rs.len + 1);
      if (code) {
        js_arena *ar;
        memcpy(code, body, (unsigned)rs.len);
        code[rs.len] = 0;
        ar = (js_arena *)malloc(sizeof *ar);
        if (ar) {
          js_parse_error pe;
          js_node *prog;
          js_arena_init(ar);
          prog = js_parse(code, ar, &pe);
          if (prog && arenas_push(ar)) {
            js_value r;
            js_set_cur_script(url);
            if (js_run(ctx, prog, &r) != 0)
              report_exc(ctx);
          } else {
            js_arena_free(ar);
            free(ar);
          }
        }
        free(code);
      }
    }
    free(body);
  }
  return js_undef();
}
static js_value swg_skip_waiting(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return sw_resolve(ctx, js_undef());
}

/* Settle a pending navigator.serviceWorker.ready and fire controllerchange when
 * the controlling worker changes. */
static void sw_set_controller(js_ctx *ctx, js_value worker)
{
  js_value old;
  if (!g_sw_container)
    return;
  old = js_get(ctx, js_obj_value(g_sw_container), "controller");
  if (js_strict_eq(old, worker))
    return;
  js_def(g_sw_container, "controller", worker, P_WRITABLE | P_ENUMERABLE);
  {
    js_value ev = make_event("controllerchange", 0, 0, js_undef());
    gc_push_scope();
    if (ev.type == JV_OBJ) {
      gc_keep(ev.u.obj);
      js_set(ctx, ev, "target", js_obj_value(g_sw_container));
      ev_fire_on(g_sw_container, ev, 0);
    }
    gc_pop_scope();
  }
  {
    js_value rr = js_get(ctx, js_obj_value(g_sw_container), "@readyres");
    if (js_is_callable(rr)) {
      js_value reg = js_get(ctx, js_obj_value(g_sw_container), "@reg");
      gc_push_scope();
      {
        js_value a = reg;
        js_call(ctx, rr, js_undef(), &a, 1);
      }
      js_delete(g_sw_container, "@readyres");
      js_delete(g_sw_container, "@ready");
      gc_pop_scope();
    }
  }
}
/* Build the ServiceWorkerGlobalScope, run the worker source in it, optionally
 * fire install/activate, and make it the page's controller. Returns the
 * page-side ServiceWorkerRegistration, or undefined if the script errored. */
static js_value sw_instantiate(js_ctx *ctx, struct sw_reg *reg, bool do_install, bool do_activate)
{
  js_object *scope;
  js_value worker, regobj, clients;
  gc_push_scope();
  scope = (js_object *)gc_keep(js_new_object(ctx, g_swscope_proto));
  js_def(scope, "self", js_obj_value(scope), P_WRITABLE | P_ENUMERABLE);
  {
    js_object *loc = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    char org[URL_MAX];
    struct purl pu;
    cors_origin(reg->script_url, org, sizeof org);
    pu.host[0] = 0;
    pu.path[0] = 0;
    pu.tls = 0;
    parse_url(reg->script_url, &pu);
    js_def(loc, "href", js_mk_cstring(ctx, reg->script_url), P_ENUMERABLE);
    js_def(loc, "origin", js_mk_cstring(ctx, org), P_ENUMERABLE);
    js_def(loc, "pathname", js_mk_cstring(ctx, pu.path), P_ENUMERABLE);
    js_def(loc, "protocol", js_mk_cstring(ctx, pu.tls ? "https:" : "http:"), P_ENUMERABLE);
    js_def(loc, "host", js_mk_cstring(ctx, pu.host), P_ENUMERABLE);
    js_def(loc, "hostname", js_mk_cstring(ctx, pu.host), P_ENUMERABLE);
    js_def(loc, "search", js_mk_cstring(ctx, ""), P_ENUMERABLE);
    js_def(loc, "hash", js_mk_cstring(ctx, ""), P_ENUMERABLE);
    js_def(scope, "location", js_obj_value(loc), P_ENUMERABLE);
  }
  js_def(scope, "caches", sw_make_cachestorage(ctx), P_ENUMERABLE);
  clients = sw_make_clients(ctx);
  if (clients.type == JV_OBJ)
    gc_keep(clients.u.obj);
  js_def(scope, "clients", clients, P_ENUMERABLE);
  worker = sw_make_worker(ctx, reg->script_url, "activated");
  if (worker.type == JV_OBJ)
    gc_keep(worker.u.obj);
  regobj = sw_make_registration(ctx, reg, worker);
  if (regobj.type == JV_OBJ)
    gc_keep(regobj.u.obj);
  js_def(scope, "registration", regobj, P_ENUMERABLE);
  js_def(scope, "serviceWorker", worker, P_ENUMERABLE);
  js_def(scope, "@worker", worker, P_WRITABLE);
  js_def(scope, "onfetch", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "oninstall", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "onactivate", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(scope, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  g_sw_active_scope = scope; /* rooted global: survives the pop */
  if (g_sw_container)
    js_def(g_sw_container, "@reg", regobj, P_WRITABLE);
  if (!sw_run_scope(ctx, scope, reg->src, reg->script_url)) {
    g_sw_active_scope = 0;
    gc_pop_scope();
    return js_undef();
  }
  if (do_install) {
    sw_fire_lifecycle(ctx, scope, "install");
    reg->installed = true;
  }
  if (do_activate)
    sw_fire_lifecycle(ctx, scope, "activate");
  sw_set_controller(ctx, worker);
  gc_pop_scope();
  return regobj;
}

/* ---- navigator.serviceWorker container ---- */
static js_value swc_register(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char script[URL_MAX], scope[URL_MAX], org[URL_MAX];
  struct sw_reg *r;
  js_value regobj;
  struct http_req rq = {0};
  struct http_resp rs = {0};
  unsigned char *body;
  bool changed;
  (void)t;
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), script, sizeof script);
  scope[0] = 0;
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value s = js_get(ctx, A(1), "scope");
    if (s.type == JV_STR)
      resolve_url(g_page_url_buf, s.u.str->data, scope, sizeof scope);
  }
  if (!scope[0])
    sw_default_scope(script, scope, sizeof scope);
  if (!cors_same_origin(g_page_url_buf, script))
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "SecurityError", "ServiceWorker script must be same-origin"));
  cors_origin(g_page_url_buf, org, sizeof org);
  rq.url = script;
  rq.method = "GET";
  body = browser_cors_fetch(g_page_url_buf, 0, &rq, &rs);
  if (!body || rs.status < 200 || rs.status >= 300) {
    if (body)
      free(body);
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "ServiceWorker script fetch failed"));
  }
  r = sw_reg_add(org, scope, script);
  if (!r) {
    free(body);
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "Error", "registration failed"));
  }
  changed = !(r->src && r->slen == rs.len && memcmp(r->src, body, (unsigned)rs.len) == 0);
  if (changed) {
    sw_reg_set_src(r, (char *)body, rs.len);
    r->installed = false;
  }
  free(body);
  regobj = sw_instantiate(ctx, r, !r->installed, true);
  if (regobj.type != JV_OBJ)
    return js_promise_rejected(ctx,
                               js_make_error(ctx, ctx->error_proto, "Error", "ServiceWorker script evaluation failed"));
  return sw_resolve(ctx, regobj);
}
static js_value swc_get_registration(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  struct sw_reg *r;
  (void)t;
  cors_origin(g_page_url_buf, org, sizeof org);
  if (argc >= 1 && A(0).type == JV_STR) {
    char u[URL_MAX];
    resolve_url(g_page_url_buf, A(0).u.str->data, u, sizeof u);
    r = sw_reg_for_url(u);
  } else
    r = sw_reg_for_url(g_page_url_buf);
  if (!r)
    return sw_resolve(ctx, js_undef());
  {
    js_value w, reg, rv;
    gc_push_scope();
    w = sw_make_worker(ctx, r->script_url, "activated");
    if (w.type == JV_OBJ)
      gc_keep(w.u.obj);
    reg = sw_make_registration(ctx, r, w);
    if (reg.type == JV_OBJ)
      gc_keep(reg.u.obj);
    rv = js_promise_resolved(ctx, reg);
    gc_pop_scope();
    return rv;
  }
}
static js_value swc_get_registrations(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  js_object *arr;
  js_value rv;
  int i;
  (void)t;
  (void)args;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < g_swr_n; i++)
    if (g_swr[i] && streq(g_swr[i]->origin, org)) {
      js_value w = sw_make_worker(ctx, g_swr[i]->script_url, "activated");
      if (w.type == JV_OBJ)
        gc_keep(w.u.obj);
      js_array_push(ctx, arr, sw_make_registration(ctx, g_swr[i], w));
    }
  rv = js_promise_resolved(ctx, js_obj_value(arr));
  gc_pop_scope();
  return rv;
}
static js_value sw_pending_executor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  if (g_sw_exec_holder) {
    js_def(g_sw_exec_holder, "res", A(0), P_WRITABLE);
    js_def(g_sw_exec_holder, "rej", A(1), P_WRITABLE);
  }
  return js_undef();
}
static js_value sw_new_pending(js_ctx *ctx, js_value *resolve_out)
{
  js_value P = js_get(ctx, js_obj_value(ctx->global), "Promise");
  js_value prom;
  if (resolve_out)
    *resolve_out = js_undef();
  if (!js_is_callable(P))
    return js_promise_resolved(ctx, js_undef());
  gc_push_scope();
  {
    js_object *holder = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_object *ex = (js_object *)gc_keep(js_new_native(ctx, sw_pending_executor, "", 2));
    js_value exv = js_obj_value(ex);
    js_object *saved = g_sw_exec_holder;
    g_sw_exec_holder = holder;
    prom = js_construct(ctx, P, &exv, 1); /* runs the executor synchronously */
    g_sw_exec_holder = saved;
    if (resolve_out)
      *resolve_out = js_get(ctx, js_obj_value(holder), "res");
  }
  gc_pop_scope();
  return prom;
}
static js_value swc_ready_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *cont = t.type == JV_OBJ ? t.u.obj : g_sw_container;
  (void)args;
  (void)argc;
  if (!cont)
    return js_promise_resolved(ctx, js_undef());
  {
    js_value ctrl = js_get(ctx, js_obj_value(cont), "controller");
    if (ctrl.type == JV_OBJ) {
      js_value reg = js_get(ctx, js_obj_value(cont), "@reg");
      if (reg.type == JV_OBJ)
        return sw_resolve(ctx, reg);
    }
  }
  {
    js_value cached = js_get(ctx, js_obj_value(cont), "@ready");
    if (cached.type == JV_OBJ)
      return cached;
  }
  {
    js_value resolve, prom = sw_new_pending(ctx, &resolve);
    js_def(cont, "@ready", prom, P_WRITABLE);
    js_def(cont, "@readyres", resolve, P_WRITABLE);
    return prom;
  }
}

/* Install the whole navigator.serviceWorker + caches surface into this context,
 * then, if a registration already controls this page, start its worker (without
 * re-firing install/activate — those already happened) so it can intercept. */
void sw_install(js_ctx *ctx, js_object *win, const char *page_url)
{
  js_object *nav = 0;
  g_swcontainer_proto = js_new_object(ctx, ctx->object_proto);
  g_swreg_proto = js_new_object(ctx, ctx->object_proto);
  g_sw_proto = js_new_object(ctx, ctx->object_proto);
  g_swscope_proto = js_new_object(ctx, ctx->object_proto);
  g_clients_proto = js_new_object(ctx, ctx->object_proto);
  g_client_proto = js_new_object(ctx, ctx->object_proto);
  g_cachestorage_proto = js_new_object(ctx, ctx->object_proto);
  g_cache_proto = js_new_object(ctx, ctx->object_proto);
  g_request_proto = js_new_object(ctx, ctx->object_proto);
  g_sw_container = 0;
  g_sw_active_scope = 0;
  g_sw_exec_holder = 0;
  g_sw_in_fetch = 0;
  gc_add_root((void **)&g_swcontainer_proto);
  gc_add_root((void **)&g_swreg_proto);
  gc_add_root((void **)&g_sw_proto);
  gc_add_root((void **)&g_swscope_proto);
  gc_add_root((void **)&g_clients_proto);
  gc_add_root((void **)&g_client_proto);
  gc_add_root((void **)&g_cachestorage_proto);
  gc_add_root((void **)&g_cache_proto);
  gc_add_root((void **)&g_request_proto);
  gc_add_root((void **)&g_sw_container);
  gc_add_root((void **)&g_sw_active_scope);

  def_method(g_swscope_proto, "addEventListener", el_add_listener, 3);
  def_method(g_swscope_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_swscope_proto, "dispatchEvent", el_dispatch, 1);
  def_method(g_swscope_proto, "skipWaiting", swg_skip_waiting, 0);
  def_method(g_swscope_proto, "importScripts", swg_import_scripts, 1);
  def_method(g_swscope_proto, "fetch", win_fetch, 2);

  def_method(g_clients_proto, "matchAll", clients_matchall, 1);
  def_method(g_clients_proto, "claim", clients_claim, 0);
  def_method(g_clients_proto, "get", clients_get, 1);
  def_method(g_clients_proto, "openWindow", clients_openwindow, 1);

  def_method(g_client_proto, "postMessage", client_postmessage, 1);

  def_method(g_sw_proto, "postMessage", worker_postmessage, 1);
  def_method(g_sw_proto, "addEventListener", el_add_listener, 3);
  def_method(g_sw_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_sw_proto, "dispatchEvent", el_dispatch, 1);

  def_method(g_swreg_proto, "unregister", reg_unregister, 0);
  def_method(g_swreg_proto, "update", reg_update, 0);
  def_method(g_swreg_proto, "showNotification", reg_show_notification, 2);
  def_method(g_swreg_proto, "getNotifications", reg_get_notifications, 0);
  def_method(g_swreg_proto, "addEventListener", el_add_listener, 3);
  def_method(g_swreg_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_swreg_proto, "dispatchEvent", el_dispatch, 1);

  def_method(g_cachestorage_proto, "open", caches_open, 1);
  def_method(g_cachestorage_proto, "match", caches_match, 1);
  def_method(g_cachestorage_proto, "has", caches_has, 1);
  def_method(g_cachestorage_proto, "delete", caches_delete, 1);
  def_method(g_cachestorage_proto, "keys", caches_keys, 0);

  def_method(g_cache_proto, "match", cache_match, 1);
  def_method(g_cache_proto, "matchAll", cache_matchall, 1);
  def_method(g_cache_proto, "add", cache_add, 1);
  def_method(g_cache_proto, "addAll", cache_addall, 1);
  def_method(g_cache_proto, "put", cache_put, 2);
  def_method(g_cache_proto, "delete", cache_delete, 1);
  def_method(g_cache_proto, "keys", cache_keys, 0);

  def_method(g_request_proto, "clone", req_clone, 0);

  js_def(win, "caches", sw_make_cachestorage(ctx), P_WRITABLE | P_CONFIGURABLE);

  {
    js_object *rc, *qc;
    gc_push_scope();
    rc = (js_object *)gc_keep(js_new_native(ctx, response_ctor, "Response", 2));
    js_def(rc, "prototype", js_obj_value(g_response_proto), 0);
    js_def(g_response_proto, "constructor", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "Response", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
    qc = (js_object *)gc_keep(js_new_native(ctx, request_ctor, "Request", 2));
    js_def(qc, "prototype", js_obj_value(g_request_proto), 0);
    js_def(g_request_proto, "constructor", js_obj_value(qc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "Request", js_obj_value(qc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }

  {
    js_value nv = js_get(ctx, js_obj_value(win), "navigator");
    if (nv.type == JV_OBJ)
      nav = nv.u.obj;
  }
  {
    js_object *cont, *rg;
    gc_push_scope();
    cont = (js_object *)gc_keep(js_new_object(ctx, g_swcontainer_proto));
    def_method(cont, "register", swc_register, 2);
    def_method(cont, "getRegistration", swc_get_registration, 1);
    def_method(cont, "getRegistrations", swc_get_registrations, 0);
    def_method(cont, "startMessages", el_noop, 0);
    def_method(cont, "addEventListener", el_add_listener, 3);
    def_method(cont, "removeEventListener", el_remove_listener, 3);
    def_method(cont, "dispatchEvent", el_dispatch, 1);
    js_def(cont, "controller", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(cont, "oncontrollerchange", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(cont, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
    js_def(cont, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
    rg = (js_object *)gc_keep(js_new_native(ctx, swc_ready_get, "ready", 0));
    js_def_accessor(cont, "ready", rg, 0, P_CONFIGURABLE | P_ENUMERABLE);
    g_sw_container = cont;
    if (nav)
      js_def(nav, "serviceWorker", js_obj_value(cont), P_ENUMERABLE);
    gc_pop_scope();
  }

  {
    struct sw_reg *r = sw_reg_for_url(page_url);
    if (r && r->src)
      sw_instantiate(ctx, r, false, false);
  }
}

/* Drop the per-context SW roots + objects. The persistent C store (g_swr/g_swc)
 * survives — that's what carries registrations + caches across navigations. */
void sw_teardown(void)
{
  gc_remove_root((void **)&g_swcontainer_proto);
  gc_remove_root((void **)&g_swreg_proto);
  gc_remove_root((void **)&g_sw_proto);
  gc_remove_root((void **)&g_swscope_proto);
  gc_remove_root((void **)&g_clients_proto);
  gc_remove_root((void **)&g_client_proto);
  gc_remove_root((void **)&g_cachestorage_proto);
  gc_remove_root((void **)&g_cache_proto);
  gc_remove_root((void **)&g_request_proto);
  gc_remove_root((void **)&g_sw_container);
  gc_remove_root((void **)&g_sw_active_scope);
  g_swcontainer_proto = g_swreg_proto = g_sw_proto = g_swscope_proto = 0;
  g_clients_proto = g_client_proto = g_cachestorage_proto = g_cache_proto = g_request_proto = 0;
  g_sw_container = g_sw_active_scope = g_sw_exec_holder = 0;
  g_sw_in_fetch = 0;
}
