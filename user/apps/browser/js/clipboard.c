/* Async Clipboard API (navigator.clipboard, ClipboardItem). */

#include "clipboard.h"
#include "bindings.h"
#include "mime.h"

/* ------------------------------------------------- Async Clipboard API */

/* Resolve a ClipboardItem entry (a Blob, a string, or a settled Promise of either) to its raw bytes */
static js_string *clip_entry_bytes(js_ctx *ctx, js_value v, char *type, int tcap)
{
  js_value settled;
  if (v.type == JV_OBJ && js_promise_inspect(ctx, v, &settled) == 1)
    v = settled;
  if (v.type == JV_OBJ && v.u.obj->proto == g_blob_proto) {
    js_value b = js_get(ctx, v, "@bytes"), tp = js_get(ctx, v, "type");
    if (type && tp.type == JV_STR && tp.u.str->len)
      strlcpy(type, tp.u.str->data, tcap);
    return b.type == JV_STR ? b.u.str : 0;
  }
  {
    js_value s = js_to_string(ctx, v);
    return s.type == JV_STR ? s.u.str : 0;
  }
}

/* new ClipboardItem({ "text/plain": blobOrString, ... }), keeps the dictionary verbatim */
js_value clipitem_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o, *types, *dict;
  js_value items = A(0), ov;
  int i;
  (void)thisv;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_clipitem_proto));
  types = (js_object *)gc_keep(js_new_array(ctx, 0));
  dict = items.type == JV_OBJ ? items.u.obj : js_new_object(ctx, ctx->object_proto);
  gc_keep(dict);
  js_def(o, "@data", js_obj_value(dict), 0);
  for (i = 0; i < dict->props.n; i++) {
    pentry *pe = &dict->props.e[i];
    if (!(pe->flags & P_ENUMERABLE) || (unsigned char)pe->key[0] < ' ')
      continue; /* skip symbol keys */
    js_array_push(ctx, types, js_mk_cstring(ctx, pe->key));
  }
  js_def(o, "types", js_obj_value(types), P_ENUMERABLE);
  js_def(o, "presentationStyle", js_mk_cstring(ctx, "unspecified"), P_ENUMERABLE);
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* item.getType(mime) -> Promise<Blob> of that representation. */
js_value clipitem_gettype(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value data = js_get(ctx, t, "@data"), e, settled;
  const char *m = arg_str(ctx, A(0));
  if (data.type != JV_OBJ)
    return js_promise_rejected(ctx, dom_exception(ctx, "NotFoundError", "The type was not found"));
  e = js_get(ctx, data, m);
  if (e.type == JV_OBJ && js_promise_inspect(ctx, e, &settled) == 1)
    e = settled;
  if (e.type == JV_UNDEF || e.type == JV_NULL)
    return js_promise_rejected(ctx, dom_exception(ctx, "NotFoundError", "The type was not found"));
  if (e.type == JV_OBJ && e.u.obj->proto == g_blob_proto)
    return js_promise_resolved(ctx, e);
  {
    js_value s = js_to_string(ctx, e);
    return js_promise_resolved(
        ctx, make_blob(ctx, s.type == JV_STR ? s.u.str->data : "", s.type == JV_STR ? s.u.str->len : 0, m));
  }
}

/* ClipboardItem.supports(type), the formats the clipboard server round-trips */
js_value clipitem_supports(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *m = arg_str(ctx, A(0));
  (void)t;
  return js_bool(streq(m, "text/plain") || streq(m, "text/html") || streq(m, "image/png"));
}

/* navigator.clipboard.writeText(text) -> Promise<undefined>. */
js_value clip_writetext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value s = js_to_string(ctx, A(0));
  (void)t;
  if (s.type != JV_STR)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "writeText: invalid text"));
  if (clip_put_text(s.u.str->data, s.u.str->len) != 0)
    return js_promise_rejected(ctx, dom_exception(ctx, "NotAllowedError", "Clipboard write failed"));
  return js_promise_resolved(ctx, js_undef());
}

/* navigator.clipboard.readTex */
js_value clip_readtext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char mime[64];
  void *data = 0;
  int len = 0;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  if (clip_get(0, mime, sizeof mime, &data, &len) != 0)
    return js_promise_resolved(ctx, js_mk_cstring(ctx, ""));
  rv = mime_is_text(mime) ? js_mk_string(ctx, data ? (const char *)data : "", len) : js_mk_cstring(ctx, "");
  if (data)
    free(data);
  return js_promise_resolved(ctx, rv);
}

/* navigator.clipboard.write([ClipboardItem,...]) -> Promise<undefined>. The kernel clipboard stores */
js_value clip_write(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value items = A(0), item, data, entry;
  js_string *bytes;
  char mime[64];
  int rc, i;
  (void)t;
  if (items.type != JV_OBJ)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "write: expected ClipboardItems"));
  item = items.u.obj->kind == OBJ_ARRAY ? js_get_index(ctx, items, 0) : items;
  if (item.type != JV_OBJ)
    return js_promise_rejected(
        ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError", "write: empty ClipboardItem list"));
  data = js_get(ctx, item, "@data");
  if (data.type != JV_OBJ)
    data = item; /* tolerate a bare dict */
  entry = js_get(ctx, data, "text/plain");
  if (entry.type != JV_UNDEF && entry.type != JV_NULL) {
    strlcpy(mime, "text/plain", sizeof mime);
  } else {
    js_object *d = data.u.obj;
    mime[0] = '\0';
    for (i = 0; i < d->props.n; i++) {
      pentry *pe = &d->props.e[i];
      if (!(pe->flags & P_ENUMERABLE) || (unsigned char)pe->key[0] < ' ')
        continue;
      if (streq(pe->key, "@data"))
        continue;
      strlcpy(mime, pe->key, sizeof mime);
      entry = js_get(ctx, data, pe->key);
      break;
    }
    if (!mime[0])
      return js_promise_resolved(ctx, js_undef()); /* nothing to write */
  }
  gc_push_scope();
  bytes = clip_entry_bytes(ctx, entry, 0, 0);
  if (bytes)
    gc_keep(bytes);
  rc = bytes ? clip_put(mime, bytes->data, bytes->len) : clip_put(mime, "", 0);
  gc_pop_scope();
  if (rc != 0)
    return js_promise_rejected(ctx, dom_exception(ctx, "NotAllowedError", "Clipboard write failed"));
  return js_promise_resolved(ctx, js_undef());
}

/* navigator.clipboard.read() -> Promise<ClipboardItem[]> of the newest clip. */
js_value clip_read(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char mime[64];
  void *data = 0;
  int len = 0;
  js_object *arr;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (clip_get(0, mime, sizeof mime, &data, &len) == 0) {
    js_object *dict = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_value dictarg, item;
    js_def(dict, mime, make_blob(ctx, data ? (const char *)data : "", len, mime),
           P_ENUMERABLE | P_WRITABLE | P_CONFIGURABLE);
    dictarg = js_obj_value(dict);
    item = clipitem_ctor(ctx, js_undef(), &dictarg, 1);
    if (item.type == JV_OBJ)
      js_array_push(ctx, arr, item);
  }
  if (data)
    free(data);
  rv = js_obj_value(arr);
  gc_pop_scope();
  return js_promise_resolved(ctx, rv);
}
