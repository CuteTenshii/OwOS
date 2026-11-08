/* Document natives: collections, factories and DOMParser documents. */

#include "bindings.h"
#include "dom_query.h"

js_value doc_create_frag(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return wrap_node(dom_new_element("#frag"));
}
js_value doc_create_comment(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = dom_new_element("#comment");
  (void)t;
  if (n)
    dom_set_attr(n, "data", arg_str(ctx, A(0)));
  return wrap_node(n);
}
js_value doc_import_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return el_clone(ctx, A(0), args + 1, argc > 1 ? argc - 1 : 0);
}
js_value doc_adopt_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(A(0));
  (void)ctx;
  (void)t;
  if (n && n->parent)
    remove_node(n->parent, n);
  return A(0);
}
js_value doc_get_by_class(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (doc_root_of(t))
    collect_class(doc_root_of(t), arg_str(ctx, A(0)), arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value doc_contains(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(A(0));
  (void)ctx;
  return js_bool(n && dom_is_ancestor(doc_root_of(t), n));
}
js_value ev_init_event(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_set(ctx, t, "type", js_mk_cstring(ctx, arg_str(ctx, A(0))));
  js_set(ctx, t, "bubbles", js_bool(js_to_boolean(A(1))));
  js_set(ctx, t, "cancelable", js_bool(js_to_boolean(A(2))));
  if (argc > 3)
    js_set(ctx, t, "detail", A(3));
  return js_undef();
}
static js_value doc_collect_tag(js_ctx *ctx, js_value t, const char *tag)
{
  js_object *arr;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (doc_root_of(t))
    js_dom_find_all_tag(doc_root_of(t), tag, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value doc_scripts(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return doc_collect_tag(ctx, t, "script");
}
js_value doc_forms(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return doc_collect_tag(ctx, t, "form");
}
js_value doc_images(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return doc_collect_tag(ctx, t, "img");
}
js_value doc_links(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (doc_root_of(t))
    js_dom_query_all(doc_root_of(t), "a[href], area[href]", arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value doc_domain(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct purl u;
  (void)t;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, parse_url(g_page_url_buf, &u) ? u.host : "");
}
js_value doc_null(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_null();
}
js_value doc_false(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(0);
}
static bool has_dquote(const char *s)
{
  for (; *s; s++)
    if (*s == '"')
      return true;
  return false;
}

js_value doc_get_by_name(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  char sel[128];
  int k = 0;
  sapp(sel, &k, sizeof sel, "[name=\"");
  sapp(sel, &k, sizeof sel, arg_str(ctx, A(0)));
  sapp(sel, &k, sizeof sel, "\"]");
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (doc_root_of(t) && !has_dquote(arg_str(ctx, A(0))))
    js_dom_query_all(doc_root_of(t), sel, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
/* a detached document with the html/head/body skeleton (document.implementation) */
static js_value impl_create_html_document(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = dom_parse("<!doctype html><html><head></head><body></body></html>", 53), *ti;
  js_value dv = wrap_document(ctx, root);
  (void)t;
  if (root && argc > 0 && (ti = dom_new_element("title"))) {
    struct dom_node *head = js_dom_find_tag(root, "head");
    set_text_content(ti, arg_str(ctx, A(0)));
    dom_append_child(head ? head : root, ti);
  }
  return dv;
}
static js_value impl_create_document(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = dom_new_element("#document");
  (void)t;
  (void)args;
  (void)argc;
  return wrap_document(ctx, root);
}
js_value doc_implementation(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cached;
  js_object *impl;
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_null();
  if (js_has_own(t.u.obj, "@impl"))
    return js_get(ctx, t, "@impl");
  gc_push_scope();
  impl = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  def_method(impl, "createHTMLDocument", impl_create_html_document, 1);
  def_method(impl, "createDocument", impl_create_document, 3);
  def_method(impl, "hasFeature", el_true, 0);
  cached = js_obj_value(impl);
  js_def(t.u.obj, "@impl", cached, P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return cached;
}
js_value doc_create_event(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return make_event("", 0, 0, js_undef());
}
