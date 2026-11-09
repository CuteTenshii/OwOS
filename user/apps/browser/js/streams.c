/* Streams API: ReadableStream, WritableStream, TransformStream, the queuing strategies, CompressionStream,
 * DecompressionStream, TextEncoderStream and TextDecoderStream, plus Blob.stream() and Response.body.
 *
 * The stream state machines are native (streamscore.c). What is left is JavaScript (k_streams_js), compiled with
 * them the first time a script touches one of the globals: the strategies, tee, async iteration, the text and
 * compression streams and the body helpers. The DEFLATE codec is native too. */

#include "bindings.h"
#include "crc32.h"
#include "deflate.h"
#include "inflate.h"
#include "streamscore.h"

#define JS(...) #__VA_ARGS__
#define STREAMS_MAX_BYTES (64u << 20)

static const char k_streams_js[] =
#include "streams_prelude.inc"
    ;

static const char *const k_globals[] = {"ReadableStream",
                                        "ReadableStreamDefaultReader",
                                        "ReadableStreamDefaultController",
                                        "WritableStream",
                                        "WritableStreamDefaultWriter",
                                        "WritableStreamDefaultController",
                                        "TransformStream",
                                        "TransformStreamDefaultController",
                                        "CountQueuingStrategy",
                                        "ByteLengthQueuingStrategy",
                                        "CompressionStream",
                                        "DecompressionStream",
                                        "TextEncoderStream",
                                        "TextDecoderStream"};

static const char *const k_helpers[] = {"bytesStream", "consume"};

static js_value helper(js_ctx *ctx, const char *name)
{
  char key[24] = "@";
  strlcpy(key + 1, name, sizeof key - 1);
  return js_get(ctx, js_obj_value(ctx->global), key);
}

enum { LOAD_NONE, LOAD_RUNNING, LOAD_OK, LOAD_FAILED };
static int g_load_state;

/* ---- DEFLATE codec ------------------------------------------------------ */

struct inbuf {
  const unsigned char *p;
  unsigned n, pos;
};
struct outbuf {
  unsigned char *p;
  unsigned n, cap;
  int over;
};

static int in_read(void *ctx)
{
  struct inbuf *in = ctx;
  return in->pos < in->n ? in->p[in->pos++] : -1;
}

static int out_write(void *ctx, const unsigned char *buf, unsigned n)
{
  struct outbuf *o = ctx;
  if (n > STREAMS_MAX_BYTES - o->n) {
    o->over = 1;
    return -1;
  }
  if (o->n + n > o->cap) {
    unsigned cap = o->cap ? o->cap : 4096;
    unsigned char *p;
    while (cap < o->n + n)
      cap *= 2;
    p = realloc(o->p, cap);
    if (!p) {
      o->over = 1;
      return -1;
    }
    o->p = p;
    o->cap = cap;
  }
  memcpy(o->p + o->n, buf, n);
  o->n += n;
  return 0;
}

static js_value bytes_to_u8(js_ctx *ctx, const unsigned char *data, unsigned n)
{
  js_value v = js_new_typedarray(ctx, TA_U8, (int)n);
  if (n && v.type == JV_OBJ && v.u.obj->kind == OBJ_TYPEDARRAY && v.u.obj->host) {
    js_typedarray *ta = v.u.obj->host;
    js_arraybuffer *ab = ta->buffer->host;
    if (ab->byteLength >= ta->offset + (int)n)
      memcpy(ab->data + ta->offset, data, n);
  }
  return v;
}

/* Offset of the DEFLATE body behind a gzip header, or -1 */
static int gzip_header_len(const unsigned char *s, unsigned n)
{
  unsigned pos = 10, flg;
  if (n < 18 || s[0] != 0x1F || s[1] != 0x8B || s[2] != 8)
    return -1;
  flg = s[3];
  if (flg & 0xE0)
    return -1;
  if (flg & 4) {
    pos += 2 + (s[pos] | (unsigned)s[pos + 1] << 8);
    if (pos > n)
      return -1;
  }
  if (flg & 8) {
    while (pos < n && s[pos])
      pos++;
    pos++;
  }
  if (flg & 16) {
    while (pos < n && s[pos])
      pos++;
    pos++;
  }
  if (flg & 2)
    pos += 2;
  return pos <= n ? (int)pos : -1;
}

