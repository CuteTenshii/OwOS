/* XMLHttpRequest. */

#include "bindings.h"

/* ------------------------------------------------------- XMLHttpRequest ---- */

js_value xhr_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char method[16], url[URL_MAX];
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(method, arg_str(ctx, A(0)), sizeof method);
  if (starts_ci(arg_str(ctx, A(1)), "blob:"))
    strlcpy(url, arg_str(ctx, A(1)), sizeof url);
  else
    resolve_url(g_page_url_buf, arg_str(ctx, A(1)), url, sizeof url);
  js_def(t.u.obj, "@method", js_mk_cstring(ctx, method[0] ? method : "GET"), 0);
  js_def(t.u.obj, "@url", js_mk_cstring(ctx, url), 0);
  js_def(t.u.obj, "@hdrs", js_mk_cstring(ctx, ""), 0);
  js_def(t.u.obj, "@ctype", js_mk_cstring(ctx, ""), 0);
  js_set(ctx, t, "readyState", js_number(1)); /* OPENED */
  emit_event(t.u.obj, "readystatechange", js_undef());
  return js_undef();
}
js_value xhr_setheader(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  char val[1024];
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(val, arg_str(ctx, A(1)), sizeof val);
  if (streq_ci(name, "content-type")) {
    js_def(t.u.obj, "@ctype", js_mk_cstring(ctx, val), 0);
    return js_undef();
  }
  { /* append "Name: value\r\n" to the accumulated request-header blob */
    js_value cur = js_get(ctx, t, "@hdrs");
    sbuf sb;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    if (cur.type == JV_STR)
      sb_putn(&sb, cur.u.str->data, cur.u.str->len);
    sb_puts(&sb, name);
    sb_puts(&sb, ": ");
    sb_puts(&sb, val);
    sb_puts(&sb, "\r\n");
    js_def(t.u.obj, "@hdrs", js_mk_string(ctx, sb.b ? sb.b : "", sb.n), 0);
    if (sb.b)
      free(sb.b);
  }
  return js_undef();
}
static js_value xhr_setrt(js_ctx *ctx, js_value t, int state)
{
  js_set(ctx, t, "readyState", js_number(state));
  emit_event(t.u.obj, "readystatechange", js_undef());
  return js_undef();
}
js_value xhr_send(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], method[16], req_ctype[160], resp_ctype[160];
  char *reqbody = 0;
  int reqlen = 0, status = 0, outlen = 0;
  unsigned char *body;
  const char *hdrs;
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(url, arg_str(ctx, js_get(ctx, t, "@url")), sizeof url);
  strlcpy(method, arg_str(ctx, js_get(ctx, t, "@method")), sizeof method);
  strlcpy(req_ctype, arg_str(ctx, js_get(ctx, t, "@ctype")), sizeof req_ctype);
  if (B->pg->csp) { /* CSP connect-src */
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("connection to", url, reason);
      js_set(ctx, t, "status", js_number(0));
      xhr_setrt(ctx, t, 4);
      emit_event(t.u.obj, "error", js_undef());
      emit_event(t.u.obj, "loadend", js_undef());
      return js_undef();
    }
    strlcpy(url, use, sizeof url);
  }
  if (argc >= 1 && A(0).type == JV_STR) {
    reqbody = A(0).u.str->data;
    reqlen = A(0).u.str->len;
    if (!req_ctype[0])
      strlcpy(req_ctype, "text/plain;charset=UTF-8", sizeof req_ctype);
  }
  {
    js_value h = js_get(ctx, t, "@hdrs");
    hdrs = h.type == JV_STR ? h.u.str->data : 0;
  }
  {
    bool with_creds = js_to_boolean(js_get(ctx, t, "withCredentials"));
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = method[0] ? method : "GET";
    rq.body = (const unsigned char *)reqbody;
    rq.body_len = reqlen;
    rq.ctype = req_ctype[0] ? req_ctype : 0;
    rq.extra_hdrs = hdrs && hdrs[0] ? hdrs : 0;
    body = starts_ci(url, "blob:") ? browser_fetch(&rq, &rs) : browser_cors_fetch(g_page_url_buf, with_creds, &rq, &rs);
    status = rs.status;
    outlen = rs.len;
    strlcpy(resp_ctype, rs.ctype, sizeof resp_ctype);
  }
  gc_push_scope();
  gc_keep(t.u.obj);
  js_def(t.u.obj, "@respctype", js_mk_cstring(ctx, resp_ctype), 0);
  js_set(ctx, t, "responseURL", js_mk_cstring(ctx, url));
  if (!body) {
    bool aborted = net_aborted();
    js_set(ctx, t, "status", js_number(0));
    xhr_setrt(ctx, t, 4);
    emit_event(t.u.obj, aborted ? "abort" : "error", js_undef());
    emit_event(t.u.obj, "loadend", js_undef());
    if (aborted)
      js_set(ctx, t, "readyState", js_number(0));
    gc_pop_scope();
    return js_undef();
  }
  {
    js_value text = js_mk_string(ctx, (const char *)body, outlen);
    gc_keep(text.u.str);
    free(body);
    js_set(ctx, t, "status", js_number(status ? status : 200));
    js_set(ctx, t, "statusText", js_mk_cstring(ctx, status == 200 ? "OK" : status == 204 ? "No Content" : ""));
    js_set(ctx, t, "responseText", text);
    { /* .response honours responseType: "json" parses, else the text */
      js_value rt = js_get(ctx, t, "responseType");
      if (rt.type == JV_STR && streq(rt.u.str->data, "json")) {
        js_value jsonobj = js_get(ctx, js_obj_value(ctx->global), "JSON");
        js_value parsefn = js_get(ctx, jsonobj, "parse");
        js_value parsed = js_is_callable(parsefn) ? js_call(ctx, parsefn, jsonobj, &text, 1) : js_null();
        if (ctx->has_exc) {
          ctx->has_exc = 0;
          ctx->exc_root = 0;
          parsed = js_null();
        }
        js_set(ctx, t, "response", parsed);
      } else {
        js_set(ctx, t, "response", text);
      }
    }
    xhr_setrt(ctx, t, 2); /* HEADERS_RECEIVED */
    xhr_setrt(ctx, t, 3); /* LOADING */
    xhr_setrt(ctx, t, 4); /* DONE */
    emit_event(t.u.obj, "load", js_undef());
    emit_event(t.u.obj, "loadend", js_undef());
  }
  gc_pop_scope();
  return js_undef();
}
js_value xhr_abort(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ && js_to_number(ctx, js_get(ctx, t, "readyState")) == 4) /* DONE: back to UNSENT, no events */
    js_set(ctx, t, "readyState", js_number(0));
  return js_undef();
}
js_value xhr_getheader(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  if (t.type == JV_OBJ && streq_ci(name, "content-type")) {
    js_value c = js_get(ctx, t, "@respctype");
    if (c.type == JV_STR && c.u.str->len)
      return c;
  }
  return js_null();
}
js_value xhr_getallheaders(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value c = t.type == JV_OBJ ? js_get(ctx, t, "@respctype") : js_undef();
  (void)args;
  (void)argc;
  if (c.type == JV_STR && c.u.str->len) {
    sbuf sb;
    js_value r;
    sb.b = 0;
    sb.n = 0;
    sb.cap = 0;
    sb_puts(&sb, "content-type: ");
    sb_putn(&sb, c.u.str->data, c.u.str->len);
    sb_puts(&sb, "\r\n");
    r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
    if (sb.b)
      free(sb.b);
    return r;
  }
  return js_mk_cstring(ctx, "");
}
js_value xhr_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  js_value ov;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_xhr_proto));
  js_def(o, "readyState", js_number(0), P_WRITABLE | P_ENUMERABLE); /* UNSENT */
  js_def(o, "status", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "statusText", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseText", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "response", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseType", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "responseURL", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "timeout", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "withCredentials", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onreadystatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onload", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onabort", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onloadend", js_null(), P_WRITABLE | P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
