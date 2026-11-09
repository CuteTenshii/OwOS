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

/* ------------------------------------------------------- MutationObserver -- *
 * An observer holds @cb, @regs (one {node, flags, filter} per observed target) and @queue (pending records).
 * dom_on_mutate stays NULL until the first observe(); records are delivered from a 0ms timer, like slotchange.
 * Nodes that clear_children frees show up in removedNodes as detached wrappers. */

enum { MO_CHILDREN = 1, MO_ATTRS = 2, MO_DATA = 4, MO_SUBTREE = 8, MO_ATTR_OLD = 16, MO_DATA_OLD = 32 };

static js_object *g_mo_proto, *g_mo_all, *g_mo_pending;

static void mo_schedule_flush(void);

static int mo_opt(js_ctx *ctx, js_value opts, const char *name) { return js_to_boolean(js_get(ctx, opts, name)); }

static int mo_filter_allows(js_ctx *ctx, js_value filter, const char *name)
{
  unsigned i;
  if (!js_is_array(filter))
    return 1;
  for (i = 0; i < js_array_len(filter.u.obj); i++) {
    js_value f = js_to_string(ctx, js_get_index(ctx, filter, i));
    if (f.type == JV_STR && streq_ci(f.u.str->data, name))
      return 1;
  }
  return 0;
}

static js_value mo_node_list(js_ctx *ctx, struct dom_node *first, int chain)
{
  js_object *arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (; first; first = chain ? first->next : 0)
    js_array_push(ctx, arr, wrap_node(first));
  return js_obj_value(arr);
}

static void mo_queue_record(js_ctx *ctx, js_object *observer, const struct dom_mutation *m, const char *type,
                            int want_old)
{
  js_object *rec;
  js_value queue = js_get(ctx, js_obj_value(observer), "@queue");
  unsigned f = P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE;
  if (!js_is_array(queue))
    return;
  gc_push_scope();
  rec = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(rec, "type", js_mk_cstring(ctx, type), f);
  js_def(rec, "target", wrap_node(m->target), f);
  js_def(rec, "addedNodes", mo_node_list(ctx, m->added, 0), f);
  js_def(rec, "removedNodes", mo_node_list(ctx, m->removed, m->kind == DOM_MUT_CLEAR), f);
  js_def(rec, "previousSibling", wrap_node(m->prev), f);
  js_def(rec, "nextSibling", wrap_node(m->next), f);
  js_def(rec, "attributeName",
         m->kind == DOM_MUT_ATTR && m->name && type[0] == 'a' ? js_mk_cstring(ctx, m->name) : js_null(), f);
  js_def(rec, "attributeNamespace", js_null(), f);
  js_def(rec, "oldValue", want_old && m->old ? js_mk_cstring(ctx, m->old) : js_null(), f);
  if (js_array_len(queue.u.obj) == 0) {
    js_array_push(ctx, g_mo_pending, js_obj_value(observer));
    mo_schedule_flush();
  }
  js_array_push(ctx, queue.u.obj, js_obj_value(rec));
  gc_pop_scope();
}

static void mo_notify(const struct dom_mutation *m)
{
  js_ctx *ctx = g_ctx;
  const char *type;
  unsigned i, j;
  if (!ctx || !g_page_live || !g_mo_all)
    return;
  if (m->kind == DOM_MUT_TEXT || (m->kind == DOM_MUT_ATTR && node_is_comment(m->target) && streq(m->name, "data")))
    type = "characterData";
  else
    type = m->kind == DOM_MUT_ATTR ? "attributes" : "childList";
  gc_push_scope();
  for (i = 0; i < js_array_len(g_mo_all); i++) {
    js_value ov = js_get_index(ctx, js_obj_value(g_mo_all), i), regs = js_get(ctx, ov, "@regs");
    int matched = 0, want_old = 0;
    if (ov.type != JV_OBJ || !js_is_array(regs))
      continue;
    for (j = 0; j < js_array_len(regs.u.obj); j++) {
      js_value reg = js_get_index(ctx, regs, j), filter = js_get(ctx, reg, "filter");
      struct dom_node *r = node_of(js_get(ctx, reg, "node"));
      int fl = (int)js_to_number(ctx, js_get(ctx, reg, "flags"));
      if (!r || (r != m->target && !((fl & MO_SUBTREE) && dom_is_ancestor(r, m->target))))
        continue;
      if (type[2] == 'i'   ? !(fl & MO_CHILDREN)
          : type[0] == 'a' ? !(fl & MO_ATTRS) || !mo_filter_allows(ctx, filter, m->name)
                           : !(fl & MO_DATA))
        continue;
      matched = 1;
      want_old |= type[0] == 'a' ? !!(fl & MO_ATTR_OLD) : !!(fl & MO_DATA_OLD);
    }
    if (matched)
      mo_queue_record(ctx, ov.u.obj, m, type, want_old);
  }
  gc_pop_scope();
}