static unsigned load_le32(const unsigned char *p)
{
  return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static int trailer_ok(int fmt, const unsigned char *t, const unsigned char *out, unsigned n)
{
  if (fmt == DEFLATE_GZIP)
    return crc32(0, out, n) == load_le32(t) && load_le32(t + 4) == n;
  if (fmt == DEFLATE_ZLIB)
    return zlib_adler32(out, n) == ((unsigned)t[0] << 24 | (unsigned)t[1] << 16 | (unsigned)t[2] << 8 | t[3]);
  return 1;
}

static js_value decompress_to_u8(js_ctx *ctx, int fmt, const unsigned char *s, unsigned n)
{
  struct inbuf in = {s, n, 0};
  struct outbuf out = {0, 0, 0, 0};
  unsigned tail = 0;
  int head = 0, rc;
  js_value v;
  if (fmt == DEFLATE_GZIP) {
    head = gzip_header_len(s, n);
    tail = 8;
  } else if (fmt == DEFLATE_ZLIB) {
    head =
        n >= 2 && (s[0] & 0x0F) == 8 && (s[0] >> 4) <= 7 && !(s[1] & 0x20) && ((s[0] << 8) | s[1]) % 31 == 0 ? 2 : -1;
    tail = 4;
  }
  if (head < 0 || n < (unsigned)head + tail)
    return js_throw_type(ctx, "Invalid compressed data header");
  in.pos = (unsigned)head;
  rc = inflate_stream(in_read, &in, out_write, &out);
  if (rc != 0 || out.over || n - in.pos != tail || !trailer_ok(fmt, s + in.pos, out.p, out.n)) {
    int over = out.over;
    free(out.p);
    return js_throw_type(ctx, over ? "Decompressed data is too large" : "Invalid or truncated compressed data");
  }
  v = bytes_to_u8(ctx, out.p, out.n);
  free(out.p);
  return v;
}

static js_value compress_to_u8(js_ctx *ctx, int fmt, const unsigned char *s, unsigned n)
{
  unsigned cap = gzip_bound(n);
  unsigned char *out = malloc(cap);
  int len;
  js_value v;
  if (!out)
    return js_throw_range(ctx, "Out of memory");
  len = deflate_compress(fmt, s, n, out, cap);
  if (len < 0) {
    free(out);
    return js_throw_type(ctx, "Compression failed");
  }
  v = bytes_to_u8(ctx, out, (unsigned)len);
  free(out);
  return v;
}

/* codec(decompress, format, bytes) -> Uint8Array */
static js_value streams_codec(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  static const unsigned char none;
  const char *name = arg_str(ctx, A(1));
  int n, fmt;
  const unsigned char *src = view_bytes(A(2), &n);
  (void)t;
  if (!strcmp(name, "gzip"))
    fmt = DEFLATE_GZIP;
  else if (!strcmp(name, "deflate"))
    fmt = DEFLATE_ZLIB;
  else if (!strcmp(name, "deflate-raw"))
    fmt = DEFLATE_RAW;
  else
    return js_throw_type(ctx, "Unsupported compression format");
  if (!src) {
    src = &none;
    n = 0;
  }
  if ((unsigned)n > STREAMS_MAX_BYTES)
    return js_throw_range(ctx, "Input is too large");
  return js_to_number(ctx, A(0)) != 0 ? decompress_to_u8(ctx, fmt, src, (unsigned)n)
                                      : compress_to_u8(ctx, fmt, src, (unsigned)n);
}

/* ---- lazy installation -------------------------------------------------- */

/* Run the prelude in ctx: its exports (the stream classes and the helpers), or undefined with the exception left
 * pending. The parsed program goes to *arena, which the caller keeps for as long as the classes live. */
js_value js_streams_run(js_ctx *ctx, js_arena **arena)
{
  js_node *prog;
  js_parse_error pe;
  js_value fn, args[2], exports = js_undef();
  *arena = malloc(sizeof **arena);
  if (!*arena)
    return js_undef();
  js_arena_init(*arena);
  prog = js_parse(k_streams_js, *arena, &pe);
  if (!prog) {
    js_arena_free(*arena);
    free(*arena);
    *arena = 0;
    return js_undef();
  }
  gc_push_scope();
  if (js_run(ctx, prog, &fn) == 0) {
    if (fn.type == JV_OBJ)
      gc_keep(fn.u.obj);
    args[0] = js_obj_value(js_new_native(ctx, streams_codec, "codec", 3));
    gc_keep(args[0].u.obj);
    args[1] = js_obj_value(streams_core_install(ctx));
    gc_keep(args[1].u.obj);
    exports = js_call(ctx, fn, js_undef(), args, 2);
  }
  gc_pop_scope();
  return ctx->has_exc ? js_undef() : exports;
}

/* Compile and run the prelude once, then replace the placeholder accessors with the real constructors */
static int streams_load(js_ctx *ctx)
{
  js_arena *ar;
  js_value exports;
  unsigned i;
  if (g_load_state != LOAD_NONE)
    return g_load_state == LOAD_OK;
  g_load_state = LOAD_RUNNING;
  exports = js_streams_run(ctx, &ar);
  if (exports.type != JV_OBJ || !arenas_push(ar)) {
    if (ctx->has_exc)
      report_exc(ctx);
    g_load_state = LOAD_FAILED;
    return 0;
  }
  gc_push_scope();
  gc_keep(exports.u.obj);
  for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++)
    js_def(ctx->global, k_globals[i], js_get(ctx, exports, k_globals[i]), P_WRITABLE | P_CONFIGURABLE);
  for (i = 0; i < sizeof k_helpers / sizeof k_helpers[0]; i++) {
    char key[24] = "@";
    strlcpy(key + 1, k_helpers[i], sizeof key - 1);
    js_def(ctx->global, key, js_get(ctx, exports, k_helpers[i]), 0);
  }
  gc_pop_scope();
  g_load_state = LOAD_OK;
  return 1;
}

