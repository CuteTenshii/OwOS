/* navigator.* and crypto. */

#include "bindings.h"
#include "features/clienthints/clienthints.h"
#include "uuid.h"

/* --- navigator.userAgentData ---------------------------------------------- */
static js_value uad_brands(js_ctx *ctx, const struct ch_brand *b)
{
  js_object *arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (int i = 0; i < CH_NBRANDS; i++) {
    js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(o, "brand", js_mk_cstring(ctx, b[i].brand), P_ENUMERABLE);
    js_def(o, "version", js_mk_cstring(ctx, b[i].version), P_ENUMERABLE);
    js_array_push(ctx, arr, js_obj_value(o));
  }
  return js_obj_value(arr);
}

void uad_low_entropy(js_ctx *ctx, js_object *o)
{
  js_def(o, "brands", uad_brands(ctx, ch_ua.brands), P_ENUMERABLE);
  js_def(o, "mobile", js_bool(ch_ua.mobile), P_ENUMERABLE);
  js_def(o, "platform", js_mk_cstring(ctx, ch_ua.platform), P_ENUMERABLE);
}

js_value uad_tojson(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  uad_low_entropy(ctx, o);
  rv = js_obj_value(o);
  gc_pop_scope();
  return rv;
}

/* getHighEntropyValues(hints) -> Promise of the low-entropy values plus each requested hint */
js_value uad_high_entropy(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value hints = A(0), rv;
  (void)t;
  if (hints.type != JV_OBJ)
    return js_promise_rejected(ctx, js_make_error(ctx, ctx->type_error_proto, "TypeError",
                                                  "getHighEntropyValues requires a sequence of hints"));
  gc_push_scope();
  gc_keep(hints.u.obj);
  js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  uad_low_entropy(ctx, o);
  unsigned n = (unsigned)js_to_number(ctx, js_get(ctx, hints, "length"));
  for (unsigned i = 0; i < n; i++) {
    const char *h = arg_str(ctx, js_get_index(ctx, hints, i));
    if (streq(h, "architecture"))
      js_def(o, h, js_mk_cstring(ctx, ch_ua.architecture), P_ENUMERABLE);
    else if (streq(h, "bitness"))
      js_def(o, h, js_mk_cstring(ctx, ch_ua.bitness), P_ENUMERABLE);
    else if (streq(h, "model"))
      js_def(o, h, js_mk_cstring(ctx, ch_ua.model), P_ENUMERABLE);
    else if (streq(h, "platformVersion"))
      js_def(o, h, js_mk_cstring(ctx, ch_ua.platform_version), P_ENUMERABLE);
    else if (streq(h, "uaFullVersion"))
      js_def(o, h, js_mk_cstring(ctx, ch_ua.full_version), P_ENUMERABLE);
    else if (streq(h, "fullVersionList"))
      js_def(o, h, uad_brands(ctx, ch_ua.full_brands), P_ENUMERABLE);
    else if (streq(h, "wow64"))
      js_def(o, h, js_bool(ch_ua.wow64), P_ENUMERABLE);
    else if (streq(h, "formFactors")) {
      js_object *ff = (js_object *)gc_keep(js_new_array(ctx, 0));
      js_array_push(ctx, ff, js_mk_cstring(ctx, ch_ua.form_factor));
      js_def(o, h, js_obj_value(ff), P_ENUMERABLE);
    }
  }
  rv = js_promise_resolved(ctx, js_obj_value(o));
  gc_pop_scope();
  return rv;
}

/* --- navigator.geolocation ------------------------------------------------ */
static js_value geo_error_obj(js_ctx *ctx, int code, const char *msg)
{
  js_object *e = js_new_object(ctx, ctx->object_proto);
  js_def(e, "code", js_number(code), P_ENUMERABLE);
  js_def(e, "message", js_mk_cstring(ctx, msg), P_ENUMERABLE);
  js_def(e, "PERMISSION_DENIED", js_number(1), P_ENUMERABLE);
  js_def(e, "POSITION_UNAVAILABLE", js_number(2), P_ENUMERABLE);
  js_def(e, "TIMEOUT", js_number(3), P_ENUMERABLE);
  return js_obj_value(e);
}
js_value geo_get_current(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value err_cb = A(1), e;
  (void)t;
  int st = worker_permission(PERM_GEOLOCATION);
  gc_push_scope();
  e = st == PERM_GRANTED ? geo_error_obj(ctx, 2, "Location information is unavailable.")
                         : geo_error_obj(ctx, 1, "User denied Geolocation");
  if (e.type == JV_OBJ)
    gc_keep(e.u.obj);
  if (js_is_callable(err_cb)) {
    js_value a = e;
    js_call(ctx, err_cb, js_undef(), &a, 1);
    if (ctx->has_exc)
      report_exc(ctx);
  }
  gc_pop_scope();
  return js_undef();
}
js_value geo_watch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  geo_get_current(ctx, t, args, argc); /* one fix attempt, same as a single call */
  return js_number(1);                 /* a watch id; clearWatch is a no-op */
}

