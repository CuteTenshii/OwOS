/* fetch(), Headers, Request and Response. */

#include "bindings.h"
#include "fetchint.h"

/* ------------------------------------------------------- fetch ------------- */
/* Body mixin, shared by Response and Request: the first read takes the body, a second one rejects */
int body_take(js_ctx *ctx, js_value t)
{
  if (js_to_boolean(js_get(ctx, t, "bodyUsed")))
    return 0;
  js_set(ctx, t, "bodyUsed", js_bool(1));
  return 1;
}
js_value body_used_error(js_ctx *ctx)
{
  return js_promise_rejected(
      ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Body is unusable: Body has already been read"));
}
js_value body_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (!body_take(ctx, t))
    return body_used_error(ctx);
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_TEXT);
  return js_promise_resolved(ctx, js_get(ctx, t, "@body"));
}
js_value body_json(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body, parsed, jsonobj, parsefn;
  (void)args;
  (void)argc;
  if (!body_take(ctx, t))
    return body_used_error(ctx);
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_JSON);
  body = js_get(ctx, t, "@body");
  jsonobj = js_get(ctx, js_obj_value(ctx->global), "JSON");
  parsefn = js_get(ctx, jsonobj, "parse");
  if (!js_is_callable(parsefn))
    return js_promise_rejected(ctx, js_mk_cstring(ctx, "no JSON.parse"));
  parsed = js_call(ctx, parsefn, jsonobj, &body, 1);
  if (ctx->has_exc) {
    js_value e = ctx->exc;
    ctx->has_exc = 0;
    ctx->exc_root = 0;
    return js_promise_rejected(ctx, e);
  }
  return js_promise_resolved(ctx, parsed);
}
/* formData() of an application/x-www-form-urlencoded body; multipart bodies are not parsed */
js_value body_formdata(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value ct, body, fd;
  const char *p;
  js_object *ka, *va;
  (void)args;
  (void)argc;
  if (!body_take(ctx, t))
    return body_used_error(ctx);
  ct = headers_ctype(ctx, js_get(ctx, t, "headers"));
  body = js_get(ctx, t, "@body");
  if (ct.type != JV_STR || !starts_ci(ct.u.str->data, "application/x-www-form-urlencoded") || body.type != JV_STR)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Content-Type was not one of \"application/x-www-form-urlencoded\""));
  gc_push_scope();
  fd = fd_ctor(ctx, js_undef(), 0, 0);
  gc_keep(fd.u.obj);
  ka = js_get(ctx, fd, "@k").u.obj;
  va = js_get(ctx, fd, "@v").u.obj;
  for (p = body.u.str->data; *p;) {
    const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p), *eq = p;
    while (eq < end && *eq != '=')
      eq++;
    if (end > p) {
      char *k = form_decode(p, (int)(eq - p)), *v = form_decode(eq < end ? eq + 1 : end, eq < end ? (int)(end - eq - 1) : 0);
      js_array_push(ctx, ka, js_mk_cstring(ctx, k ? k : ""));
      js_array_push(ctx, va, js_mk_cstring(ctx, v ? v : ""));
      free(k);
      free(v);
    }
    p = amp ? amp + 1 : end;
  }
  gc_pop_scope();
  return js_promise_resolved(ctx, fd);
}
/* Response.clone(): a second Response over the same bytes; an unread streamed body can't be split */
js_value resp_clone(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b, st, so, u, ty;
  js_object *c;
  (void)args;
  (void)argc;
  if (js_to_boolean(js_get(ctx, t, "bodyUsed")) || js_get(ctx, t, "@source").type == JV_OBJ)
    return js_throw_type(ctx, js_to_boolean(js_get(ctx, t, "bodyUsed")) ? "Response.clone: Body has already been consumed."
                                                                       : "Response.clone: a streamed body can't be cloned");
  gc_push_scope();
  b = js_get(ctx, t, "@body");
  st = js_get(ctx, t, "statusText");
  u = js_get(ctx, t, "url");
  ty = js_get(ctx, t, "type");
  c = make_response(ctx, u.type == JV_STR ? u.u.str->data : "", (int)js_to_number(ctx, js_get(ctx, t, "status")),
                    st.type == JV_STR ? st.u.str->data : "", js_get(ctx, t, "headers"), "",
                    b.type == JV_STR ? b.u.str->data : "", b.type == JV_STR ? b.u.str->len : 0);
  gc_keep(c);
  so = js_obj_value(c);
  js_def(c, "status", js_get(ctx, t, "status"), P_ENUMERABLE);
  js_def(c, "ok", js_get(ctx, t, "ok"), P_ENUMERABLE);
  if (ty.type == JV_STR)
    js_def(c, "type", ty, P_ENUMERABLE);
  gc_pop_scope();
  return so;
}

