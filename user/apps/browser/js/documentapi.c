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

/* ------------------------------------------------------- TreeWalker / NodeIterator */

enum { FILTER_ACCEPT = 1, FILTER_REJECT = 2, FILTER_SKIP = 3, FILTER_ERR = -1 };

static js_object *g_walker_proto, *g_iterator_proto;

struct walk {
  js_ctx *ctx;
  unsigned show;
  js_value filter;
  struct dom_node *root, *cur;
};

static int node_type_of(const struct dom_node *n)
{
  if (n->type == DOM_TEXT)
    return 3;
  if (n->host || streq(n->tag, "#frag"))
    return 11;
  if (streq(n->tag, "#comment"))
    return 8;
  return streq(n->tag, "#document") ? 9 : 1;
}

static int walk_accept(struct walk *w, struct dom_node *n)
{
  js_value arg, fn, self, r;
  if (!((w->show >> (node_type_of(n) - 1)) & 1))
    return FILTER_SKIP;
  if (w->filter.type != JV_OBJ)
    return FILTER_ACCEPT;
  arg = wrap_node(n);
  fn = w->filter;
  self = js_undef();
  if (!js_is_callable(fn)) {
    fn = js_get(w->ctx, w->filter, "acceptNode");
    self = w->filter;
  }
  r = js_call(w->ctx, fn, self, &arg, 1);
  return w->ctx->has_exc ? FILTER_ERR : js_to_int32(w->ctx, r) & 0xffff;
}

/* Load the walker's state; the cursor property is currentNode or referenceNode */
static int walk_open(js_ctx *ctx, js_value t, struct walk *w, const char *cursor)
{
  w->ctx = ctx;
  w->show = (unsigned)js_to_uint32(ctx, js_get(ctx, t, "whatToShow"));
  w->filter = js_get(ctx, t, "filter");
  w->root = node_of(js_get(ctx, t, "root"));
  w->cur = node_of(js_get(ctx, t, cursor));
  return w->root && w->cur;
}

static struct dom_node *prev_sibling(const struct dom_node *n)
{
  struct dom_node *c;
  if (!n->parent)
    return 0;
  for (c = n->parent->first_child; c && c->next != n; c = c->next)
    ;
  return c;
}

static struct dom_node *tw_parent(struct walk *w)
{
  struct dom_node *n = w->cur;
  while (n && n != w->root) {
    int r;
    n = n->parent;
    if (!n)
      break;
    r = walk_accept(w, n);
    if (r == FILTER_ERR)
      return 0;
    if (r == FILTER_ACCEPT)
      return n;
  }
  return 0;
}

static struct dom_node *tw_child(struct walk *w, int first)
{
  struct dom_node *n = first ? w->cur->first_child : w->cur->last_child, *s;
  while (n) {
    int r = walk_accept(w, n);
    if (r == FILTER_ERR)
      return 0;
    if (r == FILTER_ACCEPT)
      return n;
    s = r == FILTER_SKIP ? (first ? n->first_child : n->last_child) : 0;
    if (s) {
      n = s;
      continue;
    }
    while (n) {
      s = first ? n->next : prev_sibling(n);
      if (s) {
        n = s;
        break;
      }
      n = n->parent;
      if (!n || n == w->root || n == w->cur)
        return 0;
    }
  }
  return 0;
}

static struct dom_node *tw_sibling(struct walk *w, int next)
{
  struct dom_node *n = w->cur, *s;
  int r;
  if (n == w->root)
    return 0;
  for (;;) {
    s = next ? n->next : prev_sibling(n);
    while (s) {
      n = s;
      r = walk_accept(w, n);
      if (r == FILTER_ERR)
        return 0;
      if (r == FILTER_ACCEPT)
        return n;
      s = next ? n->first_child : n->last_child;
      if (r == FILTER_REJECT || !s)
        s = next ? n->next : prev_sibling(n);
    }
    n = n->parent;
    if (!n || n == w->root)
      return 0;
    r = walk_accept(w, n);
    if (r == FILTER_ERR || r == FILTER_ACCEPT)
      return 0;
  }
}

