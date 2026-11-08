/* Blob, blob: object URLs and navigator.sendBeacon. */

#include "bindings.h"

/* ------------------------------------------------------- Blob + sendBeacon */

/* Append the raw bytes of one Blob "part" */
static void blob_append_part(js_ctx *ctx, sbuf *sb, js_value v)
{
  if (v.type == JV_STR) {
    sb_putn(sb, v.u.str->data, v.u.str->len);
    return;
  }
  if (v.type == JV_OBJ) {
    js_object *o = v.u.obj;
    if (o->proto == g_blob_proto) {
      js_value b = js_get(ctx, v, "@bytes");
      if (b.type == JV_STR)
        sb_putn(sb, b.u.str->data, b.u.str->len);
      return;
    }
    if (o->kind == OBJ_ARRAYBUFFER && o->host) {
      js_arraybuffer *ab = (js_arraybuffer *)o->host;
      sb_putn(sb, (const char *)ab->data, ab->byteLength);
      return;
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host) {
      js_typedarray *ta = (js_typedarray *)o->host;
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      sb_putn(sb, (const char *)ab->data + ta->offset, ta->length * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type));
      return;
    }
  }
  {
    js_value s = js_to_string(ctx, v);
    if (s.type == JV_STR)
      sb_putn(sb, s.u.str->data, s.u.str->len);
  }
}

/* Make a Blob object holding `data[0..len)` with content type `type`. */
js_value make_blob(js_ctx *ctx, const char *data, int len, const char *type)
{
  js_object *o;
  js_value ov;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_blob_proto));
  js_def(o, "@bytes", js_mk_string(ctx, data ? data : "", len), 0);
  js_def(o, "size", js_number(len), P_ENUMERABLE);
  js_def(o, "type", js_mk_cstring(ctx, type ? type : ""), P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
js_value blob_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  sbuf sb;
  js_value parts = A(0), rv;
  char type[128];
  (void)thisv;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  type[0] = '\0';
  gc_push_scope();
  if (parts.type == JV_OBJ) { /* an array (or array-like) of parts */
    unsigned i,
        n = parts.u.obj->kind == OBJ_ARRAY ? parts.u.obj->elen : js_to_uint32(ctx, js_get(ctx, parts, "length"));
    for (i = 0; i < n; i++)
      blob_append_part(ctx, &sb, js_get_index(ctx, parts, i));
  }
  if (argc >= 2 && A(1).type == JV_OBJ) {
    js_value tp = js_get(ctx, A(1), "type");
    if (tp.type == JV_STR)
      strlcpy(type, tp.u.str->data, sizeof type);
  }
  rv = make_blob(ctx, sb.b ? sb.b : "", sb.n, type);
  if (sb.b)
    free(sb.b);
  gc_pop_scope();
  return rv;
}
js_value blob_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_promise_resolved(ctx, js_get(ctx, t, "@bytes"));
}
js_value blob_arraybuffer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b = js_get(ctx, t, "@bytes"), u8ctor, lenarg, view, buf;
  int i, len;
  (void)args;
  (void)argc;
  if (b.type != JV_STR)
    return js_promise_resolved(ctx, js_undef());
  len = b.u.str->len;
  gc_push_scope();
  gc_keep(b.u.str);
  u8ctor = js_get(ctx, js_obj_value(ctx->global), "Uint8Array");
  lenarg = js_number(len);
  view = js_construct(ctx, u8ctor, &lenarg, 1); /* a fresh Uint8Array(len) */
  if (view.type == JV_OBJ) {
    gc_keep(view.u.obj);
    for (i = 0; i < len; i++)
      js_set_index(ctx, view, (unsigned)i, js_number((unsigned char)b.u.str->data[i]));
  }
  buf = js_get(ctx, view, "buffer");
  gc_pop_scope();
  return js_promise_resolved(ctx, buf);
}
js_value blob_slice(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b = js_get(ctx, t, "@bytes");
  int len = b.type == JV_STR ? b.u.str->len : 0;
  int s = argc > 0 && A(0).type != JV_UNDEF ? js_to_int_sat(ctx, A(0)) : 0;
  int e = argc > 1 && A(1).type != JV_UNDEF ? js_to_int_sat(ctx, A(1)) : len;
  char type[128];
  type[0] = '\0';
  if (s < 0)
    s += len;
  if (s < 0)
    s = 0;
  else if (s > len)
    s = len;
  if (e < 0)
    e += len;
  if (e < 0)
    e = 0;
  else if (e > len)
    e = len;
  if (e < s)
    e = s;
  if (argc > 2 && A(2).type == JV_STR)
    strlcpy(type, A(2).u.str->data, sizeof type);
  return make_blob(ctx, b.u.str->data + s, e - s, type);
}

/* ------------------------------------------------------- blob: URLs */