static js_value lazy_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = ctx->native_data;
  (void)t;
  (void)args;
  (void)argc;
  if (name.type != JV_STR || !streams_load(ctx))
    return js_undef();
  return js_get(ctx, js_obj_value(ctx->global), name.u.str->data);
}

static js_value lazy_set(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = ctx->native_data;
  (void)t;
  if (name.type == JV_STR)
    js_def(ctx->global, name.u.str->data, A(0), P_WRITABLE | P_CONFIGURABLE);
  return js_undef();
}

/* ---- Blob.stream() and Response.body ------------------------------------ */

/* A ReadableStream over the raw bytes held in a JS string */
static js_value bytes_stream(js_ctx *ctx, js_value bytes)
{
  js_value fn, u8, r;
  if (!streams_load(ctx))
    return js_throw_type(ctx, "Streams are unavailable");
  fn = helper(ctx, "bytesStream");
  gc_push_scope();
  u8 = bytes.type == JV_STR ? bytes_to_u8(ctx, (const unsigned char *)bytes.u.str->data, (unsigned)bytes.u.str->len)
                            : bytes_to_u8(ctx, 0, 0);
  if (u8.type == JV_OBJ)
    gc_keep(u8.u.obj);
  r = js_call(ctx, fn, js_undef(), &u8, 1);
  gc_pop_scope();
  return r;
}

static js_value blob_stream(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return bytes_stream(ctx, js_get(ctx, t, "@bytes"));
}

static js_value response_body(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_get(ctx, t, "@source");
  (void)args;
  (void)argc;
  if (s.type == JV_OBJ || t.type != JV_OBJ)
    return s;
  s = js_get(ctx, t, "@stream");
  if (s.type == JV_OBJ)
    return s;
  s = bytes_stream(ctx, js_get(ctx, t, "@body"));
  if (s.type == JV_OBJ)
    js_def(t.u.obj, "@stream", s, 0);
  return s;
}