static struct dom_node *tw_previous(struct walk *w)
{
  struct dom_node *n = w->cur, *s;
  int r;
  while (n != w->root) {
    for (s = prev_sibling(n); s; s = prev_sibling(n)) {
      n = s;
      r = walk_accept(w, n);
      while (r != FILTER_REJECT && r != FILTER_ERR && n->last_child) {
        n = n->last_child;
        r = walk_accept(w, n);
      }
      if (r == FILTER_ERR)
        return 0;
      if (r == FILTER_ACCEPT)
        return n;
    }
    if (n == w->root || !n->parent)
      return 0;
    n = n->parent;
    r = walk_accept(w, n);
    if (r == FILTER_ERR)
      return 0;
    if (r == FILTER_ACCEPT)
      return n;
  }
  return 0;
}

static struct dom_node *tw_next(struct walk *w)
{
  struct dom_node *n = w->cur, *s, *up;
  int r = FILTER_ACCEPT;
  for (;;) {
    while (r != FILTER_REJECT && n->first_child) {
      n = n->first_child;
      r = walk_accept(w, n);
      if (r == FILTER_ERR)
        return 0;
      if (r == FILTER_ACCEPT)
        return n;
    }
    s = 0;
    for (up = n; up; up = up->parent) {
      if (up == w->root)
        return 0;
      s = up->next;
      if (s)
        break;
    }
    if (!s)
      return 0;
    n = s;
    r = walk_accept(w, n);
    if (r == FILTER_ERR)
      return 0;
    if (r == FILTER_ACCEPT)
      return n;
  }
}

static js_value walker_move(js_ctx *ctx, js_value t, struct dom_node *n)
{
  js_value v;
  if (ctx->has_exc)
    return js_undef();
  if (!n)
    return js_null();
  v = wrap_node(n);
  js_set(ctx, t, "currentNode", v);
  return v;
}

#define WALKER_OP(fn, expr)                                                                                            \
  static js_value fn(js_ctx *ctx, js_value t, js_value *args, int argc)                                                \
  {                                                                                                                    \
    struct walk w;                                                                                                     \
    (void)args;                                                                                                        \
    (void)argc;                                                                                                        \
    if (!walk_open(ctx, t, &w, "currentNode"))                                                                         \
      return js_null();                                                                                                \
    return walker_move(ctx, t, expr);                                                                                  \
  }
WALKER_OP(tw_parent_node, tw_parent(&w))
WALKER_OP(tw_first_child, tw_child(&w, 1))
WALKER_OP(tw_last_child, tw_child(&w, 0))
WALKER_OP(tw_next_sibling, tw_sibling(&w, 1))
WALKER_OP(tw_previous_sibling, tw_sibling(&w, 0))
WALKER_OP(tw_previous_node, tw_previous(&w))
WALKER_OP(tw_next_node, tw_next(&w))
#undef WALKER_OP

static struct dom_node *ni_following(const struct dom_node *n, const struct dom_node *root)
{
  if (n->first_child)
    return n->first_child;
  for (; n && n != root; n = n->parent)
    if (n->next)
      return n->next;
  return 0;
}

static struct dom_node *ni_preceding(const struct dom_node *n, const struct dom_node *root)
{
  struct dom_node *s;
  if (n == root)
    return 0;
  s = prev_sibling(n);
  if (!s)
    return n->parent;
  while (s->last_child)
    s = s->last_child;
  return s;
}

static js_value ni_step(js_ctx *ctx, js_value t, int next)
{
  struct walk w;
  struct dom_node *n;
  int before, r;
  if (!walk_open(ctx, t, &w, "referenceNode"))
    return js_null();
  n = w.cur;
  before = js_to_boolean(js_get(ctx, t, "pointerBeforeReferenceNode"));
  do {
    if (next) {
      if (before)
        before = 0;
      else if (!(n = ni_following(n, w.root)))
        return js_null();
    } else if (!before) {
      before = 1;
    } else if (!(n = ni_preceding(n, w.root))) {
      return js_null();
    }
    r = walk_accept(&w, n);
    if (r == FILTER_ERR)
      return js_undef();
  } while (r != FILTER_ACCEPT);
  js_def(t.u.obj, "referenceNode", wrap_node(n), P_ENUMERABLE);
  js_def(t.u.obj, "pointerBeforeReferenceNode", js_bool(before), P_ENUMERABLE);
  return wrap_node(n);
}

