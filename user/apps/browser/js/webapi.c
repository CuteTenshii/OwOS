/* Performance (High Resolution Time, User Timing) and Encoding API (TextEncoder/TextDecoder). */
#include <stdbool.h>

#include "jsint.h"

/* Strings are UTF-8 bytes: utf-8 encode/decode are copies, other decoders transcode. */

static unsigned g_perf_origin;    /* msec() captured at page setup: now() base */
static double g_perf_time_origin; /* wall-clock ms at setup: performance.timeOrigin */
static js_object *g_textencoder_proto;
static js_object *g_textdecoder_proto;

/* windows-1252 mapping for 0x80..0x9F (identity elsewhere). */
static const unsigned short k_cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};
enum { DEC_UTF8, DEC_UTF16LE, DEC_UTF16BE, DEC_LATIN1 };

/* Fresh Uint8Array holding data[0..len). */
static js_value make_uint8array(js_ctx *ctx, const unsigned char *data, int len)
{
  js_value u8ctor, lenarg, view;
  if (len < 0)
    len = 0;
  u8ctor = js_get(ctx, js_obj_value(ctx->global), "Uint8Array");
  lenarg = js_number(len);
  view = js_construct(ctx, u8ctor, &lenarg, 1);
  if (data && len && view.type == JV_OBJ && view.u.obj->kind == OBJ_TYPEDARRAY && view.u.obj->host) {
    js_typedarray *ta = (js_typedarray *)view.u.obj->host;
    if (ta->buffer && ta->buffer->host) {
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      int n = len;
      if (ta->offset + n > ab->byteLength)
        n = ab->byteLength - ta->offset;
      if (n > 0)
        memcpy(ab->data + ta->offset, data, (unsigned)n);
    }
  }
  return view;
}

/* Raw bytes of an ArrayBuffer/TypedArray/DataView (NULL/0 otherwise). */
const unsigned char *view_bytes(js_value v, int *len)
{
  *len = 0;
  if (v.type != JV_OBJ || !v.u.obj->host)
    return 0;
  if (v.u.obj->kind == OBJ_ARRAYBUFFER) {
    js_arraybuffer *ab = (js_arraybuffer *)v.u.obj->host;
    *len = ab->byteLength;
    return ab->data;
  }
  if (v.u.obj->kind == OBJ_TYPEDARRAY) {
    js_typedarray *ta = (js_typedarray *)v.u.obj->host;
    js_arraybuffer *ab;
    if (!ta->buffer || !ta->buffer->host)
      return 0;
    ab = (js_arraybuffer *)ta->buffer->host;
    *len = ta_length(ta) * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
    return ab->data + ta->offset;
  }
  return 0;
}

