/* Streams API: ReadableStream, WritableStream, TransformStream, the queuing strategies, CompressionStream,
 * DecompressionStream, TextEncoderStream and TextDecoderStream, plus Blob.stream() and Response.body.
 *
 * The stream classes are JavaScript (k_streams_js), compiled the first time a script touches one of the globals.
 * Only the DEFLATE codec is native. */

#include "bindings.h"
#include "crc32.h"
#include "deflate.h"
#include "inflate.h"

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

/* Compile and run the prelude once, then replace the placeholder accessors with the real constructors */
static int streams_load(js_ctx *ctx)
{
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_value fn, codec, exports;
  unsigned i;
  if (g_load_state != LOAD_NONE)
    return g_load_state == LOAD_OK;
  g_load_state = LOAD_RUNNING;
  ar = malloc(sizeof *ar);
  if (!ar)
    goto fail;
  js_arena_init(ar);
  prog = js_parse(k_streams_js, ar, &pe);
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    goto fail;
  }
  gc_push_scope();
  if (js_run(ctx, prog, &fn) != 0) {
    report_exc(ctx);
    gc_pop_scope();
    goto fail;
  }
  if (fn.type == JV_OBJ)
    gc_keep(fn.u.obj);
  codec = js_obj_value(js_new_native(ctx, streams_codec, "codec", 3));
  gc_keep(codec.u.obj);
  exports = js_call(ctx, fn, js_undef(), &codec, 1);
  if (ctx->has_exc) {
    report_exc(ctx);
    gc_pop_scope();
    goto fail;
  }
  if (exports.type == JV_OBJ)
    gc_keep(exports.u.obj);
  for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++)
    js_def(ctx->global, k_globals[i], js_get(ctx, exports, k_globals[i]), P_WRITABLE | P_CONFIGURABLE);
  js_def(ctx->global, "@bytesStream", js_get(ctx, exports, "bytesStream"), 0);
  gc_pop_scope();
  g_load_state = LOAD_OK;
  return 1;
fail:
  g_load_state = LOAD_FAILED;
  return 0;
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
  js_value helper, u8, r;
  if (!streams_load(ctx))
    return js_throw_type(ctx, "Streams are unavailable");
  helper = js_get(ctx, js_obj_value(ctx->global), "@bytesStream");
  gc_push_scope();
  u8 = bytes.type == JV_STR ? bytes_to_u8(ctx, (const unsigned char *)bytes.u.str->data, (unsigned)bytes.u.str->len)
                            : bytes_to_u8(ctx, 0, 0);
  if (u8.type == JV_OBJ)
    gc_keep(u8.u.obj);
  r = js_call(ctx, helper, js_undef(), &u8, 1);
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
  js_value s = js_get(ctx, t, "@stream");
  (void)args;
  (void)argc;
  if (s.type == JV_OBJ || t.type != JV_OBJ)
    return s;
  s = bytes_stream(ctx, js_get(ctx, t, "@body"));
  if (s.type == JV_OBJ)
    js_def(t.u.obj, "@stream", s, 0);
  return s;
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
}
