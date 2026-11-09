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
static void his_nav(js_ctx *ctx, js_value t, js_value *args, int argc, int replace)
{
  if (argc >= 3 && args[2].type != JV_UNDEF && args[2].type != JV_NULL) {
    char url[URL_MAX];
    int k = 0;
    resolve_url(g_page_url_buf, arg_str(ctx, A(2)), url, sizeof url);
    {
      js_value loc = js_get(ctx, js_obj_value(ctx->global), "location");
      js_set(ctx, loc, "href", js_mk_cstring(ctx, url));
    }
    if (g_document)
      js_set(ctx, js_obj_value(g_document), "URL", js_mk_cstring(ctx, url));
    for (; url[k] && k < URL_MAX - 1; k++)
      B->hist_url[k] = url[k];
    B->hist_url[k] = '\0';
    B->hist_replace = replace;
    B->hist_nav = 1;
    B->seq++;
  }
  if (t.type == JV_OBJ)
    js_set(ctx, t, "state", argc >= 1 ? args[0] : js_null());
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