/* --- TextEncoder --- */
static js_value textenc_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_obj_value(js_new_object(ctx, g_textencoder_proto));
}
static js_value textenc_encoding(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)t;
  (void)a;
  (void)n;
  return js_mk_cstring(ctx, "utf-8");
}
static js_value textenc_encode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s, r;
  (void)t;
  s = js_to_string(ctx, argc > 0 ? A(0) : js_mk_cstring(ctx, ""));
  gc_push_scope();
  if (s.type == JV_STR)
    gc_keep(s.u.str);
  r = (s.type == JV_STR) ? make_uint8array(ctx, (const unsigned char *)s.u.str->data, s.u.str->len)
                         : make_uint8array(ctx, (const unsigned char *)"", 0);
  gc_pop_scope();
  return r;
}
static js_value textenc_encodeinto(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0)), dst = A(1), r;
  js_object *ro;
  unsigned char *out = 0;
  int cap = 0, n;
  (void)t;
  if (dst.type == JV_OBJ && dst.u.obj->kind == OBJ_TYPEDARRAY && dst.u.obj->host) {
    js_typedarray *ta = (js_typedarray *)dst.u.obj->host;
    if (ta->buffer && ta->buffer->host) {
      js_arraybuffer *ab = (js_arraybuffer *)ta->buffer->host;
      out = ab->data + ta->offset;
      cap = ta_length(ta) * ta_elem_size(ta->type < 0 ? TA_U8 : ta->type);
    }
  }
  n = (s.type == JV_STR) ? s.u.str->len : 0;
  if (n > cap)
    n = cap;
  if (out && n > 0)
    memcpy(out, s.u.str->data, (unsigned)n);
  ro = js_new_object(ctx, ctx->object_proto);
  gc_push_scope();
  gc_keep(ro);
  js_def(ro, "read", js_number(n), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  js_def(ro, "written", js_number(n), P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
  r = js_obj_value(ro);
  gc_pop_scope();
  return r;
}

/* --- TextDecoder --- */
int text_decoder_kind(const char *label)
{
  char b[40];
  int i = 0, j;
  while (*label == ' ' || *label == '\t' || *label == '\n' || *label == '\r' || *label == '\f')
    label++;
  for (; label[i] && i < (int)sizeof b - 1; i++)
    b[i] = lower(label[i]);
  while (i > 0 && (b[i - 1] == ' ' || b[i - 1] == '\t' || b[i - 1] == '\n' || b[i - 1] == '\r' || b[i - 1] == '\f'))
    i--;
  b[i] = '\0';
  if (!b[0] || streq(b, "utf-8") || streq(b, "utf8") || streq(b, "unicode-1-1-utf-8"))
    return DEC_UTF8;
  if (streq(b, "utf-16") || streq(b, "utf-16le") || streq(b, "ucs-2") || streq(b, "unicode") || streq(b, "csunicode"))
    return DEC_UTF16LE;
  if (streq(b, "utf-16be") || streq(b, "unicodefffe"))
    return DEC_UTF16BE;
  j = i;
  (void)j;
  return DEC_LATIN1; /* everything else */
}
const char *text_decoder_name(int kind)
{
  switch (kind) {
  case DEC_UTF16LE:
    return "utf-16le";
  case DEC_UTF16BE:
    return "utf-16be";
  case DEC_LATIN1:
    return "windows-1252";
  default:
    return "utf-8";
  }
}
static js_value textdec_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  int kind = DEC_UTF8;
  bool fatal = false, ignore = false;
  (void)thisv;
  if (argc > 0 && A(0).type != JV_UNDEF)
    kind = text_decoder_kind(arg_str(ctx, A(0)));
  if (argc > 1 && A(1).type == JV_OBJ) {
    fatal = js_to_boolean(js_get(ctx, A(1), "fatal"));
    ignore = js_to_boolean(js_get(ctx, A(1), "ignoreBOM"));
  }
  o = js_new_object(ctx, g_textdecoder_proto);
  gc_push_scope();
  gc_keep(o);
  js_def(o, "@kind", js_number(kind), 0);
  js_def(o, "encoding", js_mk_cstring(ctx, text_decoder_name(kind)), P_ENUMERABLE);
  js_def(o, "fatal", js_bool(fatal), P_ENUMERABLE);
  js_def(o, "ignoreBOM", js_bool(ignore), P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(o);
}
/* Decode the bytes of an ArrayBuffer or view as text of the given DEC_* kind */
js_value text_decode(js_ctx *ctx, js_value view, int kind, bool ignore, bool fatal)
{
  int len = 0, i;
  const unsigned char *in;
  sbuf sb = {0, 0, 0};
  js_value r;
  in = view_bytes(view, &len);
  if (!in)
    return js_mk_cstring(ctx, "");
  if (kind == DEC_UTF8) {
    i = 0;
    if (!ignore && len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
      i = 3;
    for (; i < len;) {
      unsigned cp;
      int k = utf8_decode((const char *)in + i, (const char *)in + len, &cp);
      if (k <= 0)
        k = 1;
      /* k==1 with U+FFFD means a malformed lead byte (a real U+FFFD is 3 bytes). */
      if (k == 1 && in[i] >= 0x80 && fatal) {
        if (sb.b)
          free(sb.b);
        return js_throw_type(ctx, "The encoded data was not valid.");
      }
      {
        char tmp[4];
        int m = utf8_encode(cp, tmp);
        sb_putn(&sb, tmp, m);
      }
      i += k;
    }
  } else if (kind == DEC_UTF16LE || kind == DEC_UTF16BE) {
    bool be = (kind == DEC_UTF16BE);
    i = 0;
    if (!ignore && len >= 2) {
      if (in[0] == 0xFF && in[1] == 0xFE) {
        be = false;
        i = 2;
      } else if (in[0] == 0xFE && in[1] == 0xFF) {
        be = true;
        i = 2;
      }
    }
    for (; i + 1 < len; i += 2) {
      unsigned u = be ? (unsigned)(in[i] << 8 | in[i + 1]) : (unsigned)(in[i] | in[i + 1] << 8);
      unsigned cp = u;
      char tmp[4];
      int m;
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) { /* high surrogate + low */
        unsigned lo = be ? (unsigned)(in[i + 2] << 8 | in[i + 3]) : (unsigned)(in[i + 2] | in[i + 3] << 8);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        } else
          cp = 0xFFFD;
      } else if (u >= 0xD800 && u <= 0xDFFF) {
        cp = 0xFFFD;
      }
      if (cp == 0xFFFD && fatal) {
        if (sb.b)
          free(sb.b);
        return js_throw_type(ctx, "The encoded data was not valid.");
      }
      m = utf8_encode(cp, tmp);
      sb_putn(&sb, tmp, m);
    }
  } else { /* DEC_LATIN1 (windows-1252) */
    for (i = 0; i < len; i++) {
      unsigned b = in[i];
      unsigned cp = (b >= 0x80 && b <= 0x9F) ? k_cp1252_hi[b - 0x80] : b;
      char tmp[4];
      int m = utf8_encode(cp, tmp);
      sb_putn(&sb, tmp, m);
    }
  }
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  if (sb.b)
    free(sb.b);
  return r;
}

