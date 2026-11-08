/* fetch(), Headers, Request and Response. */

#include "bindings.h"

/* ------------------------------------------------------- fetch ------------- */
js_value resp_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_promise_resolved(ctx, js_get(ctx, t, "@body"));
}
js_value resp_json(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body = js_get(ctx, t, "@body"), parsed, jsonobj, parsefn;
  (void)args;
  (void)argc;
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

/* The controlling service worker (if any) answers a fetch() from the page */

js_value win_fetch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], method[16], req_ctype[64], resp_ctype[160];
  char extra_hdrs[1024];
  const char *reqbody = 0;
  int reqlen = 0, status = 0, outlen = 0, eh = 0;
  bool with_creds = false;
  unsigned char *body;
  js_object *resp;
  js_value rv;
  (void)t;
  { /* input is a URL string or a Request object (fetch(new Request(...))) */
    js_value in = A(0);
    const char *s;
    if (in.type == JV_OBJ) {
      js_value u = js_get(ctx, in, "url");
      s = (u.type == JV_STR) ? u.u.str->data : arg_str(ctx, in);
    } else {
      s = arg_str(ctx, in);
    }
    resolve_url(g_page_url_buf, s, url, sizeof url);
  }
  if (B->pg->csp) { /* CSP connect-src */
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("connection to", url, reason);
      return js_promise_rejected(
          ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Refused to connect (Content-Security-Policy)"));
    }
    strlcpy(url, use, sizeof url); /* use the upgraded URL */
  }
  method[0] = '\0';
  req_ctype[0] = '\0';
  extra_hdrs[0] = '\0';
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value m = js_get(ctx, A(1), "method");
    js_value b = js_get(ctx, A(1), "body");
    js_value sig = js_get(ctx, A(1), "signal");
    js_value h = js_get(ctx, A(1), "headers");
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
    (void)js_to_boolean(js_get(ctx, A(1), "keepalive")); /* sync fetch: inherently satisfied */
    {                                                    /* credentials: "include" sends cookies cross-origin */
      js_value cr = js_get(ctx, A(1), "credentials");
      if (cr.type == JV_STR && streq(cr.u.str->data, "include"))
        with_creds = true;
    }
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
        if (v.type != JV_STR)
          continue;
        if (streq_ci(k, "content-type")) {
          strlcpy(req_ctype, v.u.str->data, sizeof req_ctype);
          continue;
        }
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, k);
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, ": ");
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, v.u.str->data);
        sapp(extra_hdrs, &eh, sizeof extra_hdrs, "\r\n");
      }
    }
    /* An already-aborted signal short-circuits before touching the network */
    if (sig.type == JV_OBJ && js_to_boolean(js_get(ctx, sig, "aborted"))) {
      js_value reason = js_get(ctx, sig, "reason");
      if (reason.type == JV_UNDEF)
        reason = js_make_error(ctx, ctx->error_proto, "AbortError", "The operation was aborted");
      return js_promise_rejected(ctx, reason);
    }
    /* Body last, just before the request: body_bytes may hand back a pointer into a transient string */
    if (b.type != JV_UNDEF && b.type != JV_NULL)
      reqbody = body_bytes(ctx, b, &reqlen, req_ctype,
                           sizeof req_ctype); /* string/Blob/ArrayBuffer/TypedArray/URLSearchParams */
  }
  if (!method[0] && A(0).type == JV_OBJ) { /* method carried on a Request */
    js_value m = js_get(ctx, A(0), "method");
    if (m.type == JV_STR)
      strlcpy(method, m.u.str->data, sizeof method);
  }
  /* A controlling service worker gets first crack at the request */
  if (sw_intercept_fetch(ctx, url, method[0] ? method : "GET", &rv))
    return rv;
  {
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = method[0] ? method : "GET";
    rq.body = (const unsigned char *)reqbody;
    rq.body_len = reqlen;
    rq.ctype = req_ctype[0] ? req_ctype : 0;
    rq.extra_hdrs = extra_hdrs[0] ? extra_hdrs : 0;
    body = browser_cors_fetch(g_page_url_buf, with_creds, &rq, &rs);
    status = rs.status;
    outlen = rs.len;
    strlcpy(resp_ctype, rs.ctype, sizeof resp_ctype);
  }
  if (!body)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "Failed to fetch"));
  gc_push_scope();
  resp = (js_object *)gc_keep(
      make_response(ctx, url, status, status == 200 ? "OK" : "", resp_ctype, (const char *)body, outlen));
  free(body);
  rv = js_promise_resolved(ctx, js_obj_value(resp));
  gc_pop_scope();
  return rv;
}