static js_value mo_flush(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value list = js_obj_value(g_mo_pending);
  unsigned i;
  (void)thisv;
  (void)args;
  (void)argc;
  if (!g_mo_pending)
    return js_undef();
  gc_push_scope();
  gc_keep(list.u.obj);
  g_mo_pending = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(list.u.obj); i++) {
    js_value ov = js_get_index(ctx, list, i), queue = js_get(ctx, ov, "@queue"), cb = js_get(ctx, ov, "@cb");
    if (ov.type != JV_OBJ || !js_is_array(queue) || js_array_len(queue.u.obj) == 0)
      continue;
    gc_keep(queue.u.obj);
    js_def(ov.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
    {
      js_value argv[2];
      argv[0] = queue;
      argv[1] = ov;
      js_call(ctx, cb, ov, argv, 2);
      if (ctx->has_exc)
        report_exc_v(ctx);
    }
  }
  gc_pop_scope();
  return js_undef();
}

static void mo_schedule_flush(void)
{
  js_value set_timeout = js_get(g_ctx, js_obj_value(g_ctx->global), "setTimeout"), argv[2];
  if (!js_is_callable(set_timeout))
    return;
  gc_push_scope();
  argv[0] = js_obj_value((js_object *)gc_keep(js_new_native(g_ctx, mo_flush, "mutationobserver", 0)));
  argv[1] = js_number(0);
  js_call(g_ctx, set_timeout, js_obj_value(g_ctx->global), argv, 2);
  gc_pop_scope();
}

static js_value mo_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  if (!js_is_callable(A(0)))
    return js_throw_type(ctx, "MutationObserver: callback must be a function");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_mo_proto));
  js_def(o, "@cb", A(0), 0);
  js_def(o, "@regs", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(o, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
  return js_obj_value(o);
}

static js_value mo_observe(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value opts = A(1), regs = js_get(ctx, t, "@regs"), reg;
  int fl = 0;
  unsigned i;
  if (!node_of(A(0)))
    return js_throw_type(ctx, "MutationObserver.observe: target must be a Node");
  if (opts.type != JV_OBJ)
    return js_throw_type(ctx, "MutationObserver.observe: options must be an object");
  if (mo_opt(ctx, opts, "childList"))
    fl |= MO_CHILDREN;
  if (mo_opt(ctx, opts, "attributes"))
    fl |= MO_ATTRS;
  if (mo_opt(ctx, opts, "characterData"))
    fl |= MO_DATA;
  if (mo_opt(ctx, opts, "subtree"))
    fl |= MO_SUBTREE;
  if (mo_opt(ctx, opts, "attributeOldValue"))
    fl |= MO_ATTR_OLD | MO_ATTRS;
  if (mo_opt(ctx, opts, "characterDataOldValue"))
    fl |= MO_DATA_OLD | MO_DATA;
  if (js_get(ctx, opts, "attributeFilter").type != JV_UNDEF)
    fl |= MO_ATTRS;
  if (!(fl & (MO_CHILDREN | MO_ATTRS | MO_DATA)))
    return js_throw_type(ctx, "MutationObserver.observe: set childList, attributes or characterData");
  if (!js_is_array(regs) || t.type != JV_OBJ)
    return js_undef();
  gc_push_scope();
  gc_keep(regs.u.obj);
  reg = js_obj_value((js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)));
  js_def(reg.u.obj, "node", A(0), 0);
  js_def(reg.u.obj, "flags", js_number(fl), 0);
  js_def(reg.u.obj, "filter", js_get(ctx, opts, "attributeFilter"), 0);
  for (i = 0; i < js_array_len(regs.u.obj); i++)
    if (js_get(ctx, js_get_index(ctx, regs, i), "node").u.obj == A(0).u.obj)
      break;
  if (i < js_array_len(regs.u.obj)) {
    js_set_index(ctx, regs, i, reg);
  } else {
    if (i == 0)
      js_array_push(ctx, g_mo_all, t);
    js_array_push(ctx, regs.u.obj, reg);
  }
  dom_on_mutate = mo_notify;
  gc_pop_scope();
  return js_undef();
}