static js_value textdec_decode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)argc;
  return text_decode(ctx, A(0), js_to_int_sat(ctx, js_get(ctx, t, "@kind")), js_to_boolean(js_get(ctx, t, "ignoreBOM")),
                     js_to_boolean(js_get(ctx, t, "fatal")));
}

/* --- performance --- */
double perf_now_ms(void) { return (double)(msec() - g_perf_origin); }
static js_value perf_now(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)a;
  (void)n;
  return js_number(perf_now_ms());
}
static js_value perf_noop(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)ctx;
  (void)t;
  (void)a;
  (void)n;
  return js_undef();
}
/* Number stays itself; a string names a mark (its startTime, 0 if unknown). */
static double perf_resolve(js_ctx *ctx, js_value entries, js_value v)
{
  if (v.type == JV_NUM)
    return v.u.num;
  if (v.type == JV_STR && entries.type == JV_OBJ && entries.u.obj->kind == OBJ_ARRAY) {
    unsigned i, n = js_array_len(entries.u.obj);
    double found = 0;
    for (i = 0; i < n; i++) {
      js_value e = js_get_index(ctx, entries, i);
      js_value nm = js_get(ctx, e, "name");
      js_value ty = js_get(ctx, e, "entryType");
      if (nm.type == JV_STR && v.u.str && streq(nm.u.str->data, v.u.str->data) && ty.type == JV_STR &&
          streq(ty.u.str->data, "mark"))
        found = js_to_number(ctx, js_get(ctx, e, "startTime")); /* last match wins */
    }
    return found;
  }
  return js_to_number(ctx, v);
}
static void po_notify(js_ctx *ctx, js_value entry);
static js_value perf_add_entry(js_ctx *ctx, js_value entries, const char *name, const char *type, double start,
                               double dur)
{
  js_object *o = js_new_object(ctx, ctx->object_proto);
  js_value e;
  gc_push_scope();
  gc_keep(o);
  js_def(o, "name", js_mk_cstring(ctx, name), P_ENUMERABLE);
  js_def(o, "entryType", js_mk_cstring(ctx, type), P_ENUMERABLE);
  js_def(o, "startTime", js_number(start), P_ENUMERABLE);
  js_def(o, "duration", js_number(dur), P_ENUMERABLE);
  e = js_obj_value(o);
  if (entries.type == JV_OBJ && entries.u.obj->kind == OBJ_ARRAY)
    js_array_push(ctx, entries.u.obj, e);
  po_notify(ctx, e);
  gc_pop_scope();
  return e;
}
static js_value perf_mark(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value entries = js_get(ctx, t, "@entries");
  char name[256];
  double st = (double)(msec() - g_perf_origin);
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  if (argc > 1 && A(1).type == JV_OBJ) {
    js_value s = js_get(ctx, A(1), "startTime");
    if (s.type == JV_NUM)
      st = s.u.num;
  }
  return perf_add_entry(ctx, entries, name, "mark", st, 0);
}
static js_value perf_measure(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value entries = js_get(ctx, t, "@entries");
  char name[256];
  double st = 0, en = (double)(msec() - g_perf_origin);
  strlcpy(name, arg_str(ctx, A(0)), sizeof name);
  if (argc > 1 && A(1).type == JV_OBJ && A(1).u.obj->kind != OBJ_ARRAY) {
    js_value o = A(1);
    js_value s = js_get(ctx, o, "start"), e = js_get(ctx, o, "end"), d = js_get(ctx, o, "duration");
    int hasS = s.type != JV_UNDEF, hasE = e.type != JV_UNDEF, hasD = d.type != JV_UNDEF;
    if (hasS)
      st = perf_resolve(ctx, entries, s);
    if (hasE)
      en = perf_resolve(ctx, entries, e);
    else if (hasD && hasS)
      en = st + js_to_number(ctx, d);
    else if (hasD && !hasS) {
      st = en - js_to_number(ctx, d);
    }
  } else {
    if (argc > 1 && A(1).type != JV_UNDEF)
      st = perf_resolve(ctx, entries, A(1));
    if (argc > 2 && A(2).type != JV_UNDEF)
      en = perf_resolve(ctx, entries, A(2));
  }
  return perf_add_entry(ctx, entries, name, "measure", st, en - st);
}
/* Shared getEntries* filter; NULL type/name means no filter. */
static js_value perf_filter(js_ctx *ctx, js_value t, const char *type, const char *name)
{
  js_value entries = js_get(ctx, t, "@entries");
  js_object *out = js_new_array(ctx, 0);
  unsigned i, n;
  gc_push_scope();
  gc_keep(out);
  if (entries.type == JV_OBJ && entries.u.obj->kind == OBJ_ARRAY) {
    n = js_array_len(entries.u.obj);
    for (i = 0; i < n; i++) {
      js_value e = js_get_index(ctx, entries, i);
      js_value ty = js_get(ctx, e, "entryType");
      js_value nm = js_get(ctx, e, "name");
      if (type && !(ty.type == JV_STR && streq(ty.u.str->data, type)))
        continue;
      if (name && !(nm.type == JV_STR && streq(nm.u.str->data, name)))
        continue;
      js_array_push(ctx, out, e);
    }
  }
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value perf_getentries(js_ctx *ctx, js_value t, js_value *a, int n)
{
  (void)a;
  (void)n;
  return perf_filter(ctx, t, 0, 0);
}
static js_value perf_getbytype(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return perf_filter(ctx, t, argc > 0 ? arg_str(ctx, A(0)) : "", 0);
}
static js_value perf_getbyname(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256], type[64];
  strlcpy(name, argc > 0 ? arg_str(ctx, A(0)) : "", sizeof name);
  if (argc > 1 && A(1).type != JV_UNDEF && A(1).type != JV_NULL) {
    strlcpy(type, arg_str(ctx, A(1)), sizeof type);
    return perf_filter(ctx, t, type, name);
  }
  return perf_filter(ctx, t, 0, name);
}
/* Drop entries of `type` (optionally named `name`) from @entries. */
static void perf_clear(js_ctx *ctx, js_value t, const char *type, const char *name)
{
  js_value entries = js_get(ctx, t, "@entries");
  js_object *keep;
  unsigned i, n;
  if (entries.type != JV_OBJ || entries.u.obj->kind != OBJ_ARRAY)
    return;
  keep = js_new_array(ctx, 0);
  gc_push_scope();
  gc_keep(keep);
  n = js_array_len(entries.u.obj);
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, entries, i);
    js_value ty = js_get(ctx, e, "entryType");
    js_value nm = js_get(ctx, e, "name");
    bool match = ty.type == JV_STR && streq(ty.u.str->data, type) &&
                 (!name || (nm.type == JV_STR && streq(nm.u.str->data, name)));
    if (!match)
      js_array_push(ctx, keep, e);
  }
  js_set(ctx, t, "@entries", js_obj_value(keep));
  gc_pop_scope();
}
static js_value perf_clearmarks(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256];
  if (argc > 0 && A(0).type != JV_UNDEF) {
    strlcpy(name, arg_str(ctx, A(0)), sizeof name);
    perf_clear(ctx, t, "mark", name);
  } else
    perf_clear(ctx, t, "mark", 0);
  return js_undef();
}
static js_value perf_clearmeasures(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char name[256];
  if (argc > 0 && A(0).type != JV_UNDEF) {
    strlcpy(name, arg_str(ctx, A(0)), sizeof name);
    perf_clear(ctx, t, "measure", name);
  } else
    perf_clear(ctx, t, "measure", 0);
  return js_undef();
}
static js_value perf_tojson(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_object *o = js_new_object(ctx, ctx->object_proto);
  js_value r;
  (void)t;
  (void)a;
  (void)n;
  gc_push_scope();
  gc_keep(o);
  js_def(o, "timeOrigin", js_number(g_perf_time_origin), P_ENUMERABLE);
  r = js_obj_value(o);
  gc_pop_scope();
  return r;
}