#define JS(...) #__VA_ARGS__
/* Headers, in JavaScript: the same source runs on the page and in dedicated workers (webworker.c) */
const char fetch_headers_js[] =
#include "headers.inc"
    ;

js_value fetch_headers_install(js_ctx *ctx)
{
  js_arena *ar = (js_arena *)malloc(sizeof *ar);
  js_parse_error pe;
  js_node *prog;
  js_value fn = js_undef(), g = js_obj_value(ctx->global), ctor = js_undef();
  if (!ar)
    return ctor;
  js_arena_init(ar);
  prog = js_parse(fetch_headers_js, ar, &pe);
  if (prog && js_run(ctx, prog, &fn) == 0 && js_is_callable(fn))
    ctor = js_call(ctx, fn, js_undef(), &g, 1);
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
  }
  if (ctor.type == JV_OBJ)
    js_def(ctx->global, "@Headers", ctor, P_WRITABLE);
  return ctor;
}

js_value headers_new(js_ctx *ctx, js_value init)
{
  js_value ctor = js_get(ctx, js_obj_value(ctx->global), "@Headers");
  return js_is_callable(ctor) ? js_construct(ctx, ctor, &init, init.type == JV_UNDEF ? 0 : 1) : js_undef();
}

js_value headers_ctype(js_ctx *ctx, js_value h)
{
  js_value m = h.type == JV_OBJ ? js_get(ctx, h, "@map") : js_undef();
  return m.type == JV_OBJ ? js_get(ctx, m, "content-type") : js_undef();
}

/* Header block ("HTTP/1.1 200 OK" line first) -> [[name, value], ...], set-cookie left out */
js_value parse_header_block(js_ctx *ctx, const char *hdr)
{
  js_object *list = js_new_array(ctx, 0);
  const char *p = hdr, *eol;
  gc_push_scope();
  gc_keep(list);
  while (*p && (eol = strchr(p, '\n'))) {
    const char *colon = p, *vs, *ve = eol;
    while (colon < eol && *colon != ':')
      colon++;
    if (colon < eol && p != hdr && colon > p && p[0] != '\r') {
      char name[128];
      int nl = (int)(colon - p);
      js_object *pair;
      if (nl < (int)sizeof name) {
        memcpy(name, p, (unsigned)nl);
        name[nl] = 0;
        vs = colon + 1;
        while (vs < ve && (*vs == ' ' || *vs == '\t'))
          vs++;
        while (ve > vs && (ve[-1] == '\r' || ve[-1] == ' '))
          ve--;
        if (!streq_ci(name, "set-cookie") && !streq_ci(name, "set-cookie2")) {
          pair = js_new_array(ctx, 0);
          gc_push_scope();
          gc_keep(pair);
          js_array_push(ctx, pair, js_mk_cstring(ctx, name));
          js_array_push(ctx, pair, js_mk_string(ctx, vs, (int)(ve - vs)));
          js_array_push(ctx, list, js_obj_value(pair));
          gc_pop_scope();
        }
      }
    }
    p = eol + 1;
  }
  gc_pop_scope();
  return js_obj_value(list);
}

/* Build a Response on g_response_proto from raw bytes. `hdrs` is anything new Headers() takes (or undefined); `ctype`
 * becomes the Content-Type unless those already carry one. */
