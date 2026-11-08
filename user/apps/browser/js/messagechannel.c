/* MessageChannel and MessagePort: two entangled ports; delivery goes through the worker message queue. */

#include "jsint.h"

static js_object *g_port_proto;    /* MessagePort.prototype */
static js_object *g_channel_proto; /* MessageChannel.prototype */

static js_value clone_message(js_ctx *ctx, js_value v)
{
  js_value f = js_get(ctx, js_obj_value(ctx->global), "structuredClone"), r;
  if (v.type != JV_OBJ || !js_is_callable(f))
    return v;
  r = js_call(ctx, f, js_undef(), &v, 1);
  return ctx->has_exc ? v : r;
}

static js_value port_post(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value peer;
  (void)argc;
  if (t.type != JV_OBJ || js_to_boolean(js_get(ctx, t, "@closed")))
    return js_undef();
  peer = js_get(ctx, t, "@peer");
  if (peer.type == JV_OBJ && !js_to_boolean(js_get(ctx, peer, "@closed")))
    js_port_enqueue(peer, clone_message(ctx, A(0)));
  return js_undef();
}

static js_value port_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value peer;
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_undef();
  js_set(ctx, t, "@closed", js_bool(1));
  peer = js_get(ctx, t, "@peer");
  if (peer.type == JV_OBJ)
    js_set(ctx, peer, "@closed", js_bool(1));
  return js_undef();
}

static js_value port_illegal(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

static js_object *new_port(js_ctx *ctx)
{
  js_object *p = js_new_object(ctx, g_port_proto);
  js_def(p, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(p, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(p, "@closed", js_bool(0), P_WRITABLE);
  return p;
}

static js_value channel_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *ch, *p1, *p2;
  js_value rv;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  ch = (js_object *)gc_keep(js_new_object(ctx, g_channel_proto));
  p1 = (js_object *)gc_keep(new_port(ctx));
  p2 = (js_object *)gc_keep(new_port(ctx));
  js_def(p1, "@peer", js_obj_value(p2), P_WRITABLE);
  js_def(p2, "@peer", js_obj_value(p1), P_WRITABLE);
  js_def(ch, "port1", js_obj_value(p1), P_ENUMERABLE);
  js_def(ch, "port2", js_obj_value(p2), P_ENUMERABLE);
  rv = js_obj_value(ch);
  gc_pop_scope();
  return rv;
}

static void expose(js_ctx *ctx, js_object *win, const char *name, js_native fn, js_object *proto)
{
  js_object *ctor = js_new_native(ctx, fn, name, 0);
  gc_push_scope();
  gc_keep(ctor);
  js_def(ctor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, name, js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}

void js_msgchannel_setup(js_ctx *ctx, js_object *win)
{
  js_value et = js_get(ctx, js_obj_value(win), "EventTarget");
  js_value etp = et.type == JV_OBJ ? js_get(ctx, et, "prototype") : js_undef();
  g_port_proto = js_new_object(ctx, etp.type == JV_OBJ ? etp.u.obj : ctx->object_proto);
  gc_add_root((void **)&g_port_proto);
  def_method(g_port_proto, "postMessage", port_post, 1);
  def_method(g_port_proto, "start", el_noop, 0);
  def_method(g_port_proto, "close", port_close, 0);
  def_method(g_port_proto, "addEventListener", el_add_listener, 3);
  def_method(g_port_proto, "removeEventListener", el_remove_listener, 3);
  def_method(g_port_proto, "dispatchEvent", el_dispatch, 1);
  expose(ctx, win, "MessagePort", port_illegal, g_port_proto);
  g_channel_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_channel_proto);
  expose(ctx, win, "MessageChannel", channel_ctor, g_channel_proto);
}

void js_msgchannel_teardown(void)
{
  gc_remove_root((void **)&g_port_proto);
  gc_remove_root((void **)&g_channel_proto);
  g_port_proto = g_channel_proto = 0;
}