js_object *observer_class(js_ctx *ctx, js_object *win, const char *name, js_native ctor, js_object **proto)
{
  js_object *c;
  *proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)proto);
  gc_push_scope();
  c = (js_object *)gc_keep(js_new_native(ctx, ctor, name, 1));
  c->fn->data = js_obj_value(*proto);
  gc_write_barrier(c);
  js_def(c, "prototype", js_obj_value(*proto), 0);
  js_def(*proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return c;
}

/* --- PerformanceObserver ---
 * An observer holds @cb, @types (the entry types it watches) and @queue. Entries are queued as they are created
 * and delivered from a 0ms timer. Only the types performance.mark/measure produce are supported. */
static js_object *g_po_proto, *g_po_all, *g_pel_proto;
static int g_po_flush_pending;

static int po_supported(const char *type) { return streq(type, "mark") || streq(type, "measure"); }

static int po_wants(js_ctx *ctx, js_value observer, const char *type)
{
  js_value types = js_get(ctx, observer, "@types");
  unsigned i;
  if (!js_is_array(types))
    return 0;
  for (i = 0; i < js_array_len(types.u.obj); i++) {
    js_value t = js_get_index(ctx, types, i);
    if (t.type == JV_STR && streq(t.u.str->data, type))
      return 1;
  }
  return 0;
}