static js_value ni_next_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return ni_step(ctx, t, 1);
}

static js_value ni_previous_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  return ni_step(ctx, t, 0);
}

static js_value make_walker(js_ctx *ctx, js_object *proto, js_value *args, int argc, int iterator)
{
  js_object *o;
  js_value show = js_number(4294967295.0);
  if (!node_of(A(0)))
    return js_throw_type(ctx, "root must be a Node");
  if (A(1).type != JV_UNDEF)
    show = js_number(js_to_uint32(ctx, A(1)));
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, proto));
  js_def(o, "root", A(0), P_ENUMERABLE);
  js_def(o, "whatToShow", show, P_ENUMERABLE);
  js_def(o, "filter", A(2).type == JV_OBJ ? A(2) : js_null(), P_ENUMERABLE);
  if (iterator) {
    js_def(o, "referenceNode", A(0), P_ENUMERABLE);
    js_def(o, "pointerBeforeReferenceNode", js_bool(1), P_ENUMERABLE);
  } else {
    js_def(o, "currentNode", A(0), P_WRITABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
  return js_obj_value(o);
}

js_value doc_create_tree_walker(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return make_walker(ctx, g_walker_proto, args, argc, 0);
}

js_value doc_create_node_iterator(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  return make_walker(ctx, g_iterator_proto, args, argc, 1);
}

static js_value ni_detach(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}

void treewalker_install(js_ctx *ctx, js_object *win)
{
  static const struct {
    const char *name;
    unsigned value;
  } consts[] = {{"FILTER_ACCEPT", 1},
                {"FILTER_REJECT", 2},
                {"FILTER_SKIP", 3},
                {"SHOW_ALL", 0xffffffffu},
                {"SHOW_ELEMENT", 0x1},
                {"SHOW_ATTRIBUTE", 0x2},
                {"SHOW_TEXT", 0x4},
                {"SHOW_CDATA_SECTION", 0x8},
                {"SHOW_ENTITY_REFERENCE", 0x10},
                {"SHOW_ENTITY", 0x20},
                {"SHOW_PROCESSING_INSTRUCTION", 0x40},
                {"SHOW_COMMENT", 0x80},
                {"SHOW_DOCUMENT", 0x100},
                {"SHOW_DOCUMENT_TYPE", 0x200},
                {"SHOW_DOCUMENT_FRAGMENT", 0x400},
                {"SHOW_NOTATION", 0x800}};
  js_object *nf;
  unsigned i;
  g_walker_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_walker_proto);
  def_method(g_walker_proto, "parentNode", tw_parent_node, 0);
  def_method(g_walker_proto, "firstChild", tw_first_child, 0);
  def_method(g_walker_proto, "lastChild", tw_last_child, 0);
  def_method(g_walker_proto, "nextSibling", tw_next_sibling, 0);
  def_method(g_walker_proto, "previousSibling", tw_previous_sibling, 0);
  def_method(g_walker_proto, "previousNode", tw_previous_node, 0);
  def_method(g_walker_proto, "nextNode", tw_next_node, 0);
  g_iterator_proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)&g_iterator_proto);
  def_method(g_iterator_proto, "nextNode", ni_next_node, 0);
  def_method(g_iterator_proto, "previousNode", ni_previous_node, 0);
  def_method(g_iterator_proto, "detach", ni_detach, 0);
  gc_push_scope();
  nf = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  for (i = 0; i < sizeof consts / sizeof consts[0]; i++)
    js_def(nf, consts[i].name, js_number(consts[i].value), P_ENUMERABLE);
  js_def(win, "NodeFilter", js_obj_value(nf), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
}
