/* Blob, blob: object URLs and navigator.sendBeacon. */

#include "bindings.h"
#include "mime.h"

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
    if (is_blob(o)) {
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
      sb_putn(sb, (const char *)ab->data + ta->offset, ta_length(ta) * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type));
      return;
    }
  }
  {
    js_value s = js_to_string(ctx, v);
    if (s.type == JV_STR)
      sb_putn(sb, s.u.str->data, s.u.str->len);
  }
}

/* structured clone of a page Blob: its bytes travel with its type */
int blob_clone_write(js_ctx *ctx, js_value v, char kind[16], unsigned char **bytes, int *len, char **meta)
{
  js_value b, type;
  if (!is_blob(v.u.obj))
    return 0;
  b = js_get(ctx, v, "@bytes");
  type = js_get(ctx, v, "type");
  if (b.type != JV_STR)
    return 0;
  *bytes = (unsigned char *)malloc((unsigned)b.u.str->len + 1);
  if (!*bytes)
    return 0;
  memcpy(*bytes, b.u.str->data, (unsigned)b.u.str->len);
  *len = b.u.str->len;
  *meta = type.type == JV_STR ? strdup(type.u.str->data) : 0;
  strcpy(kind, "blob");
  return 1;
}

js_value blob_clone_read(js_ctx *ctx, const char *kind, const unsigned char *bytes, int len, const char *meta)
{
  if (!streq(kind, "blob"))
    return js_undef();
  return make_blob(ctx, (const char *)bytes, len, meta);
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
bool is_blob(const js_object *o) { return o->proto == g_blob_proto || o->proto == g_file_proto; }

/* A File: a Blob with a name, a modification time and the path inside a picked folder */
js_value make_file(js_ctx *ctx, const char *data, int len, const char *type, const char *name, const char *rel,
                   double modified)
{
  js_value b = make_blob(ctx, data, len, type);
  gc_push_scope();
  gc_keep(b.u.obj);
  b.u.obj->proto = g_file_proto;
  js_def(b.u.obj, "name", js_mk_cstring(ctx, name), P_ENUMERABLE);
  js_def(b.u.obj, "lastModified", js_number(modified), P_ENUMERABLE);
  js_def(b.u.obj, "webkitRelativePath", js_mk_cstring(ctx, rel), P_ENUMERABLE);
  gc_pop_scope();
  return b;
}

/* new File(parts, name, {type}) */
js_value file_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value blobargs[2] = {A(0), argc > 2 ? A(2) : js_undef()};
  js_value b = blob_ctor(ctx, thisv, blobargs, argc > 2 ? 2 : 1), bytes, type, f;
  char name[256];
  double modified;
  strlcpy(name, arg_str(ctx, A(1)), sizeof name);
  modified = (double)now_ms();
  if (argc > 2 && A(2).type == JV_OBJ) {
    js_value lm = js_get(ctx, A(2), "lastModified");
    if (lm.type == JV_NUM)
      modified = js_to_number(ctx, lm);
  }
  gc_push_scope();
  gc_keep(b.u.obj);
  bytes = js_get(ctx, b, "@bytes");
  type = js_get(ctx, b, "type");
  f = make_file(ctx, bytes.u.str->data, bytes.u.str->len, type.type == JV_STR ? type.u.str->data : "", name, "",
                modified);
  gc_pop_scope();
  return f;
}

js_value filelist_item(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  unsigned i = js_to_uint32(ctx, A(0));
  js_object *o = t.type == JV_OBJ ? t.u.obj : 0;
  return o && i < js_array_len(o) ? js_get_index(ctx, t, i) : js_null();
}

#define FILE_READ_MAX (32 << 20)

/* owofs packs a UTC stamp as year-2000:6 month:4 day:5 hour:5 min:6 sec:6; 0 means unknown */
static double mtime_ms(unsigned m)
{
  if (!m)
    return 0;
  int y = 2000 + (int)(m >> 26), mo = (int)((m >> 22) & 0xF), d = (int)((m >> 17) & 0x1F);
  y -= mo <= 2;
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long days = (long long)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  long long secs = days * 86400 + (long long)((m >> 12) & 0x1F) * 3600 + (long long)((m >> 6) & 0x3F) * 60 + (m & 0x3F);
  return (double)secs * 1000;
}

js_value dom_file_value(js_ctx *ctx, const struct dom_files *f, int i)
{
  struct statinfo st = {0};
  char *data = 0;
  int len = 0;
  js_value r;
  if (!f)
    return make_file(ctx, "", 0, "application/octet-stream", "", "", (double)now_ms());
  if (stat(f->path[i], &st) == 0 && !st.is_dir && st.size <= FILE_READ_MAX && (data = malloc(st.size + 1))) {
    len = readfile_at(f->path[i], 0, data, st.size);
    if (len < 0)
      len = 0;
  }
  const char *name = f->path[i];
  for (const char *q = name; *q; q++)
    if (*q == '/')
      name = q + 1;
  r = make_file(ctx, data ? data : "", len, mime_from_ext(f->path[i]), name, f->rel[i], mtime_ms(st.mtime));
  free(data);
  return r;
}

void js_files_changed(struct dom_node *input)
{
  if (input->wrapper)
    js_def((js_object *)input->wrapper, "@files", js_undef(), 0);
}