static js_value po_flush(js_ctx *ctx, js_value thisv, js_value *args, int argc);

static void po_schedule_flush(js_ctx *ctx)
{
  js_value set_timeout = js_get(ctx, js_obj_value(ctx->global), "setTimeout"), argv[2];
  if (g_po_flush_pending || !js_is_callable(set_timeout))
    return;
  g_po_flush_pending = 1;
  gc_push_scope();
  argv[0] = js_obj_value((js_object *)gc_keep(js_new_native(ctx, po_flush, "performanceobserver", 0)));
  argv[1] = js_number(0);
  js_call(ctx, set_timeout, js_obj_value(ctx->global), argv, 2);
  gc_pop_scope();
}

static void po_notify(js_ctx *ctx, js_value entry)
{
  js_value type = js_get(ctx, entry, "entryType");
  unsigned i;
  if (!g_po_all || type.type != JV_STR)
    return;
  for (i = 0; i < js_array_len(g_po_all); i++) {
    js_value ov = js_get_index(ctx, js_obj_value(g_po_all), i), queue = js_get(ctx, ov, "@queue");
    if (js_is_array(queue) && po_wants(ctx, ov, type.u.str->data)) {
      js_array_push(ctx, queue.u.obj, entry);
      po_schedule_flush(ctx);
    }
  }
}

