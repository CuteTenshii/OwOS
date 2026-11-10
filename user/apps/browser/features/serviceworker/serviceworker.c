/* Service Workers and the Cache API. Registrations and caches persist across navigations. */
#include <stdbool.h>

#include "features/cors/cors.h" /* cors_origin / cors_same_origin: SW scope + cache keys */
#include "features/persist/persist.h"
#include "jsint.h"
#include "mutex.h"
#include "notify.h" /* notify_send: a service worker's showNotification()    */

/* One cached request/response pair; body is raw bytes. */
struct sw_c_entry {
  char *method, *url, *statustext, *ctype;
  unsigned char *body;
  int blen, status;
};
/* A named Cache, scoped to an origin. */
struct sw_cache {
  int id; /* stable across the process (JS wrapper key) */
  char *origin, *name;
  struct sw_c_entry *e;
  int n, cap;
};
static struct sw_cache **g_swc; /* every open Cache, all origins             */
static int g_swc_n, g_swc_cap, g_swc_nextid = 1;
static bool g_swc_loading; /* reading a file: the entries it holds are not changes */

/* The caches are shared by the page and its workers; one lock covers the store and everything that holds a pointer
 * into it. A thread may take it again while it holds it (a ToString hook can re-enter the Cache API). */
static mutex_t g_swc_lock = MUTEX_INIT;
static __thread int g_swc_depth;
static void swc_lock(void)
{
  if (g_swc_depth++ == 0)
    mutex_lock(&g_swc_lock);
}
static void swc_unlock(void)
{
  if (--g_swc_depth == 0)
    mutex_unlock(&g_swc_lock);
}
static void sw_cache_dirty(const char *origin);

/* A registration; src is kept so the worker can be re-run without a re-fetch. */
struct sw_reg {
  char *origin, *scope, *script_url, *src;
  int slen;
  bool installed; /* install has fired once for this src        */
};
static struct sw_reg **g_swr;
static int g_swr_n, g_swr_cap;

/* Per-navigation JS objects, rebuilt in sw_install and dropped in sw_teardown. */
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

/* The worker script runs in the page context inside its own scope object bound as `self`. */

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
/* A resolved Promise of `v`, keeping v alive across the promise allocation. */
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
    c->origin = strdup(origin);
    c->name = strdup(name);
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
  e->method = strdup(method && method[0] ? method : "GET");
  e->url = strdup(url);
  e->status = status ? status : 200;
  e->statustext = strdup(st ? st : "");
  e->ctype = strdup(ct ? ct : "");
  e->blen = blen > 0 ? blen : 0;
  e->body = (unsigned char *)malloc((unsigned)(e->blen ? e->blen : 1));
  if (e->body && e->blen)
    memcpy(e->body, body, (unsigned)e->blen);
  if (!g_swc_loading)
    sw_cache_dirty(c->origin);
}

/* ---- persistence: the caches of one origin are one file, written after they change ---- */

#define SWC_SAVE_MS 500                    /* least gap between two writes */
static char **g_swc_loaded, **g_swc_dirty; /* origins read from disk / changed since they were written */
static int g_swc_nloaded, g_swc_ndirty;
static unsigned g_swc_last_save;

