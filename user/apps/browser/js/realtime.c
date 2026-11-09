/* RTCPeerConnection stub, WebSocket and BroadcastChannel. */

#include "bindings.h"
#include "websocket.h"

/* ------------------------------------------------- RTCPeerConnection ------- */
/* OwOS has no WebRTC/ICE stack. Sites `new` RTCPeerConnection to feature-detect */

js_value rtc_noop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}

js_value rtc_create_data_channel(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *dc;
  js_value rv;
  (void)t;
  gc_push_scope();
  dc = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(dc, "label", argc > 0 ? js_to_string(ctx, A(0)) : js_mk_cstring(ctx, ""), P_ENUMERABLE);
  js_def(dc, "readyState", js_mk_cstring(ctx, "connecting"), P_ENUMERABLE);
  js_def(dc, "onopen", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(dc, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(dc, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def_native(ctx, dc, "send", rtc_noop, 1);
  js_def_native(ctx, dc, "close", rtc_noop, 0);
  js_def_native(ctx, dc, "addEventListener", rtc_noop, 3);
  js_def_native(ctx, dc, "removeEventListener", rtc_noop, 3);
  rv = js_obj_value(dc);
  gc_pop_scope();
  return rv;
}

/* createOffer/createAnswer: the legacy (successCb */
static js_value rtc_offer_like(js_ctx *ctx, js_value *args, int argc, const char *type)
{
  js_object *o;
  js_value rv;
  if (js_is_callable(A(0)))
    return js_undef();
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(o, "type", js_mk_cstring(ctx, type), P_ENUMERABLE);
  js_def(o, "sdp", js_mk_cstring(ctx, ""), P_ENUMERABLE);
  rv = js_promise_resolved(ctx, js_obj_value(o));
  gc_pop_scope();
  return rv;
}
js_value rtc_create_offer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return rtc_offer_like(ctx, args, argc, "offer");
}
js_value rtc_create_answer(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return rtc_offer_like(ctx, args, argc, "answer");
}

/* setLocalDescription/setRemoteDescription/addIceCandidate: inert for the legacy */
js_value rtc_set_desc(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (js_is_callable(A(1)))
    return js_undef();
  return js_promise_resolved(ctx, js_undef());
}

js_value rtc_get_stats(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o;
  js_value rv;
  (void)t;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "forEach", rtc_noop, 1); /* an empty stats report */
  js_def_native(ctx, o, "values", rtc_noop, 0);
  rv = js_promise_resolved(ctx, js_obj_value(o));
  gc_pop_scope();
  return rv;
}

js_value rtc_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *self;
  js_value rv;
  (void)thisv;
  (void)args;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  self = (js_object *)gc_keep(js_new_object(ctx, g_rtc_proto));
  js_def(self, "iceGatheringState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "iceConnectionState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "connectionState", js_mk_cstring(ctx, "new"), P_ENUMERABLE);
  js_def(self, "signalingState", js_mk_cstring(ctx, "stable"), P_ENUMERABLE);
  js_def(self, "localDescription", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "remoteDescription", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onicecandidate", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "ondatachannel", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "oniceconnectionstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onconnectionstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onsignalingstatechange", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(self, "onnegotiationneeded", js_null(), P_WRITABLE | P_ENUMERABLE);
  rv = js_obj_value(self);
  gc_pop_scope();
  return rv;
}

/* ------------------------------------------------------- WebSocket -------- */
/* A WebSocket wrapper's `host` slot points at the ws_conn */

static ws_conn *ws_of(js_value t) { return (t.type == JV_OBJ) ? (ws_conn *)t.u.obj->host : 0; }

/* Fire a non-bubbling `type` event on a target object */
void emit_event(js_object *w, const char *type, js_value data)
{
  js_value ev;
  char on[24];
  gc_push_scope();
  gc_keep(w);
  ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "target", js_obj_value(w));
    js_set(g_ctx, ev, "currentTarget", js_obj_value(w));
    if (data.type != JV_UNDEF)
      js_def(ev.u.obj, "data", data, P_WRITABLE | P_ENUMERABLE);
  }
  on[0] = 'o';
  on[1] = 'n';
  strlcpy(on + 2, type, (int)sizeof on - 2);
  {
    js_value h = js_get(g_ctx, js_obj_value(w), on);
    if (js_is_callable(h)) {
      js_value a = ev;
      js_set_event_script(type, 0);
      js_call(g_ctx, h, js_obj_value(w), &a, 1);
      if (g_ctx->has_exc)
        report_exc(g_ctx);
    }
  }
  ev_fire_on(w, ev, 0); /* addEventListener listeners */
  gc_pop_scope();
}