js_object *make_response(js_ctx *ctx, const char *url, int status, const char *statustext, js_value hdrs,
                         const char *ctype, const char *body, int len)
{
  js_object *resp;
  js_value h;
  gc_push_scope();
  resp = (js_object *)gc_keep(js_new_object(ctx, g_response_proto));
  js_def(resp, "@body", js_mk_string(ctx, body ? body : "", len < 0 ? 0 : len), 0);
  js_def(resp, "status", js_number(status ? status : 200), P_ENUMERABLE);
  js_def(resp, "ok", js_bool(status >= 200 && status < 300), P_ENUMERABLE);
  js_def(resp, "statusText", js_mk_cstring(ctx, statustext ? statustext : ""), P_ENUMERABLE);
  js_def(resp, "url", js_mk_cstring(ctx, url ? url : ""), P_ENUMERABLE);
  js_def(resp, "redirected", js_bool(0), P_ENUMERABLE);
  js_def(resp, "type", js_mk_cstring(ctx, "basic"), P_ENUMERABLE);
  js_def(resp, "bodyUsed", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  h = headers_new(ctx, hdrs);
  if (h.type == JV_OBJ) {
    gc_keep(h.u.obj);
    if (ctype && ctype[0] && headers_ctype(ctx, h).type != JV_STR) {
      js_value kv[2];
      kv[0] = js_mk_cstring(ctx, "content-type");
      kv[1] = js_mk_cstring(ctx, ctype);
      js_call(ctx, js_get(ctx, h, "set"), h, kv, 2);
    }
    js_def(resp, "headers", h, P_ENUMERABLE);
  }
  gc_pop_scope();
  return resp;
}

/* A Response from a body value and a ResponseInit; `default_ctype` replaces the one the body implies, when given */
static js_value response_build(js_ctx *ctx, js_value body, js_value init, const char *default_ctype)
{
  int status = 200;
  char st[64], ct[160];
  js_value stream = js_undef(), bstr, rv, hinit = js_undef();
  js_object *resp;
  strlcpy(st, "", sizeof st);
  strlcpy(ct, "text/plain;charset=UTF-8", sizeof ct);
  gc_push_scope();
  bstr = response_init_body(ctx, body, &stream, ct, sizeof ct);
  if (default_ctype)
    strlcpy(ct, default_ctype, sizeof ct);
  if (stream.type == JV_OBJ)
    gc_keep(stream.u.obj);
  if (bstr.type == JV_STR)
    gc_keep(bstr.u.str);
  if (init.type == JV_OBJ) {
    js_value s = js_get(ctx, init, "status");
    js_value stt = js_get(ctx, init, "statusText");
    if (s.type != JV_UNDEF) {
      double d = js_to_number(ctx, s);
      if (!(d >= 200 && d <= 599)) {
        gc_pop_scope();
        return js_throw_range(ctx, "The status provided is outside the range [200, 599].");
      }
      status = (int)d;
    }
    if (stt.type == JV_STR)
      strlcpy(st, stt.u.str->data, sizeof st);
    hinit = js_get(ctx, init, "headers");
  }
  resp = (js_object *)gc_keep(make_response(ctx, "", status, st, hinit, ct, bstr.type == JV_STR ? bstr.u.str->data : "",
                                            bstr.type == JV_STR ? bstr.u.str->len : 0));
  js_def(resp, "type", js_mk_cstring(ctx, "default"), P_ENUMERABLE);
  if (stream.type == JV_OBJ)
    js_def(resp, "@source", stream, 0);
  rv = js_obj_value(resp);
  gc_pop_scope();
  return rv;
}

js_value response_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)argc;
  return response_build(ctx, A(0), A(1), 0);
}

/* Response.error(): a network error, status 0 */
js_value response_static_error(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *r;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  r = (js_object *)gc_keep(make_response(ctx, "", 200, "", js_undef(), "", "", 0));
  js_def(r, "status", js_number(0), P_ENUMERABLE);
  js_def(r, "ok", js_bool(0), P_ENUMERABLE);
  js_def(r, "type", js_mk_cstring(ctx, "error"), P_ENUMERABLE);
  rv = js_obj_value(r);
  gc_pop_scope();
  return rv;
}

/* Response.json(data, init): the JSON text of data as an application/json body */
js_value response_static_json(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value json = js_get(ctx, js_obj_value(ctx->global), "JSON"), text, rv;
  (void)t;
  (void)argc;
  text = js_call(ctx, js_get(ctx, json, "stringify"), json, args, argc > 0 ? 1 : 0);
  if (ctx->has_exc)
    return js_undef();
  if (text.type != JV_STR)
    return js_throw_type(ctx, "Response.json: the value is not serializable");
  gc_push_scope();
  gc_keep(text.u.str);
  rv = response_build(ctx, text, A(1), "application/json");
  gc_pop_scope();
  return rv;
}

/* Response.redirect(url, status = 302) */
js_value response_static_redirect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX];
  int status = argc > 1 && A(1).type != JV_UNDEF ? js_to_int_sat(ctx, A(1)) : 302;
  js_object *r;
  js_value rv, kv[2], h;
  (void)t;
  if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
    return js_throw_range(ctx, "Invalid status code");
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  gc_push_scope();
  r = (js_object *)gc_keep(make_response(ctx, "", status, "", js_undef(), "", "", 0));
  js_def(r, "type", js_mk_cstring(ctx, "default"), P_ENUMERABLE);
  h = js_get(ctx, js_obj_value(r), "headers");
  kv[0] = js_mk_cstring(ctx, "location");
  kv[1] = js_mk_cstring(ctx, url);
  js_call(ctx, js_get(ctx, h, "set"), h, kv, 2);
  rv = js_obj_value(r);
  gc_pop_scope();
  return rv;
}