static js_value po_flush(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *observers;
  unsigned i;
  (void)thisv;
  (void)args;
  (void)argc;
  g_po_flush_pending = 0;
  if (!g_po_all)
    return js_undef();
  gc_push_scope();
  observers = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(g_po_all); i++)
    js_array_push(ctx, observers, js_get_index(ctx, js_obj_value(g_po_all), i));
  for (i = 0; i < js_array_len(observers); i++) {
    js_value ov = js_get_index(ctx, js_obj_value(observers), i), queue = js_get(ctx, ov, "@queue");
    js_object *list, *opts;
    js_value argv[3];
    if (!js_is_array(queue) || js_array_len(queue.u.obj) == 0)
      continue;
    gc_push_scope();
    gc_keep(queue.u.obj);
    js_def(ov.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
    list = (js_object *)gc_keep(js_new_object(ctx, g_pel_proto));
    js_def(list, "@entries", queue, 0);
    opts = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(opts, "droppedEntriesCount", js_number(0), P_ENUMERABLE);
    argv[0] = js_obj_value(list);
    argv[1] = ov;
    argv[2] = js_obj_value(opts);
    js_call(ctx, js_get(ctx, ov, "@cb"), ov, argv, 3);
    if (ctx->has_exc)
      report_exc(ctx);
    gc_pop_scope();
  }
  gc_pop_scope();
  return js_undef();
}

static js_value po_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  if (!js_is_callable(A(0)))
    return js_throw_type(ctx, "PerformanceObserver: callback must be a function");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_po_proto));
  js_def(o, "@cb", A(0), 0);
  js_def(o, "@types", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(o, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
  return js_obj_value(o);
}

static void po_register(js_ctx *ctx, js_value t)
{
  unsigned i;
  for (i = 0; i < js_array_len(g_po_all); i++)
    if (js_get_index(ctx, js_obj_value(g_po_all), i).u.obj == t.u.obj)
      return;
  js_array_push(ctx, g_po_all, t);
}

static js_value po_observe(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value opts = A(0), et, ty, types = js_get(ctx, t, "@types");
  unsigned i;
  if (opts.type != JV_OBJ)
    return js_throw_type(ctx, "PerformanceObserver.observe: options must be an object");
  if (!js_is_array(types))
    return js_undef();
  et = js_get(ctx, opts, "entryTypes");
  ty = js_get(ctx, opts, "type");
  if (et.type == JV_UNDEF && ty.type == JV_UNDEF)
    return js_throw_type(ctx, "PerformanceObserver.observe: entryTypes or type is required");
  if (et.type != JV_UNDEF && ty.type != JV_UNDEF)
    return js_throw_type(ctx, "PerformanceObserver.observe: entryTypes and type are mutually exclusive");
  gc_push_scope();
  if (et.type != JV_UNDEF) {
    js_object *kept;
    if (!js_is_array(et)) {
      gc_pop_scope();
      return js_throw_type(ctx, "PerformanceObserver.observe: entryTypes must be a sequence");
    }
    kept = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < js_array_len(et.u.obj); i++) {
      js_value s = js_to_string(ctx, js_get_index(ctx, et, i));
      if (s.type == JV_STR && po_supported(s.u.str->data))
        js_array_push(ctx, kept, s);
    }
    js_def(t.u.obj, "@types", js_obj_value(kept), 0);
    if (js_array_len(kept) > 0)
      po_register(ctx, t);
  } else {
    js_value s = js_to_string(ctx, ty);
    if (s.type == JV_STR && po_supported(s.u.str->data)) {
      if (!po_wants(ctx, t, s.u.str->data))
        js_array_push(ctx, types.u.obj, s);
      po_register(ctx, t);
      if (js_to_boolean(js_get(ctx, opts, "buffered"))) {
        js_value perf = js_get(ctx, js_obj_value(ctx->global), "performance");
        js_value past = js_get(ctx, perf, "@entries"), queue = js_get(ctx, t, "@queue");
        if (js_is_array(past) && js_is_array(queue))
          for (i = 0; i < js_array_len(past.u.obj); i++) {
            js_value e = js_get_index(ctx, past, i), et2 = js_get(ctx, e, "entryType");
            if (et2.type == JV_STR && streq(et2.u.str->data, s.u.str->data))
              js_array_push(ctx, queue.u.obj, e);
          }
        if (js_is_array(queue) && js_array_len(queue.u.obj) > 0)
          po_schedule_flush(ctx);
      }
    }
  }
  gc_pop_scope();
  return js_undef();
}