static js_value mo_take_records(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value queue = js_get(ctx, t, "@queue");
  (void)args;
  (void)argc;
  if (!js_is_array(queue) || t.type != JV_OBJ)
    return js_obj_value(js_new_array(ctx, 0));
  js_def(t.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  return queue;
}

static js_value mo_disconnect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *kept;
  unsigned i;
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ || !g_mo_all)
    return js_undef();
  gc_push_scope();
  js_def(t.u.obj, "@regs", js_obj_value(js_new_array(ctx, 0)), 0);
  js_def(t.u.obj, "@queue", js_obj_value(js_new_array(ctx, 0)), 0);
  kept = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(g_mo_all); i++) {
    js_value o = js_get_index(ctx, js_obj_value(g_mo_all), i);
    if (o.u.obj != t.u.obj)
      js_array_push(ctx, kept, o);
  }
  g_mo_all = kept;
  gc_pop_scope();
  return js_undef();
}

/* ------------------------------------------------------- ResizeObserver --- *
 * Sizes are polled from the worker tick (js_resize_pump); an observer reports a target whose box differs from
 * the size it last reported (initially 0x0). @targets holds {node, w, h} entries. */

static js_object *g_ro_proto, *g_ro_all;

static js_value ro_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  if (!js_is_callable(A(0)))
    return js_throw_type(ctx, "ResizeObserver: callback must be a function");
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, g_ro_proto));
  js_def(o, "@cb", A(0), 0);
  js_def(o, "@targets", js_obj_value(js_new_array(ctx, 0)), 0);
  gc_pop_scope();
  return js_obj_value(o);
}

static int ro_index(js_ctx *ctx, js_value targets, js_value node)
{
  unsigned i;
  for (i = 0; i < js_array_len(targets.u.obj); i++)
    if (js_get(ctx, js_get_index(ctx, targets, i), "node").u.obj == node.u.obj)
      return (int)i;
  return -1;
}

static js_value ro_observe(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value targets = js_get(ctx, t, "@targets");
  js_object *entry;
  if (!node_of(A(0)))
    return js_throw_type(ctx, "ResizeObserver.observe: target must be an Element");
  if (!js_is_array(targets) || ro_index(ctx, targets, A(0)) >= 0)
    return js_undef();
  gc_push_scope();
  gc_keep(targets.u.obj);
  entry = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(entry, "node", A(0), 0);
  js_def(entry, "w", js_number(0), P_WRITABLE);
  js_def(entry, "h", js_number(0), P_WRITABLE);
  if (js_array_len(targets.u.obj) == 0)
    js_array_push(ctx, g_ro_all, t);
  js_array_push(ctx, targets.u.obj, js_obj_value(entry));
  gc_pop_scope();
  return js_undef();
}

static void ro_forget(js_ctx *ctx, js_value observer)
{
  js_object *kept;
  unsigned i;
  gc_push_scope();
  kept = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(g_ro_all); i++) {
    js_value o = js_get_index(ctx, js_obj_value(g_ro_all), i);
    if (o.u.obj != observer.u.obj)
      js_array_push(ctx, kept, o);
  }
  g_ro_all = kept;
  gc_pop_scope();
}

static js_value ro_unobserve(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value targets = js_get(ctx, t, "@targets");
  int at;
  js_object *kept;
  unsigned i;
  if (!js_is_array(targets) || A(0).type != JV_OBJ || (at = ro_index(ctx, targets, A(0))) < 0)
    return js_undef();
  gc_push_scope();
  kept = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (i = 0; i < js_array_len(targets.u.obj); i++)
    if ((int)i != at)
      js_array_push(ctx, kept, js_get_index(ctx, targets, i));
  js_def(t.u.obj, "@targets", js_obj_value(kept), 0);
  if (js_array_len(kept) == 0)
    ro_forget(ctx, t);
  gc_pop_scope();
  return js_undef();
}

static js_value ro_disconnect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type != JV_OBJ)
    return js_undef();
  gc_push_scope();
  js_def(t.u.obj, "@targets", js_obj_value(js_new_array(ctx, 0)), 0);
  ro_forget(ctx, t);
  gc_pop_scope();
  return js_undef();
}

static js_value ro_size_list(js_ctx *ctx, int w, int h)
{
  js_object *arr = (js_object *)gc_keep(js_new_array(ctx, 0)),
            *s = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(s, "inlineSize", js_number(w), P_ENUMERABLE);
  js_def(s, "blockSize", js_number(h), P_ENUMERABLE);
  js_array_push(ctx, arr, js_obj_value(s));
  return js_obj_value(arr);
}