/* The controlling service worker (if any) answers a fetch() from the page */

const char *const fetch_cache_names[] = {"default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached"};

/* The `cache` mode of fetch/Request(input, init): init wins over a Request input. -1 with *err on a bad value. */
int fetch_cache_mode(js_ctx *ctx, js_value input, js_value init, const char **err)
{
  js_value cv = init.type == JV_OBJ ? js_get(ctx, init, "cache") : js_undef();
  js_value mv = init.type == JV_OBJ ? js_get(ctx, init, "mode") : js_undef();
  int mode = HC_DEFAULT;
  if (input.type == JV_OBJ) {
    if (cv.type == JV_UNDEF)
      cv = js_get(ctx, input, "cache");
    if (mv.type == JV_UNDEF)
      mv = js_get(ctx, input, "mode");
  }
  if (cv.type != JV_UNDEF) {
    const char *s = arg_str(ctx, cv);
    for (mode = 0; mode < (int)(sizeof fetch_cache_names / sizeof *fetch_cache_names); mode++)
      if (streq(s, fetch_cache_names[mode]))
        break;
    if (mode == (int)(sizeof fetch_cache_names / sizeof *fetch_cache_names)) {
      *err = "Invalid cache mode";
      return -1;
    }
  }
  if (mode == HC_ONLY_IF_CACHED && !(mv.type == JV_STR && streq(mv.u.str->data, "same-origin"))) {
    *err = "'only-if-cached' can be set only with 'same-origin' mode";
    return -1;
  }
  return mode;
}

static const char *status_text(int status)
{
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 303:
    return "See Other";
  case 304:
    return "Not Modified";
  case 307:
    return "Temporary Redirect";
  case 308:
    return "Permanent Redirect";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 410:
    return "Gone";
  case 429:
    return "Too Many Requests";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  case 504:
    return "Gateway Timeout";
  default:
    return "";
  }
}

int fetch_req_header(struct fetch_req *rq, const char *name, const char *value)
{
  int need;
  char *ne;
  if (streq_ci(name, "content-type")) {
    strlcpy(rq->ctype, value, sizeof rq->ctype);
    return 0;
  }
  need = (int)(strlen(name) + strlen(value)) + 5;
  ne = (char *)realloc(rq->extra_hdrs, (unsigned)(rq->extra_len + need));
  if (!ne)
    return -1;
  rq->extra_hdrs = ne;
  rq->extra_len += snprintf(ne + rq->extra_len, (unsigned)need, "%s: %s\r\n", name, value);
  return 0;
}

void fetch_req_free(struct fetch_req *rq)
{
  free(rq->extra_hdrs);
  rq->extra_hdrs = 0;
}

int fetch_csp(char *url, int cap, const char *what)
{
  char use[URL_MAX], reason[64];
  if (!B->pg->csp)
    return 1;
  if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
    js_csp_log(what, url, reason);
    return 0;
  }
  strlcpy(url, use, cap);
  return 1;
}

int fetch_csp_ws(const char *url)
{
  char norm[URL_MAX], use[URL_MAX], reason[64];
  int k = 0;
  if (!B->pg->csp)
    return 1;
  if (starts_ci(url, "wss://")) { /* connect-src matches sockets by their http(s) twin */
    sapp(norm, &k, sizeof norm, "https://");
    sapp(norm, &k, sizeof norm, url + 6);
  } else if (starts_ci(url, "ws://")) {
    sapp(norm, &k, sizeof norm, "http://");
    sapp(norm, &k, sizeof norm, url + 5);
  } else {
    sapp(norm, &k, sizeof norm, url);
  }
  if (csp_check_url(B->pg->csp, CSP_CONNECT, norm, g_page_url_buf, use, sizeof use, reason, sizeof reason))
    return 1;
  js_csp_log("WebSocket to", url, reason);
  return 0;
}