js_value response_init_body(js_ctx *ctx, js_value body, js_value *stream, char *ctype, int ctcap)
{
  if (body.type == JV_UNDEF || body.type == JV_NULL)
    return js_mk_cstring(ctx, "");
  if (body.type == JV_OBJ) {
    js_object *o = body.u.obj;
    if (is_blob(o)) {
      js_value type = js_get(ctx, body, "type");
      strlcpy(ctype, type.type == JV_STR ? type.u.str->data : "", (unsigned)ctcap);
      return js_get(ctx, body, "@bytes");
    }
    if ((o->kind == OBJ_ARRAYBUFFER || o->kind == OBJ_TYPEDARRAY) && o->host) {
      int n;
      const unsigned char *p = view_bytes(body, &n);
      ctype[0] = '\0';
      return js_mk_string(ctx, p ? (const char *)p : "", p ? n : 0);
    }
    if (js_is_callable(js_get(ctx, body, "getReader")) && js_is_callable(js_get(ctx, body, "pipeTo"))) {
      ctype[0] = '\0';
      *stream = body;
      return js_mk_cstring(ctx, "");
    }
  }
  return body.type == JV_STR ? body : js_to_string(ctx, body);
}

js_value response_consume(js_ctx *ctx, js_value resp, int kind)
{
  js_value src = js_get(ctx, resp, "@source"), args[3], hdrs, type, r;
  if (src.type != JV_OBJ || !streams_load(ctx))
    return js_promise_rejected(ctx, js_mk_cstring(ctx, "Response has no stream body"));
  hdrs = js_get(ctx, resp, "headers");
  type = hdrs.type == JV_OBJ ? js_get(ctx, hdrs, "@ctype") : js_undef();
  gc_push_scope();
  args[0] = src;
  args[1] = js_number(kind);
  args[2] = type.type == JV_STR ? type : js_mk_cstring(ctx, "");
  if (args[2].type == JV_STR)
    gc_keep(args[2].u.str);
  js_def(resp.u.obj, "bodyUsed", js_bool(1), P_WRITABLE | P_ENUMERABLE);
  r = js_call(ctx, helper(ctx, "consume"), js_undef(), args, 3);
  gc_pop_scope();
  return r;
}

static js_value response_arraybuffer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body, u8;
  (void)args;
  (void)argc;
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_BUFFER);
  body = js_get(ctx, t, "@body");
  gc_push_scope();
  u8 = body.type == JV_STR ? bytes_to_u8(ctx, (const unsigned char *)body.u.str->data, (unsigned)body.u.str->len)
                           : bytes_to_u8(ctx, 0, 0);
  if (u8.type == JV_OBJ)
    gc_keep(u8.u.obj);
  u8 = js_get(ctx, u8, "buffer");
  gc_pop_scope();
  return js_promise_resolved(ctx, u8);
}

static js_value response_blob(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value body, hdrs, type;
  (void)args;
  (void)argc;
  if (js_get(ctx, t, "@source").type == JV_OBJ)
    return response_consume(ctx, t, CONSUME_BLOB);
  body = js_get(ctx, t, "@body");
  hdrs = js_get(ctx, t, "headers");
  type = hdrs.type == JV_OBJ ? js_get(ctx, hdrs, "@ctype") : js_undef();
  return js_promise_resolved(ctx, make_blob(ctx, body.type == JV_STR ? body.u.str->data : "",
                                            body.type == JV_STR ? body.u.str->len : 0,
                                            type.type == JV_STR ? type.u.str->data : ""));
}

void js_streams_setup(js_ctx *ctx, js_object *win)
{
  unsigned i;
  g_load_state = LOAD_NONE;
  for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++) {
    js_object *get, *set;
    gc_push_scope();
    get = gc_keep(js_new_native(ctx, lazy_get, k_globals[i], 0));
    set = gc_keep(js_new_native(ctx, lazy_set, k_globals[i], 1));
    get->fn->data = js_mk_cstring(ctx, k_globals[i]);
    gc_write_barrier(get);
    set->fn->data = js_mk_cstring(ctx, k_globals[i]);
    gc_write_barrier(set);
    js_def_accessor(win, k_globals[i], get, set, P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_method(g_blob_proto, "stream", blob_stream, 0);
  def_acc(g_response_proto, "body", response_body, 0);
  def_method(g_response_proto, "arrayBuffer", response_arraybuffer, 0);
  def_method(g_response_proto, "blob", response_blob, 0);
}