/* --- navigator.permissions.query ------------------------------------------ */
js_value perm_query(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value d = A(0), rv;
  const char *name = d.type == JV_OBJ ? arg_str(ctx, js_get(ctx, d, "name")) : "";
  const char *state = "prompt";
  int f = perm_feature_from_token(name);
  js_object *ps;
  (void)t;
  if (f >= 0) {
    int st = perm_state_now(f);
    state = st == PERM_GRANTED ? "granted" : st == PERM_DENIED ? "denied" : "prompt";
  } else if (streq_ci(name, "clipboard-read") || streq_ci(name, "clipboard-write")) {
    state = "granted";
  }
  gc_push_scope();
  ps = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(ps, "state", js_mk_cstring(ctx, state), P_ENUMERABLE);
  js_def(ps, "name", js_mk_cstring(ctx, name), P_ENUMERABLE);
  js_def(ps, "onchange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def_native(ctx, ps, "addEventListener", el_noop, 2);
  js_def_native(ctx, ps, "removeEventListener", el_noop, 2);
  rv = js_promise_resolved(ctx, js_obj_value(ps));
  gc_pop_scope();
  return rv;
}

/* ------------------------------------------------------- crypto ----------- */

/* Fill an integer TypedArray's backing bytes in place; returns the same array */
js_value crypto_getrandomvalues(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = A(0);
  js_typedarray *ta;
  js_arraybuffer *ab;
  int nbytes;
  (void)t;
  if (v.type != JV_OBJ || v.u.obj->kind != OBJ_TYPEDARRAY)
    return js_throw_type(ctx, "Failed to execute 'getRandomValues': argument is not an "
                              "integer-typed TypedArray");
  ta = (js_typedarray *)v.u.obj->host;
  if (!ta || ta->type < 0 || ta->type == TA_F32 || ta->type == TA_F64 || ta->type == TA_F16)
    return dom_throw(ctx, "TypeMismatchError", "getRandomValues requires an integer-typed TypedArray");
  nbytes = ta_length(ta) * ta_elem_size(ta->type);
  if (nbytes > 65536)
    return dom_throw(ctx, "QuotaExceededError", "getRandomValues can only generate a maximum of 65536 bytes");
  ab = (js_arraybuffer *)ta->buffer->host;
  if (ab && nbytes > 0)
    rng_bytes(ab->data + ta->offset, nbytes);
  return v;
}
js_value crypto_randomuuid(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  unsigned char rnd[16];
  uuid_t u;
  char s[UUID_STR_LEN];
  (void)t;
  (void)args;
  (void)argc;
  rng_bytes(rnd, sizeof rnd);
  uuid_v4_from(u, rnd);
  uuid_to_string(u, s, 0);
  return js_mk_cstring(ctx, s);
}

/* The DOM `Node` interface: not constructable */
js_value node_ctor(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
void def_node_consts(js_object *o)
{
  js_def(o, "ELEMENT_NODE", js_number(1), 0);
  js_def(o, "ATTRIBUTE_NODE", js_number(2), 0);
  js_def(o, "TEXT_NODE", js_number(3), 0);
  js_def(o, "CDATA_SECTION_NODE", js_number(4), 0);
  js_def(o, "ENTITY_REFERENCE_NODE", js_number(5), 0);
  js_def(o, "ENTITY_NODE", js_number(6), 0);
  js_def(o, "PROCESSING_INSTRUCTION_NODE", js_number(7), 0);
  js_def(o, "COMMENT_NODE", js_number(8), 0);
  js_def(o, "DOCUMENT_NODE", js_number(9), 0);
  js_def(o, "DOCUMENT_TYPE_NODE", js_number(10), 0);
  js_def(o, "DOCUMENT_FRAGMENT_NODE", js_number(11), 0);
  js_def(o, "NOTATION_NODE", js_number(12), 0);
}
