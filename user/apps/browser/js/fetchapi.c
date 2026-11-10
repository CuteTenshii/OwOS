/* fetch(), Headers, Request and Response. */

#include "bindings.h"
#include "fetchint.h"

/* ------------------------------------------------------- fetch ------------- */
js_value resp_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_TEXT);
  return js_promise_resolved(ctx, js_get(ctx, t, "@body"));
}
js_value resp_json(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body = js_get(ctx, t, "@body"), parsed, jsonobj, parsefn;
  (void)args;
  (void)argc;
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_JSON);
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
js_value resp_clone(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return t; /* body isn't consumed here, so self works */
}
js_value hdr_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  (void)argc;
  if (streq_ci(name, "content-type"))
    return js_get(ctx, t, "@ctype");
  return js_null();
}
js_value hdr_has(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = hdr_get(ctx, t, args, argc);
  return js_bool(v.type == JV_STR);
}

/* Build a Response on g_response_proto from raw bytes */
js_object *make_response(js_ctx *ctx, const char *url, int status, const char *statustext, const char *ctype,
                         const char *body, int len)
{
  js_object *resp, *hdrs;
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
  hdrs = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(hdrs, "@ctype", js_mk_cstring(ctx, ctype ? ctype : ""), 0);
  js_def_native(ctx, hdrs, "get", hdr_get, 1);
  js_def_native(ctx, hdrs, "has", hdr_has, 1);
  js_def(resp, "headers", js_obj_value(hdrs), P_ENUMERABLE);
  gc_pop_scope();
  return resp;
}

js_value response_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  int status = 200;
  char st[64], ct[160];
  js_value stream = js_undef(), bstr, rv;
  js_object *resp;
  (void)thisv;
  strlcpy(st, "", sizeof st);
  strlcpy(ct, "text/plain;charset=UTF-8", sizeof ct);
  gc_push_scope();
  bstr = response_init_body(ctx, A(0), &stream, ct, sizeof ct);
  if (stream.type == JV_OBJ)
    gc_keep(stream.u.obj);
  if (bstr.type == JV_STR)
    gc_keep(bstr.u.str);
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
  resp = (js_object *)gc_keep(make_response(ctx, "", status, st, ct, bstr.type == JV_STR ? bstr.u.str->data : "",
                                            bstr.type == JV_STR ? bstr.u.str->len : 0));
  if (stream.type == JV_OBJ)
    js_def(resp, "@source", stream, 0);
  rv = js_obj_value(resp);
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

js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct fetch_req rq = {.method = "GET"};
  struct fetch_res *rs;
  js_object *resp;
  js_value rv, in = A(0), init = A(1);
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
  if (init.type == JV_OBJ) {
    js_value m = js_get(ctx, init, "method");
    js_value b = js_get(ctx, init, "body");
    js_value sig = js_get(ctx, init, "signal");
    js_value h = js_get(ctx, init, "headers");
    js_value cr = js_get(ctx, init, "credentials");
    if (m.type == JV_STR)
      strlcpy(rq.method, m.u.str->data, sizeof rq.method);
    (void)js_to_boolean(js_get(ctx, init, "keepalive")); /* sync fetch: inherently satisfied */
    rq.with_creds = cr.type == JV_STR && streq(cr.u.str->data, "include"); /* "include" sends cookies cross-origin */
    /* Request headers: a plain { name: value } object (a Headers instance is a plain object here too) */
    if (h.type == JV_OBJ) {
      pmap *pm = &h.u.obj->props;
      int i;
      for (i = 0; i < pm->n; i++) {
        const char *k = pm->e[i].key;
        js_value v;
        if (!k || k[0] == '@' || (pm->e[i].flags & P_ACCESSOR))
          continue;
        v = js_to_string(ctx, js_get(ctx, h, k));
        if (v.type == JV_STR && fetch_req_header(&rq, k, v.u.str->data) < 0) {
          fetch_req_free(&rq);
          return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Out of memory"));
        }
      }
    }
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
                                              rs->status_text, rs->ctype, (const char *)rs->body, rs->len));
    rv = js_promise_resolved(ctx, js_obj_value(resp));
    gc_pop_scope();
  }
  free(rs->body);
  free(rs);
  fetch_req_free(&rq);
  return rv;
}