static js_value po_disconnect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *kept;
  unsigned i;
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ || !g_po_all)
    return js_undef();
  gc_push_scope();
  js_def(t.u.obj, "@types", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(t.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  kept = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(g_po_all); i++) {
    js_value o = js_get_index(ctx, js_obj_value(g_po_all), i);
    if (o.u.obj != t.u.obj)
      js_array_push(ctx, kept, o);
  }
  g_po_all = kept;
  gc_pop_scope();
  return js_undef();
}

static js_value po_take_records(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value queue = js_get(ctx, t, "@queue");
  (void)args;
  (void)argc;
  if (!js_is_array(queue) || t.type != JV_OBJ)
    return js_obj_value(js_new_array(ctx, 0));
  js_def(t.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  return queue;
}

static void po_setup(js_ctx *ctx, js_object *win)
{
  js_object *ctor, *supported;
  gc_push_scope();
  ctor = observer_class(ctx, win, "PerformanceObserver", po_ctor, &g_po_proto);
  def_method(g_po_proto, "observe", po_observe, 1);
  def_method(g_po_proto, "disconnect", po_disconnect, 0);
  def_method(g_po_proto, "takeRecords", po_take_records, 0);
  supported = (js_object *)gc_keep(js_new_array(ctx, 0));
  js_array_push(ctx, supported, js_mk_cstring(ctx, "mark"));
  js_array_push(ctx, supported, js_mk_cstring(ctx, "measure"));
  js_def(ctor, "supportedEntryTypes", js_obj_value(supported), P_ENUMERABLE);
  g_pel_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_pel_proto);
  def_method(g_pel_proto, "getEntries", perf_getentries, 0);
  def_method(g_pel_proto, "getEntriesByType", perf_getbytype, 1);
  def_method(g_pel_proto, "getEntriesByName", perf_getbyname, 2);
  g_po_all = js_new_array(ctx, 0);
  gc_add_root((void **)&g_po_all);
  g_po_flush_pending = 0;
  gc_pop_scope();
}