/* input.files: the same FileList until the selection changes */
js_value input_get_files(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value cached, list;
  const char *type;
  (void)args;
  (void)argc;
  type = n ? dom_get_attr(n, "type") : 0;
  if (!n || !type || !streq_ci(type, "file"))
    return js_null();
  cached = js_get(ctx, t, "@files");
  if (cached.type == JV_OBJ)
    return cached;
  gc_push_scope();
  list = js_obj_value((js_object *)gc_keep(js_new_array(ctx, 0)));
  list.u.obj->proto = g_filelist_proto;
  for (int i = 0; n->files && i < n->files->n; i++)
    js_array_push(ctx, list.u.obj, dom_file_value(ctx, n->files, i));
  js_def(t.u.obj, "@files", list, 0);
  gc_pop_scope();
  return list;
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
  if (argc < 1 || A(0).type != JV_OBJ || !is_blob(A(0).u.obj))
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

static bool formdata_has_blob(js_ctx *ctx, js_value fd)
{
  js_value va = js_get(ctx, fd, "@v");
  if (va.type != JV_OBJ)
    return false;
  for (unsigned i = 0, n = js_array_len(va.u.obj); i < n; i++) {
    js_value v = js_get_index(ctx, va, i);
    if (v.type == JV_OBJ)
      return true;
  }
  return false;
}

static void put_quoted(sbuf *sb, const char *s)
{
  sb_putc(sb, '"');
  for (; *s; s++) {
    if (*s == '"')
      sb_puts(sb, "%22");
    else if (*s == '\r')
      sb_puts(sb, "%0D");
    else if (*s == '\n')
      sb_puts(sb, "%0A");
    else
      sb_putc(sb, *s);
  }
  sb_putc(sb, '"');
}

/* The multipart/form-data body of a FormData holding files. It is kept on the FormData ("@mp") so the bytes
 * outlive the call, with its Content-Type in "@ct". */
static js_value formdata_multipart(js_ctx *ctx, js_value fd)
{
  js_value ka = js_get(ctx, fd, "@k"), va = js_get(ctx, fd, "@v"), mp;
  char boundary[48], ct[96];
  unsigned r[2];
  sbuf sb = {0, 0, 0};
  rng_bytes(r, sizeof r);
  snprintf(boundary, sizeof boundary, "----OwOSFormBoundary%08x%08x", r[0], r[1]);
  for (unsigned i = 0, n = js_array_len(ka.u.obj); i < n; i++) {
    js_value k = js_get_index(ctx, ka, i), v = js_get_index(ctx, va, i);
    sb_puts(&sb, "--");
    sb_puts(&sb, boundary);
    sb_puts(&sb, "\r\nContent-Disposition: form-data; name=");
    put_quoted(&sb, k.type == JV_STR ? k.u.str->data : "");
    if (v.type == JV_OBJ && is_blob(v.u.obj)) {
      js_value nm = js_get(ctx, v, "name"), tp = js_get(ctx, v, "type"), b = js_get(ctx, v, "@bytes");
      sb_puts(&sb, "; filename=");
      put_quoted(&sb, nm.type == JV_STR ? nm.u.str->data : "blob");
      sb_puts(&sb, "\r\nContent-Type: ");
      sb_puts(&sb, tp.type == JV_STR && tp.u.str->len ? tp.u.str->data : "application/octet-stream");
      sb_puts(&sb, "\r\n\r\n");
      if (b.type == JV_STR)
        sb_putn(&sb, b.u.str->data, b.u.str->len);
    } else {
      js_value s = js_to_string(ctx, v);
      sb_puts(&sb, "\r\n\r\n");
      sb_puts(&sb, s.type == JV_STR ? s.u.str->data : "");
    }
    sb_puts(&sb, "\r\n");
  }
  sb_puts(&sb, "--");
  sb_puts(&sb, boundary);
  sb_puts(&sb, "--\r\n");
  snprintf(ct, sizeof ct, "multipart/form-data; boundary=%s", boundary);
  gc_push_scope();
  mp = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  gc_keep(mp.u.str);
  js_def(fd.u.obj, "@mp", mp, 0);
  js_def(fd.u.obj, "@ct", js_mk_cstring(ctx, ct), 0);
  gc_pop_scope();
  free(sb.b);
  return mp;
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
    if (is_blob(o)) {
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
      *len = ta_length(ta) * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
      return (const char *)ab->data + ta->offset;
    }
    if (o->proto == g_sp_proto) {
      js_value s = sp_tostring(ctx, v, 0, 0);
      if (ctype && !ctype[0])
        strlcpy(ctype, "application/x-www-form-urlencoded;charset=UTF-8", ctcap);
      *len = s.type == JV_STR ? s.u.str->len : 0;
      return s.type == JV_STR ? s.u.str->data : "";
    }
    if (o->proto == g_formdata_proto && formdata_has_blob(ctx, v)) {
      js_value mp = formdata_multipart(ctx, v);
      js_value ct = js_get(ctx, v, "@ct");
      if (ctype && !ctype[0] && ct.type == JV_STR)
        strlcpy(ctype, ct.u.str->data, ctcap);
      *len = mp.type == JV_STR ? mp.u.str->len : 0;
      return mp.type == JV_STR ? mp.u.str->data : "";
    }
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
  if (!fetch_csp(url, sizeof url, "beacon to"))
    return js_bool(0);
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