js_value url_create_object_url(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  static const char hx[] = "0123456789abcdef";
  js_value loc, org, bytes, type;
  char url[URL_MAX], id[37];
  unsigned char b[16];
  int i, p = 0, n = 0;
  (void)t;
  if (argc < 1 || A(0).type != JV_OBJ || A(0).u.obj->proto != g_blob_proto)
    return js_throw_type(ctx, "URL.createObjectURL: argument must be a Blob");
  rng_bytes(b, 16);
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  for (i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      id[p++] = '-';
    id[p++] = hx[b[i] >> 4];
    id[p++] = hx[b[i] & 15];
  }
  id[p] = '\0';
  loc = js_get(ctx, js_obj_value(ctx->global), "location");
  org = loc.type == JV_OBJ ? js_get(ctx, loc, "origin") : js_undef();
  sapp(url, &n, sizeof url, "blob:");
  if (org.type == JV_STR && !streq(org.u.str->data, "null"))
    sapp(url, &n, sizeof url, org.u.str->data);
  sapp(url, &n, sizeof url, "/");
  sapp(url, &n, sizeof url, id);
  bytes = js_get(ctx, A(0), "@bytes");
  type = js_get(ctx, A(0), "type");
  if (bytes.type != JV_STR || !blobstore_put(url, (const unsigned char *)bytes.u.str->data, bytes.u.str->len,
                                             type.type == JV_STR ? type.u.str->data : ""))
    return js_throw_type(ctx, "URL.createObjectURL: out of memory");
  return js_mk_cstring(ctx, url);
}

js_value url_revoke_object_url(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (argc >= 1)
    blobstore_del(arg_str(ctx, A(0)));
  return js_undef();
}

/* Extract the request bytes + a default content-type from a body value */
const char *body_bytes(js_ctx *ctx, js_value v, int *len, char *ctype, int ctcap)
{
  if (v.type == JV_STR) {
    *len = v.u.str->len;
    if (ctype && !ctype[0])
      strlcpy(ctype, "text/plain;charset=UTF-8", ctcap);
    return v.u.str->data;
  }
  if (v.type == JV_OBJ) {
    js_object *o = v.u.obj;
    if (o->proto == g_blob_proto) {
      js_value b = js_get(ctx, v, "@bytes"), tp = js_get(ctx, v, "type");
      if (ctype && !ctype[0] && tp.type == JV_STR && tp.u.str->len)
        strlcpy(ctype, tp.u.str->data, ctcap);
      *len = b.type == JV_STR ? b.u.str->len : 0;
      return b.type == JV_STR ? b.u.str->data : "";
    }
    if (o->kind == OBJ_ARRAYBUFFER && o->host) {
      js_arraybuffer *ab = (js_arraybuffer *)o->host;
      *len = ab->byteLength;
      return (const char *)ab->data;
    }
    if (o->kind == OBJ_TYPEDARRAY && o->host) {
      js_typedarray *ta = (js_typedarray *)o->host;
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      *len = ta->length * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
      return (const char *)ab->data + ta->offset;
    }
    if (o->proto == g_sp_proto) {
      js_value s = sp_tostring(ctx, v, 0, 0);
      if (ctype && !ctype[0])
        strlcpy(ctype, "application/x-www-form-urlencoded;charset=UTF-8", ctcap);
      *len = s.type == JV_STR ? s.u.str->len : 0;
      return s.type == JV_STR ? s.u.str->data : "";
    }
    /* FormData: no file input here, so serialize as urlencoded rather than multipart */
    if (o->proto == g_formdata_proto) {
      js_value s = sp_tostring(ctx, v, 0, 0);
      if (ctype && !ctype[0])
        strlcpy(ctype, "application/x-www-form-urlencoded;charset=UTF-8", ctcap);
      *len = s.type == JV_STR ? s.u.str->len : 0;
      return s.type == JV_STR ? s.u.str->data : "";
    }
  }
  *len = 0;
  return "";
}

/* navigator.sendBeacon(url, data) -> boolean */
js_value nav_sendbeacon(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char url[URL_MAX], ctype[160];
  const char *body;
  int len = 0;
  unsigned char *resp;
  js_value data = A(1);
  (void)t;
  if (argc < 1)
    return js_bool(0);
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), url, sizeof url);
  if (B->pg->csp) {
    char use[URL_MAX], reason[64];
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, url, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("beacon to", url, reason);
      return js_bool(0);
    }
    strlcpy(url, use, sizeof url);
  }
  ctype[0] = '\0';
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  else if (data.type == JV_STR)
    gc_keep(data.u.str);
  body = (argc >= 2 && data.type != JV_UNDEF && data.type != JV_NULL) ? body_bytes(ctx, data, &len, ctype, sizeof ctype)
                                                                      : "";
  { /* no-cors fire-and-forget: send and discard, no CORS gating on the reply */
    struct http_req rq = {0};
    struct http_resp rs = {0};
    rq.url = url;
    rq.method = "POST";
    rq.body = (const unsigned char *)body;
    rq.body_len = len;
    rq.ctype = ctype[0] ? ctype : 0;
    resp = browser_fetch(&rq, &rs);
  }
  gc_pop_scope();
  if (resp)
    free(resp);
  return js_bool(resp != 0); /* queued (sent) */
}
