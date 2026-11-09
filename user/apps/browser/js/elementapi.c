/* Element natives: tree queries, mutation and attributes. */

#include "bindings.h"
#include "dom_query.h"

js_value el_get_by_tag(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *arr;
  char tag[32];
  lower_into(tag, sizeof tag, arg_str(ctx, A(0)));
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    js_dom_find_all_tag(n, tag, arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value el_query(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  return wrap_node(js_dom_find_selector(n, arg_str(ctx, A(0))));
}
js_value el_query_all(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *arr;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    js_dom_query_all(n, arg_str(ctx, A(0)), arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
static bool node_matches_sel(struct dom_node *n, const char *sel) { return js_dom_matches(n, sel); }
js_value el_matches(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  return js_bool(n && node_matches_sel(n, arg_str(ctx, A(0))));
}
js_value el_closest(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char sel[128];
  strlcpy(sel, arg_str(ctx, A(0)), sizeof sel);
  for (; n; n = n->parent)
    if (n->type == DOM_ELEMENT && node_matches_sel(n, sel))
      return wrap_node(n);
  return js_null();
}
js_value el_is_same_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  return js_bool(node_of(t) && node_of(t) == node_of(A(0)));
}
js_value el_contains(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *o = node_of(A(0));
  (void)ctx;
  for (; o; o = o->parent)
    if (o == n)
      return js_bool(1);
  return js_bool(0);
}

static struct dom_node *prev_sibling(struct dom_node *n)
{
  struct dom_node *c;
  if (!n || !n->parent)
    return 0;
  for (c = n->parent->first_child; c && c->next != n; c = c->next)
    ;
  return c;
}
static struct dom_node *first_elem(struct dom_node *c)
{
  while (c && c->type != DOM_ELEMENT)
    c = c->next;
  return c;
}
js_value el_child_nodes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c;
  js_object *arr;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    for (c = n->first_child; c; c = c->next)
      js_array_push(ctx, arr, wrap_node(c));
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value el_first_elem_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return n ? wrap_node(first_elem(n->first_child)) : js_null();
}
js_value el_last_elem_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c, *last = 0;
  (void)ctx;
  (void)args;
  (void)argc;
  for (c = n ? n->first_child : 0; c; c = c->next)
    if (c->type == DOM_ELEMENT)
      last = c;
  return wrap_node(last);
}
js_value el_child_count(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c;
  int k = 0;
  (void)ctx;
  (void)args;
  (void)argc;
  for (c = n ? n->first_child : 0; c; c = c->next)
    k += c->type == DOM_ELEMENT;
  return js_number(k);
}
js_value el_next_elem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return n ? wrap_node(first_elem(n->next)) : js_null();
}
js_value el_prev(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return wrap_node(prev_sibling(node_of(t)));
}
js_value el_prev_elem(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *p;
  (void)ctx;
  (void)args;
  (void)argc;
  for (p = prev_sibling(n); p && p->type != DOM_ELEMENT; p = prev_sibling(p))
    ;
  return wrap_node(p);
}
js_value el_has_children(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(n && n->first_child);
}
js_value el_is_connected(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(n && node_connected(n));
}
js_value el_owner_document(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_obj_value(g_document);
}
js_value el_root_node(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  while (n && n->parent)
    n = n->parent;
  return n == g_doc_root ? js_obj_value(g_document) : wrap_node(n);
}
js_value el_node_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v;
  (void)args;
  (void)argc;
  if (!n)
    return js_null();
  if (n->type == DOM_TEXT)
    return js_mk_cstring(ctx, n->text ? n->text : "");
  v = node_is_comment(n) ? dom_get_attr(n, "data") : 0;
  return v ? js_mk_cstring(ctx, v) : js_null();
}
js_value el_set_node_value(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n && n->type == DOM_TEXT)
    dom_set_text(n, arg_str(ctx, A(0)));
  else if (n && node_is_comment(n))
    dom_set_attr(n, "data", arg_str(ctx, A(0)));
  return js_undef();
}
js_value el_outer_html(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  sbuf sb = {0, 0, 0, 0};
  js_value r;
  (void)args;
  (void)argc;
  if (!n)
    return js_mk_cstring(ctx, "");
  serialize_node(n, &sb, 0);
  r = js_mk_string(ctx, sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}
js_value el_clone(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c;
  (void)ctx;
  if (!n)
    return js_null();
  c = dom_clone(n, js_to_boolean(A(0)));
  if (c && c->type == DOM_ELEMENT) {
    js_value cls = ce_lookup(c->tag);
    if (cls.type == JV_OBJ)
      ce_upgrade_node(c, cls);
    ce_upgrade_children(c);
  }
  return wrap_node(c);
}
/* An argument of before/after/append/...: a node, or a string turned into a text node */
static struct dom_node *arg_node(js_ctx *ctx, js_value v)
{
  struct dom_node *n = node_of(v);
  return n ? n : dom_new_text(arg_str(ctx, v));
}
js_value el_remove(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n && n->parent)
    remove_node(n->parent, n);
  return js_undef();
}
/* Insert every argument before `ref` in `parent` (ref NULL appends) */
static void insert_args(js_ctx *ctx, struct dom_node *parent, struct dom_node *ref, js_value *args, int argc)
{
  int i;
  for (i = 0; i < argc && parent && !ctx->has_exc; i++) {
    struct dom_node *c = arg_node(ctx, args[i]);
    if (c)
      insert_node(ctx, parent, c, ref);
  }
}
js_value el_append_multi(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  insert_args(ctx, node_of(t), 0, args, argc);
  return js_undef();
}
js_value el_replace_children(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  while (n && n->first_child)
    remove_node(n, n->first_child);
  insert_args(ctx, n, 0, args, argc);
  return js_undef();
}
js_value el_prepend(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  insert_args(ctx, n, n ? n->first_child : 0, args, argc);
  return js_undef();
}
js_value el_before(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n && n->parent)
    insert_args(ctx, n->parent, n, args, argc);
  return js_undef();
}
js_value el_after(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n && n->parent)
    insert_args(ctx, n->parent, n->next, args, argc);
  return js_undef();
}
js_value el_replace_with(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *parent = n ? n->parent : 0, *next = n ? n->next : 0;
  if (parent) {
    remove_node(parent, n);
    insert_args(ctx, parent, next && next->parent == parent ? next : 0, args, argc);
  }
  return js_undef();
}
js_value el_replace_child(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *nc = node_of(A(0)), *old = node_of(A(1));
  if (n && nc && old && old->parent == n && nc != old) {
    struct dom_node *next = old->next;
    remove_node(n, old);
    insert_node(ctx, n, nc, next);
  }
  return A(1);
}
js_value el_insert_adjacent_html(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *tree, *c, *ref, *parent;
  char pos[16];
  lower_into(pos, sizeof pos, arg_str(ctx, A(0)));
  if (!n)
    return js_undef();
  tree = dom_parse(arg_str(ctx, A(1)), (int)strlen(arg_str(ctx, A(1))));
  if (!tree)
    return js_undef();
  if (streq(pos, "beforebegin") || streq(pos, "afterend")) {
    parent = n->parent;
    ref = streq(pos, "beforebegin") ? n : n->next;
  } else {
    parent = n;
    ref = streq(pos, "afterbegin") ? n->first_child : 0;
  }
  c = tree->first_child;
  tree->first_child = tree->last_child = 0;
  while (c) {
    struct dom_node *nx = c->next;
    c->next = 0;
    c->parent = 0;
    if (parent &&
        (streq(pos, "beforebegin") || streq(pos, "afterbegin") || streq(pos, "beforeend") || streq(pos, "afterend")))
      insert_node(ctx, parent, c, ref);
    else
      dom_free(c);
    c = nx;
  }
  dom_free(tree);
  return js_undef();
}
js_value el_insert_adjacent_el(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *c = node_of(A(1));
  char pos[16];
  lower_into(pos, sizeof pos, arg_str(ctx, A(0)));
  if (!n || !c)
    return js_null();
  if (streq(pos, "beforebegin") && n->parent)
    insert_node(ctx, n->parent, c, n);
  else if (streq(pos, "afterend") && n->parent)
    insert_node(ctx, n->parent, c, n->next);
  else if (streq(pos, "afterbegin"))
    insert_node(ctx, n, c, n->first_child);
  else if (streq(pos, "beforeend"))
    insert_node(ctx, n, c, 0);
  else
    return js_null();
  return A(1);
}
js_value el_insert_adjacent_text(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value a2[2];
  struct dom_node *tx = dom_new_text(arg_str(ctx, A(1)));
  a2[0] = A(0);
  a2[1] = wrap_node(tx);
  if (tx)
    el_insert_adjacent_el(ctx, t, a2, 2);
  return js_undef();
}
void collect_class(struct dom_node *root, const char *classes, js_object *out)
{
  struct dom_node *n = root;
  while (n) {
    if (n != root && n->type == DOM_ELEMENT) {
      const char *cl = dom_get_attr(n, "class");
      bool all = *classes != 0;
      const char *p = classes;
      while (all && *p) {
        char one[64];
        int k = 0;
        while (*p == ' ')
          p++;
        while (*p && *p != ' ' && k < 63)
          one[k++] = *p++;
        one[k] = 0;
        while (*p && *p != ' ')
          p++;
        if (k && !(cl && tag_in_list(cl, one)))
          all = false;
      }
      if (all)
        js_array_push(g_ctx, out, wrap_node(n));
    }
    if (n->first_child) {
      n = n->first_child;
      continue;
    }
    while (n != root && !n->next)
      n = n->parent;
    if (n == root)
      break;
    n = n->next;
  }
}
js_value el_get_by_class(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *arr;
  struct dom_node *n = node_of(t);
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (n)
    collect_class(n, arg_str(ctx, A(0)), arr);
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value el_click(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n && !js_dispatch_dom_event(n, "click", 0, 0) && html_control_kind(n) == CTRL_FILE)
    browser_file_input_activate(n);
  return js_undef();
}
js_value el_attr_names(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  struct dom_attr *a;
  js_object *arr;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (a = n ? n->attrs : 0; a; a = a->next)
    js_array_push(ctx, arr, js_mk_cstring(ctx, dom_attr_qname(n, a)));
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value el_attributes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  struct dom_attr *a;
  js_object *arr;
  (void)args;
  (void)argc;
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (a = n ? n->attrs : 0; a; a = a->next) {
    js_object *o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(o, "name", js_mk_cstring(ctx, dom_attr_qname(n, a)), P_ENUMERABLE);
    js_def(o, "value", js_mk_cstring(ctx, a->value ? a->value : ""), P_ENUMERABLE | P_WRITABLE);
    js_def(o, "namespaceURI", dom_attr_ns(a)[0] ? js_mk_cstring(ctx, dom_attr_ns(a)) : js_null(), P_ENUMERABLE);
    js_def(o, "prefix",
           dom_prefix_len(a->name) ? js_mk_string(ctx, dom_attr_qname(n, a), dom_prefix_len(a->name)) : js_null(),
           P_ENUMERABLE);
    js_def(o, "localName", js_mk_cstring(ctx, dom_local_name(dom_attr_qname(n, a))), P_ENUMERABLE);
    js_array_push(ctx, arr, js_obj_value(o));
  }
  gc_pop_scope();
  return js_obj_value(arr);
}
js_value el_has_attrs(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(n && n->attrs);
}
js_value el_toggle_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *name = arg_str(ctx, A(0));
  bool has = n && dom_get_attr(n, name), want = argc > 1 ? js_to_boolean(A(1)) : !has;
  if (n && want && !has)
    dom_set_attr(n, name, "");
  else if (n && !want && has)
    dom_remove_attr(n, name);
  return js_bool(want);
}
struct dom_node *tree_next(struct dom_node *n)
{
  if (n->first_child)
    return n->first_child;
  while (n && !n->next)
    n = n->parent;
  return n ? n->next : 0;
}
js_value el_compare_position(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *a = node_of(t), *b = node_of(A(0)), *r, *n;
  (void)ctx;
  if (!a || !b || a == b)
    return js_number(0);
  if (dom_is_ancestor(a, b))
    return js_number(16 | 4); /* contained_by | following */
  if (dom_is_ancestor(b, a))
    return js_number(8 | 2); /* contains | preceding */
  for (r = a; r->parent; r = r->parent)
    ;
  if (!dom_is_ancestor(r, b))
    return js_number(1 | 32 | 4); /* disconnected | implementation_specific | following */
  for (n = r; n; n = tree_next(n)) {
    if (n == a)
      return js_number(4);
    if (n == b)
      return js_number(2);
  }
  return js_number(1);
}