static js_value ro_make_entry(js_ctx *ctx, js_value target, int w, int h)
{
  js_object *e = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto)),
            *r = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(r, "x", js_number(0), P_ENUMERABLE);
  js_def(r, "y", js_number(0), P_ENUMERABLE);
  js_def(r, "width", js_number(w), P_ENUMERABLE);
  js_def(r, "height", js_number(h), P_ENUMERABLE);
  js_def(r, "top", js_number(0), P_ENUMERABLE);
  js_def(r, "left", js_number(0), P_ENUMERABLE);
  js_def(r, "right", js_number(w), P_ENUMERABLE);
  js_def(r, "bottom", js_number(h), P_ENUMERABLE);
  js_def(e, "target", target, P_ENUMERABLE);
  js_def(e, "contentRect", js_obj_value(r), P_ENUMERABLE);
  js_def(e, "contentBoxSize", ro_size_list(ctx, w, h), P_ENUMERABLE);
  js_def(e, "borderBoxSize", ro_size_list(ctx, w, h), P_ENUMERABLE);
  js_def(e, "devicePixelContentBoxSize", ro_size_list(ctx, w, h), P_ENUMERABLE);
  return js_obj_value(e);
}

/* Deliver size changes to every ResizeObserver; returns the number of callbacks run */
int js_resize_pump(void)
{
  js_ctx *ctx = g_ctx;
  unsigned i, j;
  int fired = 0;
  if (!ctx || !g_page_live || !g_ro_all || js_array_len(g_ro_all) == 0)
    return 0;
  gc_push_scope();
  {
    js_object *observers = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (i = 0; i < js_array_len(g_ro_all); i++)
      js_array_push(ctx, observers, js_get_index(ctx, js_obj_value(g_ro_all), i));
    for (i = 0; i < js_array_len(observers); i++) {
      js_value ov = js_get_index(ctx, js_obj_value(observers), i), targets = js_get(ctx, ov, "@targets");
      js_object *entries;
      if (ov.type != JV_OBJ || !js_is_array(targets))
        continue;
      gc_push_scope();
      entries = (js_object *)gc_keep(js_new_array(ctx, 0));
      for (j = 0; j < js_array_len(targets.u.obj); j++) {
        js_value en = js_get_index(ctx, targets, j), node = js_get(ctx, en, "node");
        int x, y, w, h;
        if (!node_of(node) || !node_box(node, &x, &y, &w, &h))
          continue;
        if (w == (int)js_to_number(ctx, js_get(ctx, en, "w")) && h == (int)js_to_number(ctx, js_get(ctx, en, "h")))
          continue;
        js_set(ctx, en, "w", js_number(w));
        js_set(ctx, en, "h", js_number(h));
        js_array_push(ctx, entries, ro_make_entry(ctx, node, w, h));
      }
      if (js_array_len(entries) > 0) {
        js_value argv[2];
        argv[0] = js_obj_value(entries);
        argv[1] = ov;
        js_call(ctx, js_get(ctx, ov, "@cb"), ov, argv, 2);
        if (ctx->has_exc)
          report_exc_v(ctx);
        fired++;
      }
      gc_pop_scope();
    }
  }
  gc_pop_scope();
  return fired;
}

static js_object *observer_class(js_ctx *ctx, js_object *win, const char *name, js_native ctor, js_object **proto)
{
  js_object *c;
  *proto = js_new_object(ctx, ctx->object_proto);
  gc_add_root((void **)proto);
  gc_push_scope();
  c = (js_object *)gc_keep(js_new_native(ctx, ctor, name, 1));
  c->fn->data = js_obj_value(*proto);
  gc_write_barrier(c);
  js_def(c, "prototype", js_obj_value(*proto), 0);
  js_def(*proto, "constructor", js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, name, js_obj_value(c), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();
  return c;
}

void observers_install(js_ctx *ctx, js_object *win)
{
  dom_on_mutate = 0;
  observer_class(ctx, win, "MutationObserver", mo_ctor, &g_mo_proto);
  def_method(g_mo_proto, "observe", mo_observe, 2);
  def_method(g_mo_proto, "disconnect", mo_disconnect, 0);
  def_method(g_mo_proto, "takeRecords", mo_take_records, 0);
  g_mo_all = js_new_array(ctx, 0);
  g_mo_pending = js_new_array(ctx, 0);
  gc_add_root((void **)&g_mo_all);
  gc_add_root((void **)&g_mo_pending);
  observer_class(ctx, win, "ResizeObserver", ro_ctor, &g_ro_proto);
  def_method(g_ro_proto, "observe", ro_observe, 1);
  def_method(g_ro_proto, "unobserve", ro_unobserve, 1);
  def_method(g_ro_proto, "disconnect", ro_disconnect, 0);
  g_ro_all = js_new_array(ctx, 0);
  gc_add_root((void **)&g_ro_all);
}

void documentapi_teardown(void)
{
  dom_on_mutate = 0;
  gc_remove_root((void **)&g_walker_proto);
  gc_remove_root((void **)&g_iterator_proto);
  gc_remove_root((void **)&g_mo_proto);
  gc_remove_root((void **)&g_mo_all);
  gc_remove_root((void **)&g_mo_pending);
  gc_remove_root((void **)&g_ro_proto);
  gc_remove_root((void **)&g_ro_all);
}