int fetch_run(const struct fetch_req *rq, struct fetch_res *rs)
{
  struct http_req hq = {0};
  struct http_resp hs = {0};
  hq.url = rq->url;
  hq.method = rq->method[0] ? rq->method : "GET";
  hq.body = rq->body;
  hq.body_len = rq->body_len;
  hq.ctype = rq->ctype[0] ? rq->ctype : 0;
  hq.extra_hdrs = rq->extra_hdrs;
  hq.cache_mode = rq->cache_mode;
  hs.final_url = rs->final_url;
  hs.final_cap = sizeof rs->final_url;
  hs.hdr = rs->hdr;
  hs.hdr_cap = sizeof rs->hdr;
  rs->final_url[0] = 0;
  rs->hdr[0] = 0;
  rs->status_text[0] = 0;
  if (starts_ci(rq->url, "data:")) {
    rs->body = decode_data_url(rq->url, &hs.len, hs.ctype, sizeof hs.ctype);
    hs.status = rs->body ? 200 : 0;
    snprintf(rs->hdr, sizeof rs->hdr, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n\r\n", hs.ctype);
  } else if (starts_ci(rq->url, "blob:")) {
    rs->body = browser_fetch(&hq, &hs);
  } else {
    rs->body = browser_cors_fetch(g_page_url_buf, rq->with_creds, &hq, &hs);
  }
  rs->status = hs.status;
  rs->len = hs.len;
  strlcpy(rs->ctype, hs.ctype, sizeof rs->ctype);
  if (!rs->body)
    return net_aborted() ? FETCH_ABORTED : FETCH_FAILED;
  {
    const char *sp = strchr(rs->hdr, ' '), *eol = strchr(rs->hdr, '\n');
    if (sp && eol && sp < eol && (sp = strchr(sp + 1, ' ')) && sp < eol) { /* "HTTP/1.1 200 OK": the reason phrase */
      int l = (int)(eol - sp - 1);
      while (l > 0 && (sp[l] == '\r' || sp[l] == ' '))
        l--;
      if (l > 0 && l < (int)sizeof rs->status_text) {
        memcpy(rs->status_text, sp + 1, (unsigned)l);
        rs->status_text[l] = 0;
      }
    }
    if (!rs->status_text[0])
      strlcpy(rs->status_text, status_text(rs->status), sizeof rs->status_text);
  }
  return FETCH_OK;
}

unsigned char *fetch_get(const char *url, int *len)
{
  struct fetch_req rq = {.method = "GET"};
  struct fetch_res *rs = malloc(sizeof *rs);
  unsigned char *body = 0;
  if (!rs)
    return 0;
  strlcpy(rq.url, url, sizeof rq.url);
  if (fetch_run(&rq, rs) == FETCH_OK && rs->status >= 200 && rs->status < 300) {
    body = rs->body;
    *len = rs->len;
  } else {
    free(rs->body);
  }
  free(rs);
  return body;
}

/* Add every header of `src` (anything new Headers() takes) to the request; -1 with the error in *exc */
static int fetch_req_add_headers(js_ctx *ctx, struct fetch_req *rq, js_value src, js_value *exc)
{
  js_value h = headers_new(ctx, src), map, order;
  unsigned i;
  int rc = 0;
  if (ctx->has_exc) {
    *exc = ctx->exc;
    ctx->has_exc = 0;
    ctx->exc_root = 0;
    return -1;
  }
  map = h.type == JV_OBJ ? js_get(ctx, h, "@map") : js_undef();
  order = h.type == JV_OBJ ? js_get(ctx, h, "@order") : js_undef();
  if (map.type != JV_OBJ || order.type != JV_OBJ)
    return 0;
  gc_push_scope();
  gc_keep(h.u.obj);
  for (i = 0; rc == 0 && i < js_array_len(order.u.obj); i++) {
    js_value k = js_get_index(ctx, order, i), v = k.type == JV_STR ? js_get(ctx, map, k.u.str->data) : js_undef();
    if (v.type == JV_STR && fetch_req_header(rq, k.u.str->data, v.u.str->data) < 0) {
      *exc = js_make_error(ctx, ctx->type_error_proto, "TypeError", "Out of memory");
      rc = -1;
    }
  }
  gc_pop_scope();
  return rc;
}

js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct fetch_req rq = {.method = "GET"};
  struct fetch_res *rs;
  js_object *resp;
  js_value rv, in = A(0), init = A(1), hsrc, exc;
  const char *s, *cache_err;
  (void)t;
  if (in.type == JV_OBJ) { /* input is a URL string or a Request object (fetch(new Request(...))) */
    js_value u = js_get(ctx, in, "url");
    s = (u.type == JV_STR) ? u.u.str->data : arg_str(ctx, in);
  } else {
    s = arg_str(ctx, in);
  }
  if (starts_ci(s, "blob:"))
    strlcpy(rq.url, s, sizeof rq.url);
  else
    resolve_url(g_page_url_buf, s, rq.url, sizeof rq.url);
  if (!fetch_csp(rq.url, sizeof rq.url, "connection to"))
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Refused to connect (Content-Security-Policy)"));
  if (in.type == JV_OBJ) { /* method carried on a Request */
    js_value m = js_get(ctx, in, "method");
    if (m.type == JV_STR)
      strlcpy(rq.method, m.u.str->data, sizeof rq.method);
  }
  hsrc = init.type == JV_OBJ ? js_get(ctx, init, "headers") : js_undef();
  if (hsrc.type == JV_UNDEF && in.type == JV_OBJ)
    hsrc = js_get(ctx, in, "headers"); /* the headers of a Request input */
  if (hsrc.type != JV_UNDEF && fetch_req_add_headers(ctx, &rq, hsrc, &exc) < 0) {
    fetch_req_free(&rq);
    return js_promise_rejected(ctx, exc);
  }
  if (init.type == JV_OBJ || in.type == JV_OBJ) {
    js_value m = init.type == JV_OBJ ? js_get(ctx, init, "method") : js_undef();
    js_value b = init.type == JV_OBJ ? js_get(ctx, init, "body") : js_undef();
    js_value sig = init.type == JV_OBJ ? js_get(ctx, init, "signal") : js_undef();
    js_value cr = init.type == JV_OBJ ? js_get(ctx, init, "credentials") : js_undef();
    if (sig.type == JV_UNDEF && in.type == JV_OBJ)
      sig = js_get(ctx, in, "signal");
    if (cr.type == JV_UNDEF && in.type == JV_OBJ)
      cr = js_get(ctx, in, "credentials");
    if (m.type == JV_STR)
      strlcpy(rq.method, m.u.str->data, sizeof rq.method);
    rq.with_creds = cr.type == JV_STR && streq(cr.u.str->data, "include"); /* "include" sends cookies cross-origin */
    /* An already-aborted signal short-circuits before touching the network */
    if (sig.type == JV_OBJ && js_to_boolean(js_get(ctx, sig, "aborted"))) {
      js_value reason = js_get(ctx, sig, "reason");
      fetch_req_free(&rq);
      if (reason.type == JV_UNDEF)
        reason = dom_exception(ctx, "AbortError", "The operation was aborted");
      return js_promise_rejected(ctx, reason);
    }
    /* Body last, just before the request: body_bytes may hand back a pointer into a transient string */
    if (b.type != JV_UNDEF && b.type != JV_NULL)
      rq.body = (const unsigned char *)body_bytes(ctx, b, &rq.body_len, rq.ctype, sizeof rq.ctype);
  }
  if (!rq.body && in.type == JV_OBJ) { /* the body of a Request input (its headers carry the type) */
    js_value b = js_get(ctx, in, "@body");
    if (b.type == JV_STR && b.u.str->len) {
      rq.body = (const unsigned char *)b.u.str->data;
      rq.body_len = b.u.str->len;
      js_set(ctx, in, "bodyUsed", js_bool(1));
    }
  }
  rq.cache_mode = fetch_cache_mode(ctx, in, init, &cache_err);
  if (rq.cache_mode < 0) {
    fetch_req_free(&rq);
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", cache_err));
  }
  /* A controlling service worker gets first crack at the request */
  if (sw_intercept_fetch(ctx, rq.url, rq.method, &rv)) {
    fetch_req_free(&rq);
    return rv;
  }
  rs = malloc(sizeof *rs);
  if (!rs) {
    fetch_req_free(&rq);
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Out of memory"));
  }
  switch (fetch_run(&rq, rs)) {
  case FETCH_ABORTED:
    rv = js_promise_rejected(ctx, dom_exception(ctx, "AbortError", "The user aborted a request."));
    break;
  case FETCH_FAILED:
    rv = js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Failed to fetch"));
    break;
  default:
    gc_push_scope();
    resp = (js_object *)gc_keep(make_response(ctx, rs->final_url[0] ? rs->final_url : rq.url, rs->status,
                                              rs->status_text, parse_header_block(ctx, rs->hdr), rs->ctype,
                                              (const char *)rs->body, rs->len));
    rv = js_promise_resolved(ctx, js_obj_value(resp));
    gc_pop_scope();
  }
  free(rs->body);
  free(rs);
  fetch_req_free(&rq);
  return rv;
}