js_value ws_send_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_of(t);
  js_value d = A(0);
  (void)argc;
  if (!c || ws_state(c) != WS_OPEN)
    return js_undef();
  if (d.type == JV_STR) {
    ws_send(c, d.u.str->data, d.u.str->len, 1);
  } else { /* no ArrayBuffer yet: coerce to text */
    js_value s = js_to_string(ctx, d);
    if (s.type == JV_STR)
      ws_send(c, s.u.str->data, s.u.str->len, 1);
  }
  return js_undef();
}

js_value ws_close_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  ws_conn *c = ws_of(t);
  int code = (argc > 0 && args[0].type == JV_NUM) ? (int)args[0].u.num : 1000;
  const char *reason = (argc > 1) ? arg_str(ctx, A(1)) : "";
  if (c && ws_state(c) != WS_CLOSED) {
    ws_close(c, code, reason);
    js_set(ctx, t, "readyState", js_number(WS_CLOSING));
  }
  return js_undef();
}

js_value ws_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *url = arg_str(ctx, A(0));
  char protos[256];
  ws_conn *c;
  js_object *o;
  js_value ov;
  (void)thisv;
  protos[0] = 0;
  if (argc > 1) { /* a subprotocol string or array */
    js_value p = A(1);
    if (p.type == JV_STR) {
      strlcpy(protos, p.u.str->data, sizeof protos);
    } else if (p.type == JV_OBJ) {
      unsigned n = js_array_len(p.u.obj), i;
      int k = 0; /* page-controlled: bound each append */
      for (i = 0; i < n; i++) {
        const char *s = arg_str(ctx, js_get_index(ctx, p, i));
        if (!*s)
          continue;
        if (k)
          sapp(protos, &k, sizeof protos, ",");
        sapp(protos, &k, sizeof protos, s);
      }
    }
  }

  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_ws_proto));
  bool ws_blocked = false;
  if (B->pg->csp) { /* CSP connect-src (ws->http for matching) */
    char norm[URL_MAX], use[URL_MAX], reason[64];
    int k = 0;
    norm[0] = '\0';
    if (starts_ci(url, "wss://")) {
      sapp(norm, &k, sizeof norm, "https://");
      sapp(norm, &k, sizeof norm, url + 6);
    } else if (starts_ci(url, "ws://")) {
      sapp(norm, &k, sizeof norm, "http://");
      sapp(norm, &k, sizeof norm, url + 5);
    } else {
      sapp(norm, &k, sizeof norm, url);
    }
    if (!csp_check_url(B->pg->csp, CSP_CONNECT, norm, g_page_url_buf, use, sizeof use, reason, sizeof reason)) {
      js_csp_log("WebSocket to", url, reason);
      ws_blocked = true;
    }
  }
  c = ws_blocked ? 0 : ws_open(url, protos[0] ? protos : 0); /* blocks for connect + handshake */
  o->host = c;
  js_def(o, "url", js_mk_cstring(ctx, url), P_ENUMERABLE);
  js_def(o, "readyState", js_number(c ? WS_OPEN : WS_CLOSED), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "bufferedAmount", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "protocol", js_mk_cstring(ctx, c ? ws_protocol(c) : ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "extensions", js_mk_cstring(ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "binaryType", js_mk_cstring(ctx, "arraybuffer"), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onopen", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "@open_sent", js_bool(0), P_WRITABLE);    /* "open" delivered (pump) */
  js_def(o, "@dead", js_bool(c ? 0 : 1), P_WRITABLE); /* connect failed: emit error */
  if (g_websockets)
    js_array_push(ctx, g_websockets, js_obj_value(o));
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}

/* Drive every live WebSocket: deliver the deferred "open", drain inbound messages */
int js_websockets_pump(void)
{
  int fired = 0;
  unsigned i, n;
  if (!g_page_live || !g_ctx || !g_websockets)
    return 0;
  n = js_array_len(g_websockets);
  for (i = 0; i < n; i++) {
    js_value wv = js_get_index(g_ctx, js_obj_value(g_websockets), i);
    js_object *w;
    ws_conn *c;
    if (wv.type != JV_OBJ)
      continue;
    w = wv.u.obj;
    c = (ws_conn *)w->host;

    if (!c) { /* failed connect, or already closed */
      if (js_to_boolean(js_get(g_ctx, wv, "@dead"))) {
        js_set(g_ctx, wv, "@dead", js_bool(0));
        js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
        emit_event(w, "error", js_undef());
        emit_event(w, "close", js_undef());
        fired++;
      }
      continue;
    }

    if (!js_to_boolean(js_get(g_ctx, wv, "@open_sent"))) {
      js_set(g_ctx, wv, "@open_sent", js_bool(1));
      js_set(g_ctx, wv, "readyState", js_number(WS_OPEN));
      emit_event(w, "open", js_undef());
      fired++;
      c = (ws_conn *)w->host; /* a handler may have closed it */
      if (!c)
        continue;
    }

    for (;;) {
      const uint8_t *data = 0;
      int dlen = 0, is_text = 0, ev;
      ev = ws_poll(c, &data, &dlen, &is_text);
      if (ev == WS_EV_MESSAGE) {
        emit_event(w, "message", js_mk_string(g_ctx, (const char *)data, dlen));
        fired++;
        if (w->host != c)
          break; /* handler closed it mid-drain */
        continue;
      }
      if (ev == WS_EV_CLOSE || ev == WS_EV_ERROR) {
        if (ev == WS_EV_ERROR)
          emit_event(w, "error", js_undef());
        js_set(g_ctx, wv, "readyState", js_number(WS_CLOSED));
        emit_event(w, "close", js_undef());
        ws_free(c);
        w->host = 0;
        fired++;
      }
      break; /* WS_EV_NONE, or terminal */
    }
  }
  return fired;
}

/* ------------------------------------------------------- BroadcastChannel - */

/* A MessageEvent over the shared Event.prototype (so preventDefault/stopPropagation exist) */
js_value make_message_event(js_value data, js_value ports)
{
  js_object *e;
  js_value loc, org;
  const char *origin;
  loc = js_get(g_ctx, js_obj_value(g_ctx->global), "location");
  org = (loc.type == JV_OBJ) ? js_get(g_ctx, loc, "origin") : js_undef();
  origin = (org.type == JV_STR) ? org.u.str->data : "";
  gc_push_scope();
  if (data.type == JV_OBJ)
    gc_keep(data.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, g_event_proto));
  js_def(e, "type", js_mk_cstring(g_ctx, "message"), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "data", data, P_WRITABLE | P_ENUMERABLE);
  js_def(e, "origin", js_mk_cstring(g_ctx, origin), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "lastEventId", js_mk_cstring(g_ctx, ""), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "source", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "ports", ports.type == JV_OBJ ? ports : js_obj_value(js_new_array(g_ctx, 0)), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "bubbles", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "cancelable", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "defaultPrevented", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "target", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "currentTarget", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "@dp", js_bool(0), P_WRITABLE);
  js_def(e, "@sp", js_bool(0), P_WRITABLE);
  gc_pop_scope();
  event_adopt_iface(js_obj_value(e), "MessageEvent");
  return js_obj_value(e);
}

/* postMessage(msg): a clone for every other open same-named channel, in this page and in its workers */
js_value bc_post_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value namev;
  js_clone *c;
  if (t.type != JV_OBJ || !g_bchannels || js_to_boolean(js_get(ctx, t, "@closed")))
    return js_undef(); /* no-op after close */
  c = js_clone_write(ctx, A(0), js_undef());
  if (!c)
    return js_undef();
  namev = js_get(ctx, t, "name");
  js_bc_dispatch(namev.type == JV_STR ? namev.u.str->data : "", c, t.u.obj, 0);
  js_clone_free(c);
  return js_undef();
}

js_value bc_close_m(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ)
    js_set(ctx, t, "@closed", js_bool(1));
  return js_undef();
}

js_value bchannel_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  const char *name = arg_str(ctx, A(0));
  js_object *o;
  js_value ov;
  (void)thisv;
  (void)argc; /* native ctor builds its own object */
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_bchannel_proto));
  js_def(o, "name", js_mk_cstring(ctx, name), P_ENUMERABLE); /* readonly */
  js_def(o, "onmessage", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "onmessageerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(o, "@closed", js_bool(0), P_WRITABLE);
  if (g_bchannels)
    js_array_push(ctx, g_bchannels, js_obj_value(o));
  ov = js_obj_value(o);
  gc_pop_scope();
  return ov;
}
