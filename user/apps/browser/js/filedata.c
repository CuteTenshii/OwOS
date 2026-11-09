/* FileReader, ProgressEvent and the DataTransfer family (DataTransfer, DataTransferItem, DataTransferItemList).
 * The classes are JavaScript (filedata_prelude.inc), compiled the first time a script touches one of the globals;
 * the byte conversions and the FileList they hand out are native. */

#include "base64.h"
#include "bindings.h"

#define JS(...) #__VA_ARGS__

static const char k_filedata_js[] =
#include "filedata_prelude.inc"
    ;

static const char *const k_globals[] = {"FileReader",       "ProgressEvent",         "DataTransfer",
                                        "DataTransferItem", "DataTransferItemList"};

enum { LOAD_NONE, LOAD_RUNNING, LOAD_OK, LOAD_FAILED };
static int g_load_state;

enum { READ_TEXT, READ_BUFFER, READ_BINARY, READ_DATAURL };

/* N.read(blob, mode): the Blob's bytes as a string, ArrayBuffer, binary string or data: URL */
static js_value fd_read(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value b = A(0).type == JV_OBJ ? js_get(ctx, A(0), "@bytes") : js_undef(), type, r;
  int mode = js_to_int_sat(ctx, A(1)), len;
  const unsigned char *p;
  sbuf sb = {0, 0, 0};
  (void)t;
  if (b.type != JV_STR)
    return js_undef();
  p = (const unsigned char *)b.u.str->data;
  len = b.u.str->len;
  if (mode == READ_BUFFER) {
    js_value out;
    return js_promise_inspect(ctx, blob_arraybuffer(ctx, A(0), 0, 0), &out) == 1 ? out : js_undef();
  }
  if (mode == READ_TEXT)
    return b;
  if (mode == READ_BINARY) {
    for (int i = 0; i < len; i++) {
      if (p[i] < 0x80) {
        sb_putc(&sb, (char)p[i]);
      } else {
        sb_putc(&sb, (char)(0xC0 | (p[i] >> 6)));
        sb_putc(&sb, (char)(0x80 | (p[i] & 0x3F)));
      }
    }
  } else {
    char *enc = malloc((unsigned)base64_encoded_len(len) + 1);
    if (!enc)
      return js_throw_range(ctx, "out of memory");
    type = js_get(ctx, A(0), "type");
    sb_puts(&sb, "data:");
    sb_puts(&sb, type.type == JV_STR && type.u.str->len ? type.u.str->data : "application/octet-stream");
    sb_puts(&sb, ";base64,");
    enc[base64_encode(p, len, enc)] = '\0';
    sb_puts(&sb, enc);
    free(enc);
  }
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}

/* N.fileList(array): a FileList holding the same files */
static js_value fd_filelist(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value list;
  (void)t;
  if (A(0).type != JV_OBJ)
    return js_null();
  gc_push_scope();
  list = js_obj_value((js_object *)gc_keep(js_new_array(ctx, 0)));
  list.u.obj->proto = g_filelist_proto;
  for (unsigned i = 0, n = js_array_len(A(0).u.obj); i < n; i++)
    js_array_push(ctx, list.u.obj, js_get_index(ctx, A(0), i));
  gc_pop_scope();
  return list;
}

static int filedata_load(js_ctx *ctx)
{
  js_arena *ar;
  js_node *prog;
  js_parse_error pe;
  js_value fn, native, exports = js_undef();
  unsigned i;
  if (g_load_state != LOAD_NONE)
    return g_load_state == LOAD_OK;
  g_load_state = LOAD_RUNNING;
  ar = malloc(sizeof *ar);
  if (!ar)
    goto fail;
  js_arena_init(ar);
  prog = js_parse(k_filedata_js, ar, &pe);
  if (!prog || !arenas_push(ar)) {
    js_arena_free(ar);
    free(ar);
    goto fail;
  }
  gc_push_scope();
  if (js_run(ctx, prog, &fn) == 0) {
    js_object *n = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    if (fn.type == JV_OBJ)
      gc_keep(fn.u.obj);
    native = js_obj_value(n);
    js_def_native(ctx, n, "read", fd_read, 2);
    js_def_native(ctx, n, "fileList", fd_filelist, 1);
    exports = js_call(ctx, fn, js_undef(), &native, 1);
  }
  if (exports.type == JV_OBJ && !ctx->has_exc) {
    gc_keep(exports.u.obj);
    for (i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++)
      js_def(ctx->global, k_globals[i], js_get(ctx, exports, k_globals[i]), P_WRITABLE | P_CONFIGURABLE);
  }
  gc_pop_scope();
  if (exports.type != JV_OBJ || ctx->has_exc)
    goto fail;
  g_load_state = LOAD_OK;
  return 1;
fail:
  if (ctx->has_exc)
    report_exc(ctx);
  g_load_state = LOAD_FAILED;
  return 0;
}

static js_value lazy_get(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value name = ctx->native_data;
  (void)t;
  (void)args;
  (void)argc;
  if (name.type != JV_STR || !filedata_load(ctx))
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

void js_filedata_setup(js_ctx *ctx, js_object *win)
{
  g_load_state = LOAD_NONE;
  for (unsigned i = 0; i < sizeof k_globals / sizeof k_globals[0]; i++) {
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
}