struct wbuf {
  unsigned char *p;
  unsigned n, cap;
  int failed;
};
static void wb_put(struct wbuf *b, const void *d, unsigned n)
{
  if (b->failed)
    return;
  if (b->n + n > b->cap) {
    unsigned nc = b->cap ? b->cap : 4096;
    unsigned char *np;
    while (nc < b->n + n)
      nc *= 2;
    np = (unsigned char *)realloc(b->p, nc);
    if (!np) {
      b->failed = 1;
      return;
    }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void wb_u32(struct wbuf *b, unsigned v)
{
  unsigned char x[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  wb_put(b, x, 4);
}
static void wb_str(struct wbuf *b, const char *s)
{
  unsigned n = s ? (unsigned)strlen(s) : 0;
  wb_u32(b, n);
  wb_put(b, s, n);
}

struct rbuf {
  const unsigned char *p;
  unsigned n, at;
  int bad;
};
static unsigned rb_u32(struct rbuf *r)
{
  unsigned v;
  if (r->bad || r->n - r->at < 4) {
    r->bad = 1;
    return 0;
  }
  v = r->p[r->at] | (unsigned)r->p[r->at + 1] << 8 | (unsigned)r->p[r->at + 2] << 16 | (unsigned)r->p[r->at + 3] << 24;
  r->at += 4;
  return v;
}
/* A length-prefixed run of bytes inside the buffer, NUL-terminated copy when `text` */
static const unsigned char *rb_bytes(struct rbuf *r, unsigned *len)
{
  const unsigned char *p;
  *len = rb_u32(r);
  if (r->bad || r->n - r->at < *len) {
    r->bad = 1;
    *len = 0;
    return 0;
  }
  p = r->p + r->at;
  r->at += *len;
  return p;
}
static char *rb_str(struct rbuf *r)
{
  unsigned n;
  const unsigned char *p = rb_bytes(r, &n);
  char *s;
  if (r->bad)
    return 0;
  s = (char *)malloc(n + 1);
  if (s) {
    memcpy(s, p, n);
    s[n] = 0;
  }
  return s;
}

static int sw_origin_listed(char **list, int n, const char *origin)
{
  int i;
  for (i = 0; i < n; i++)
    if (streq(list[i], origin))
      return 1;
  return 0;
}
static void sw_origin_add(char ***list, int *n, const char *origin)
{
  char **nl = (char **)realloc(*list, (unsigned)(*n + 1) * sizeof **list);
  if (!nl)
    return;
  *list = nl;
  nl[*n] = strdup(origin);
  if (nl[*n])
    (*n)++;
}
static void sw_cache_dirty(const char *origin)
{
  if (!sw_origin_listed(g_swc_dirty, g_swc_ndirty, origin))
    sw_origin_add(&g_swc_dirty, &g_swc_ndirty, origin);
}

/* Reads the origin's file the first time its caches are looked at */
static void sw_caches_ensure(const char *origin)
{
  char path[160];
  unsigned char *bytes;
  int len = 0;
  struct rbuf r;
  unsigned nc, i;
  if (sw_origin_listed(g_swc_loaded, g_swc_nloaded, origin))
    return;
  sw_origin_add(&g_swc_loaded, &g_swc_nloaded, origin);
  if (persist_path("cache", origin, path, sizeof path) != 0 || !(bytes = persist_read(path, &len)))
    return;
  r.p = bytes;
  r.n = (unsigned)len;
  r.at = 0;
  r.bad = 0;
  g_swc_loading = true;
  nc = rb_u32(&r);
  for (i = 0; i < nc && !r.bad; i++) {
    char *name = rb_str(&r);
    unsigned ne = rb_u32(&r), k;
    struct sw_cache *c = name && !r.bad ? sw_cache_find(origin, name, true) : 0;
    free(name);
    for (k = 0; k < ne && !r.bad; k++) {
      char *method = rb_str(&r), *url = rb_str(&r), *st = rb_str(&r), *ct = rb_str(&r);
      unsigned status = rb_u32(&r), blen;
      const unsigned char *body = rb_bytes(&r, &blen);
      if (c && !r.bad && method && url)
        sw_cache_store(c, method, url, (int)status, st, ct, (const char *)body, (int)blen);
      free(method);
      free(url);
      free(st);
      free(ct);
    }
  }
  g_swc_loading = false;
  free(bytes);
}

static void sw_caches_save(const char *origin)
{
  struct wbuf b = {0, 0, 0, 0};
  char path[160];
  unsigned nc = 0;
  int i, k;
  if (persist_path("cache", origin, path, sizeof path) != 0)
    return;
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, origin))
      nc++;
  wb_u32(&b, nc);
  for (i = 0; i < g_swc_n; i++) {
    struct sw_cache *c = g_swc[i];
    if (!c || !streq(c->origin, origin))
      continue;
    wb_str(&b, c->name);
    wb_u32(&b, (unsigned)c->n);
    for (k = 0; k < c->n; k++) {
      struct sw_c_entry *e = &c->e[k];
      wb_str(&b, e->method);
      wb_str(&b, e->url);
      wb_str(&b, e->statustext);
      wb_str(&b, e->ctype);
      wb_u32(&b, (unsigned)e->status);
      wb_u32(&b, (unsigned)e->blen);
      wb_put(&b, e->body, (unsigned)e->blen);
    }
  }
  if (!b.failed)
    persist_write(path, b.p, (int)b.n);
  free(b.p);
}

/* Writes the origins whose caches changed; `force` ignores the gap since the last write */
static void sw_caches_flush(bool force)
{
  int i;
  swc_lock();
  if (!g_swc_ndirty || (!force && msec() - g_swc_last_save < SWC_SAVE_MS)) {
    swc_unlock();
    return;
  }
  for (i = 0; i < g_swc_ndirty; i++) {
    sw_caches_save(g_swc_dirty[i]);
    free(g_swc_dirty[i]);
  }
  free(g_swc_dirty);
  g_swc_dirty = 0;
  g_swc_ndirty = 0;
  g_swc_last_save = msec();
  swc_unlock();
}

/* The page is going away: what is on disk is the truth, so drop the caches that have a file. The ones of an origin
 * that cannot persist stay in memory for the next page of this process. */
static void sw_caches_unload(void)
{
  int i, k, keep = 0;
  char path[160];
  swc_lock();
  for (i = 0; i < g_swc_n; i++) {
    struct sw_cache *c = g_swc[i];
    if (c && persist_path("cache", c->origin, path, sizeof path) != 0) {
      g_swc[keep++] = c;
      continue;
    }
    if (!c)
      continue;
    for (k = 0; k < c->n; k++)
      sw_entry_free(&c->e[k]);
    free(c->e);
    free(c->origin);
    free(c->name);
    free(c);
  }
  g_swc_n = keep;
  for (i = 0; i < g_swc_nloaded; i++)
    free(g_swc_loaded[i]);
  free(g_swc_loaded);
  g_swc_loaded = 0;
  g_swc_nloaded = 0;
  swc_unlock();
}

int sw_cache_pump(void)
{
  sw_caches_flush(false);
  return 0;
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
    char *url = strdup(script_url);
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
  r->origin = strdup(origin);
  r->scope = strdup(scope);
  r->script_url = strdup(script_url);
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
/* Registration controlling page_url: same origin, longest matching scope prefix. */
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
/* Default scope: the script URL up to its last '/'. */
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
  const char *s, *err;
  js_value rv, mv;
  int cm;
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
  cm = fetch_cache_mode(ctx, A(0), A(1), &err);
  if (cm < 0)
    return js_throw_type(ctx, err);
  rv = sw_make_request(ctx, url, method);
  gc_push_scope();
  gc_keep(rv.u.obj);
  js_def(rv.u.obj, "cache", js_mk_cstring(ctx, fetch_cache_names[cm]), P_ENUMERABLE);
  mv = A(1).type == JV_OBJ ? js_get(ctx, A(1), "mode") : js_undef();
  if (mv.type == JV_UNDEF && in.type == JV_OBJ)
    mv = js_get(ctx, in, "mode");
  if (mv.type == JV_STR)
    js_def(rv.u.obj, "mode", mv, P_ENUMERABLE);
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
/* The network fetch of add/addAll runs without the lock: the cache is looked up again by id to store the result */
static int cache_id_of(js_ctx *ctx, js_value t)
{
  struct sw_cache *c;
  int id = -1;
  swc_lock();
  c = cache_of(ctx, t);
  if (c)
    id = c->id;
  swc_unlock();
  return id;
}
static bool cache_fetch_store(int id, const char *method, const char *url)
{
  struct fetch_req rq = {.method = "GET"};
  struct fetch_res *rs = malloc(sizeof *rs);
  struct sw_cache *c;
  bool ok;
  if (!rs)
    return false;
  strlcpy(rq.url, url, sizeof rq.url);
  if (method[0])
    strlcpy(rq.method, method, sizeof rq.method);
  ok = fetch_run(&rq, rs) == FETCH_OK && rs->status >= 200 && rs->status < 300;
  if (ok) {
    swc_lock();
    c = sw_cache_by_id(id);
    if (c)
      sw_cache_store(c, method, url, rs->status, rs->status_text, rs->ctype, (const char *)rs->body, rs->len);
    swc_unlock();
  }
  free(rs->body);
  free(rs);
  return ok;
}
static js_value cache_add(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], method[16];
  int id = cache_id_of(ctx, t);
  (void)argc;
  if (id < 0)
    return sw_resolve(ctx, js_undef());
  sw_input_url(ctx, A(0), url, sizeof url);
  sw_input_method(ctx, A(0), method, sizeof method);
  if (!cache_fetch_store(id, method, url))
    return js_promise_rejected(ctx,
                               js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.add: request failed"));
  return sw_resolve(ctx, js_undef());
}
static js_value cache_addall(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value list = A(0);
  unsigned n, i;
  int id = cache_id_of(ctx, t);
  (void)argc;
  if (id < 0 || list.type != JV_OBJ)
    return sw_resolve(ctx, js_undef());
  n = js_array_len(list.u.obj);
  for (i = 0; i < n; i++) {
    char url[URL_MAX], method[16];
    js_value it = js_get_index(ctx, list, i);
    sw_input_url(ctx, it, url, sizeof url);
    sw_input_method(ctx, it, method, sizeof method);
    if (!cache_fetch_store(id, method, url))
      return js_promise_rejected(
          ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Cache.addAll: request failed"));
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
  sw_cache_dirty(c->origin);
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
  sw_caches_ensure(org);
  if (!sw_cache_find(org, arg_str(ctx, A(0)), false))
    sw_cache_dirty(org);
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
  sw_caches_ensure(org);
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
  sw_caches_ensure(org);
  return sw_resolve(ctx, js_bool(sw_cache_find(org, arg_str(ctx, A(0)), false) != 0));
}
/* Removes the named cache of an origin; whether there was one */
static bool sw_cache_drop(const char *org, const char *name)
{
  int i, k;
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, org) && streq(g_swc[i]->name, name)) {
      struct sw_cache *c = g_swc[i];
      for (k = 0; k < c->n; k++)
        sw_entry_free(&c->e[k]);
      free(c->e);
      free(c->origin);
      free(c->name);
      free(c);
      g_swc[i] = g_swc[--g_swc_n];
      sw_cache_dirty(org);
      return true;
    }
  return false;
}
static js_value caches_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char org[URL_MAX];
  (void)t;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  sw_caches_ensure(org);
  return sw_resolve(ctx, js_bool(sw_cache_drop(org, arg_str(ctx, A(0)))));
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
  sw_caches_ensure(org);
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
  ev = make_message_event(data, js_undef());
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
  unsigned char *body;
  int blen = 0;
  (void)args;
  (void)argc;
  cors_origin(g_page_url_buf, org, sizeof org);
  r = sw_reg_find_scope(org, sc.type == JV_STR ? sc.u.str->data : "");
  if (!r)
    return sw_resolve(ctx, t);
  body = fetch_get(r->script_url, &blen);
  if (body) {
    bool changed = !(r->src && r->slen == blen && memcmp(r->src, body, (unsigned)blen) == 0);
    if (changed) {
      sw_reg_set_src(r, (char *)body, blen);
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
  /* Promises settle synchronously; this exists so handlers calling it don't throw. */
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
    unsigned char *body;
    int blen = 0;
    resolve_url(href.type == JV_STR ? href.u.str->data : g_page_url_buf, arg_str(ctx, args[i]), url, sizeof url);
    body = fetch_get(url, &blen);
    if (!body)
      return js_throw_type(ctx, "importScripts failed");
    {
      char *code = (char *)malloc((unsigned)blen + 1);
      if (code) {
        js_arena *ar;
        memcpy(code, body, (unsigned)blen);
        code[blen] = 0;
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

/* Settles a pending .ready and fires controllerchange when the controller changes. */
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
/* Runs the worker, optionally fires install/activate, sets it as controller. Undefined on error. */
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
  unsigned char *body;
  int blen = 0;
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
    return js_promise_rejected(ctx, dom_exception(ctx, "SecurityError", "ServiceWorker script must be same-origin"));
  cors_origin(g_page_url_buf, org, sizeof org);
  body = fetch_get(script, &blen);
  if (!body) {
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "ServiceWorker script fetch failed"));
  }
  r = sw_reg_add(org, scope, script);
  if (!r) {
    free(body);
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->error_proto, "Error", "registration failed"));
  }
  changed = !(r->src && r->slen == blen && memcmp(r->src, body, (unsigned)blen) == 0);
  if (changed) {
    sw_reg_set_src(r, (char *)body, blen);
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

#define LOCKED(fn)                                                                                                     \
  static js_value fn##_locked(js_ctx *ctx, js_value t, js_value *args, int argc)                                       \
  {                                                                                                                    \
    js_value r;                                                                                                        \
    swc_lock();                                                                                                        \
    r = fn(ctx, t, args, argc);                                                                                        \
    swc_unlock();                                                                                                      \
    return r;                                                                                                          \
  }
LOCKED(caches_open)
LOCKED(caches_match)
LOCKED(caches_has)
LOCKED(caches_delete)
LOCKED(caches_keys)
LOCKED(cache_match)
LOCKED(cache_matchall)
LOCKED(cache_put)
LOCKED(cache_delete)
LOCKED(cache_keys)
#undef LOCKED

/* ---- the Cache API inside a worker: natives the script side builds Cache and CacheStorage on ---- */

static __thread char g_wc_origin[URL_MAX];

static js_value wc_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c;
  const char *name = arg_str(ctx, A(0));
  int id;
  (void)t;
  swc_lock();
  sw_caches_ensure(g_wc_origin);
  if (!sw_cache_find(g_wc_origin, name, false))
    sw_cache_dirty(g_wc_origin);
  c = sw_cache_find(g_wc_origin, name, true);
  id = c ? c->id : -1;
  swc_unlock();
  return js_number(id);
}

static js_value wc_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int has;
  (void)t;
  swc_lock();
  sw_caches_ensure(g_wc_origin);
  has = sw_cache_find(g_wc_origin, arg_str(ctx, A(0)), false) != 0;
  swc_unlock();
  return js_bool(has);
}

static js_value wc_delete(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int gone;
  (void)t;
  swc_lock();
  sw_caches_ensure(g_wc_origin);
  gone = sw_cache_drop(g_wc_origin, arg_str(ctx, A(0)));
  swc_unlock();
  return js_bool(gone);
}

/* The names of this origin's caches */
static js_value wc_names(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  int i;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  swc_lock();
  sw_caches_ensure(g_wc_origin);
  for (i = 0; i < g_swc_n; i++)
    if (g_swc[i] && streq(g_swc[i]->origin, g_wc_origin))
      js_array_push(ctx, arr, js_mk_cstring(ctx, g_swc[i]->name));
  swc_unlock();
  gc_pop_scope();
  return js_obj_value(arr);
}

/* N.cachePut(id, method, url, status, statusText, contentType, bytes) */
static js_value wc_put(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c;
  int n = 0;
  const unsigned char *body = view_bytes(A(6), &n);
  char method[16], url[URL_MAX];
  (void)t;
  strlcpy(method, arg_str(ctx, A(1)), sizeof method);
  strlcpy(url, arg_str(ctx, A(2)), sizeof url);
  swc_lock();
  c = sw_cache_by_id((int)js_to_number(ctx, A(0)));
  if (c)
    sw_cache_store(c, method, url, (int)js_to_number(ctx, A(3)), arg_str(ctx, A(4)), arg_str(ctx, A(5)),
                   body ? (const char *)body : "", body ? n : 0);
  swc_unlock();
  return js_undef();
}

static js_value wc_entry_object(js_ctx *ctx, struct sw_c_entry *e)
{
  js_object *o;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "url", js_mk_cstring(ctx, e->url), P_ENUMERABLE);
  js_def(o, "method", js_mk_cstring(ctx, e->method), P_ENUMERABLE);
  js_def(o, "status", js_number(e->status), P_ENUMERABLE);
  js_def(o, "statusText", js_mk_cstring(ctx, e->statustext), P_ENUMERABLE);
  js_def(o, "ctype", js_mk_cstring(ctx, e->ctype), P_ENUMERABLE);
  js_def(o, "body", js_new_arraybuffer(ctx, e->body, e->blen), P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(o);
}

/* N.cacheMatch(id, url): the stored response, from cache `id` or (id < 0) the first cache of the origin that has it */
static js_value wc_match(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int id = (int)js_to_number(ctx, A(0)), i, k;
  char url[URL_MAX];
  js_value r = js_undef();
  (void)t;
  strlcpy(url, arg_str(ctx, A(1)), sizeof url);
  swc_lock();
  if (id < 0)
    sw_caches_ensure(g_wc_origin);
  for (i = 0; i < g_swc_n && r.type == JV_UNDEF; i++) {
    struct sw_cache *c = g_swc[i];
    if (!c || (id < 0 ? !streq(c->origin, g_wc_origin) : c->id != id))
      continue;
    k = sw_cache_index(c, url);
    if (k >= 0)
      r = wc_entry_object(ctx, &c->e[k]);
  }
  swc_unlock();
  return r;
}

static js_value wc_remove(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c;
  char url[URL_MAX];
  int i, gone = 0;
  (void)t;
  strlcpy(url, arg_str(ctx, A(1)), sizeof url);
  swc_lock();
  c = sw_cache_by_id((int)js_to_number(ctx, A(0)));
  i = c ? sw_cache_index(c, url) : -1;
  if (i >= 0) {
    sw_entry_free(&c->e[i]);
    c->e[i] = c->e[--c->n];
    sw_cache_dirty(c->origin);
    gone = 1;
  }
  swc_unlock();
  return js_bool(gone);
}

/* N.cacheEntries(id): [{url, method}] in the order stored */
static js_value wc_entries(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct sw_cache *c;
  js_object *arr;
  int i;
  (void)t;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  swc_lock();
  c = sw_cache_by_id((int)js_to_number(ctx, A(0)));
  for (i = 0; c && i < c->n; i++) {
    js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(o, "url", js_mk_cstring(ctx, c->e[i].url), P_ENUMERABLE);
    js_def(o, "method", js_mk_cstring(ctx, c->e[i].method), P_ENUMERABLE);
    js_array_push(ctx, arr, js_obj_value(o));
  }
  swc_unlock();
  gc_pop_scope();
  return js_obj_value(arr);
}

void sw_cache_worker_setup(js_ctx *ctx, js_object *n, const char *origin)
{
  strlcpy(g_wc_origin, origin, sizeof g_wc_origin);
  js_def_native(ctx, n, "cacheOpen", wc_open, 1);
  js_def_native(ctx, n, "cacheHas", wc_has, 1);
  js_def_native(ctx, n, "cacheDelete", wc_delete, 1);
  js_def_native(ctx, n, "cacheNames", wc_names, 0);
  js_def_native(ctx, n, "cachePut", wc_put, 7);
  js_def_native(ctx, n, "cacheMatch", wc_match, 2);
  js_def_native(ctx, n, "cacheRemove", wc_remove, 2);
  js_def_native(ctx, n, "cacheEntries", wc_entries, 1);
}

/* Installs navigator.serviceWorker + caches; restarts the controlling worker without re-firing install. */
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

  def_method(g_cachestorage_proto, "open", caches_open_locked, 1);
  def_method(g_cachestorage_proto, "match", caches_match_locked, 1);
  def_method(g_cachestorage_proto, "has", caches_has_locked, 1);
  def_method(g_cachestorage_proto, "delete", caches_delete_locked, 1);
  def_method(g_cachestorage_proto, "keys", caches_keys_locked, 0);

  def_method(g_cache_proto, "match", cache_match_locked, 1);
  def_method(g_cache_proto, "matchAll", cache_matchall_locked, 1);
  def_method(g_cache_proto, "add", cache_add, 1);
  def_method(g_cache_proto, "addAll", cache_addall, 1);
  def_method(g_cache_proto, "put", cache_put_locked, 2);
  def_method(g_cache_proto, "delete", cache_delete_locked, 1);
  def_method(g_cache_proto, "keys", cache_keys_locked, 0);

  def_method(g_request_proto, "clone", req_clone, 0);

  js_def(win, "caches", sw_make_cachestorage(ctx), P_WRITABLE | P_CONFIGURABLE);

  {
    js_object *qc;
    gc_push_scope();
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

/* Drops per-context roots; the persistent store (g_swr/g_swc) survives. */
void sw_teardown(void)
{
  sw_caches_flush(true);
  sw_caches_unload();
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
