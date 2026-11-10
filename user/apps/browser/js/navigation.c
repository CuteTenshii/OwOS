/* History API and location navigation. */

#include "bindings.h"

/* History API, wired to the browser's per-tab back/forward history */
js_value his_back(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  B->hist_go = -1;
  B->seq++;
  return js_undef();
}
js_value his_forward(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  B->hist_go = 1;
  B->seq++;
  return js_undef();
}
js_value his_go(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int d = argc >= 1 ? js_to_int_sat(ctx, args[0]) : 0;
  (void)t;
  B->hist_go = d < 0 ? -1 : (d > 0 ? 1 : 0);
  if (B->hist_go)
    B->seq++;
  return js_undef();
}
js_value his_length(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_number(B->hist_len > 0 ? B->hist_len : 1);
}
static void set_page_url(js_ctx *ctx, const char *url)
{
  js_value loc = js_get(ctx, js_obj_value(ctx->global), "location");
  if (loc.type == JV_OBJ)
    fill_url_props(ctx, loc.u.obj, url);
  if (g_document) {
    js_set(ctx, js_obj_value(g_document), "URL", js_mk_cstring(ctx, url));
    js_set(ctx, js_obj_value(g_document), "documentURI", js_mk_cstring(ctx, url));
  }
  strlcpy(g_page_url_buf, url, URL_MAX);
}
/* The per-entry state store: history["@states"]["k<key>"], with "@cur"/"@next" the live and last-issued keys */
static js_object *his_states(js_ctx *ctx, js_object *h)
{
  js_value s = js_get(ctx, js_obj_value(h), "@states");
  if (s.type != JV_OBJ) {
    gc_push_scope();
    s = js_obj_value((js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)));
    js_def(h, "@states", s, P_WRITABLE);
    gc_pop_scope();
  }
  return s.u.obj;
}
static void his_nav(js_ctx *ctx, js_value t, js_value *args, int argc, int replace)
{
  char url[URL_MAX], name[16];
  js_value state = argc >= 1 && args[0].type != JV_UNDEF ? args[0] : js_null();
  int key;
  if (t.type != JV_OBJ)
    return;
  if (argc >= 3 && args[2].type != JV_UNDEF && args[2].type != JV_NULL)
    resolve_url(g_page_url_buf, arg_str(ctx, A(2)), url, sizeof url);
  else
    strlcpy(url, g_page_url_buf, sizeof url);
  key = js_to_int_sat(ctx, js_get(ctx, t, "@cur"));
  if (!replace) {
    key = js_to_int_sat(ctx, js_get(ctx, t, "@next")) + 1;
    js_set(ctx, t, "@next", js_number(key));
    js_set(ctx, t, "@cur", js_number(key));
  }
  snprintf(name, sizeof name, "k%d", key);
  js_set(ctx, js_obj_value(his_states(ctx, t.u.obj)), name, state);
  js_set(ctx, t, "state", state);
  set_page_url(ctx, url);
  strlcpy(B->hist_url, url, sizeof B->hist_url);
  B->hist_key = key;
  B->hist_replace = replace;
  B->hist_nav = 1;
  B->seq++;
}
js_value his_pushstate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  his_nav(ctx, t, args, argc, 0);
  return js_undef();
}
js_value his_replacestate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  his_nav(ctx, t, args, argc, 1);
  return js_undef();
}

/* location.assign/replace/reload: a real navigation, posted to the main thread */
js_value loc_assign(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)argc;
  resolve_url(g_page_url_buf, arg_str(ctx, A(0)), B->loc_url, sizeof B->loc_url);
  B->loc_nav = 1;
  B->seq++;
  return js_undef();
}
js_value loc_reload(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  strlcpy(B->loc_url, g_page_url_buf, sizeof B->loc_url);
  B->loc_nav = 1;
  B->seq++;
  return js_undef();
}
js_value loc_tostring(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return js_get(ctx, t, "href");
}

/* Worker thread: the page is going away (navigation or tab switch), so it becomes hidden */
void js_page_hide(void)
{
  js_value doc, ev;
  if (!g_page_live || !g_ctx || js_irq_stopped(g_ctx->irq))
    return;
  doc = js_obj_value(g_document);
  if (js_to_boolean(js_get(g_ctx, doc, "hidden")))
    return;
  js_set(g_ctx, doc, "visibilityState", js_mk_cstring(g_ctx, "hidden"));
  js_set(g_ctx, doc, "hidden", js_bool(1));
  gc_push_scope();
  ev = make_event("visibilitychange", 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "cancelable", js_bool(0));
    dispatch_from(g_document, ev);
  }
  gc_pop_scope();
}

/* Worker thread: main moved to another entry of this document; restore its URL and state, then fire popstate */
void js_history_pop(int key, const char *url)
{
  js_value h, st, ev;
  char name[16];
  if (!g_page_live || !g_ctx || js_irq_stopped(g_ctx->irq))
    return;
  h = js_get(g_ctx, js_obj_value(g_ctx->global), "history");
  if (h.type != JV_OBJ)
    return;
  gc_push_scope();
  gc_keep(h.u.obj);
  snprintf(name, sizeof name, "k%d", key);
  st = js_get(g_ctx, js_obj_value(his_states(g_ctx, h.u.obj)), name);
  if (st.type == JV_UNDEF)
    st = js_null();
  set_page_url(g_ctx, url);
  js_set(g_ctx, h, "@cur", js_number(key));
  js_set(g_ctx, h, "state", st);
  ev = make_event("popstate", 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    js_set(g_ctx, ev, "bubbles", js_bool(0));
    js_set(g_ctx, ev, "cancelable", js_bool(0));
    js_set(g_ctx, ev, "state", st);
    dispatch_from(g_ctx->global, ev);
  }
  gc_pop_scope();
}