void js_webapi_setup(js_ctx *ctx, js_object *win)
{
  g_textencoder_proto = js_new_object(ctx, ctx->object_proto);
  g_textdecoder_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_textencoder_proto);
  gc_add_root((void **)&g_textdecoder_proto);
  /* performance: now/timeOrigin, User Timing, legacy timing/navigation objects. */
  {
    js_object *perf = js_new_object(ctx, ctx->object_proto);
    js_object *entries;
    gc_push_scope();
    gc_keep(perf);
    g_perf_origin = msec();
    g_perf_time_origin = (double)now_ms();
    entries = js_new_array(ctx, 0);
    js_def(perf, "@entries", js_obj_value(entries), 0);
    js_def(perf, "timeOrigin", js_number(g_perf_time_origin), P_ENUMERABLE);
    def_method(perf, "now", perf_now, 0);
    def_method(perf, "mark", perf_mark, 1);
    def_method(perf, "measure", perf_measure, 1);
    def_method(perf, "getEntries", perf_getentries, 0);
    def_method(perf, "getEntriesByType", perf_getbytype, 1);
    def_method(perf, "getEntriesByName", perf_getbyname, 2);
    def_method(perf, "clearMarks", perf_clearmarks, 1);
    def_method(perf, "clearMeasures", perf_clearmeasures, 1);
    def_method(perf, "clearResourceTimings", perf_noop, 0);
    def_method(perf, "setResourceTimingBufferSize", perf_noop, 1);
    def_method(perf, "toJSON", perf_tojson, 0);
    def_method(perf, "addEventListener", el_noop, 2);
    def_method(perf, "removeEventListener", el_noop, 2);
    { /* performance.timing (deprecated): every phase anchored at timeOrigin */
      js_object *tm = js_new_object(ctx, ctx->object_proto);
      static const char *k_timing[] = {
          "navigationStart",
          "unloadEventStart",
          "unloadEventEnd",
          "redirectStart",
          "redirectEnd",
          "fetchStart",
          "domainLookupStart",
          "domainLookupEnd",
          "connectStart",
          "connectEnd",
          "secureConnectionStart",
          "requestStart",
          "responseStart",
          "responseEnd",
          "domLoading",
          "domInteractive",
          "domContentLoadedEventStart",
          "domContentLoadedEventEnd",
          "domComplete",
          "loadEventStart",
          "loadEventEnd",
      };
      unsigned ti;
      gc_keep(tm);
      for (ti = 0; ti < sizeof k_timing / sizeof k_timing[0]; ti++)
        js_def(tm, k_timing[ti], js_number(g_perf_time_origin), P_ENUMERABLE);
      js_def(perf, "timing", js_obj_value(tm), P_ENUMERABLE);
    }
    { /* performance.navigation (deprecated) */
      js_object *nv = js_new_object(ctx, ctx->object_proto);
      gc_keep(nv);
      js_def(nv, "type", js_number(0), P_ENUMERABLE); /* TYPE_NAVIGATE */
      js_def(nv, "redirectCount", js_number(0), P_ENUMERABLE);
      js_def(perf, "navigation", js_obj_value(nv), P_ENUMERABLE);
    }
    { /* performance.memory (non-standard; several sites feature-probe it) */
      js_object *mem = js_new_object(ctx, ctx->object_proto);
      gc_keep(mem);
      js_def(mem, "jsHeapSizeLimit", js_number(256 * 1024 * 1024), P_ENUMERABLE);
      js_def(mem, "totalJSHeapSize", js_number(16 * 1024 * 1024), P_ENUMERABLE);
      js_def(mem, "usedJSHeapSize", js_number(8 * 1024 * 1024), P_ENUMERABLE);
      js_def(perf, "memory", js_obj_value(mem), P_ENUMERABLE);
    }
    js_def(win, "performance", js_obj_value(perf), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  po_setup(ctx, win);
  /* TextEncoder / TextDecoder (WHATWG Encoding API) */
  def_method(g_textencoder_proto, "encode", textenc_encode, 1);
  def_method(g_textencoder_proto, "encodeInto", textenc_encodeinto, 2);
  def_acc(g_textencoder_proto, "encoding", textenc_encoding, 0);
  def_method(g_textdecoder_proto, "decode", textdec_decode, 1);
  {
    js_object *ec = js_new_native(ctx, textenc_ctor, "TextEncoder", 0);
    js_object *dc = js_new_native(ctx, textdec_ctor, "TextDecoder", 0);
    gc_push_scope();
    gc_keep(ec);
    gc_keep(dc);
    js_def(ec, "prototype", js_obj_value(g_textencoder_proto), 0);
    js_def(g_textencoder_proto, "constructor", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
    js_def(dc, "prototype", js_obj_value(g_textdecoder_proto), 0);
    js_def(g_textdecoder_proto, "constructor", js_obj_value(dc), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "TextEncoder", js_obj_value(ec), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "TextDecoder", js_obj_value(dc), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
}

/* Unroot Encoding-API prototypes at teardown (gc_add_root does not dedup). */
void js_webapi_teardown(void)
{
  gc_remove_root((void **)&g_textencoder_proto);
  gc_remove_root((void **)&g_textdecoder_proto);
  g_textencoder_proto = g_textdecoder_proto = 0;
  gc_remove_root((void **)&g_po_proto);
  gc_remove_root((void **)&g_pel_proto);
  gc_remove_root((void **)&g_po_all);
  g_po_proto = g_pel_proto = g_po_all = 0;
}
