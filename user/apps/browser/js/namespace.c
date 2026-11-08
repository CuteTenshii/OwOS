/* DOM namespaces: namespaceURI/prefix/localName, the *NS element and attribute methods, lookups. */

#include "bindings.h"

static int is_el(const struct dom_node *n) { return n && n->type == DOM_ELEMENT && n->tag[0] != '#'; }
/* nullable string argument -> malloc'd copy ("" for null/undefined) */
static char *arg_dup(js_ctx *ctx, js_value v)
{
  return strdup(v.type == JV_NULL || v.type == JV_UNDEF ? "" : arg_str(ctx, v));
}
static js_value str_or_null(js_ctx *ctx, const char *s) { return s && *s ? js_mk_cstring(ctx, s) : js_null(); }

js_value el_namespace_uri(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)args;
  (void)argc;
  return is_el(n) ? str_or_null(ctx, dom_ns(n)) : js_null();
}
js_value el_prefix(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  int pl = is_el(n) ? dom_prefix_len(n->tag) : 0;
  (void)args;
  (void)argc;
  return pl ? js_mk_string(ctx, dom_qname(n), pl) : js_null();
}
js_value el_local_name(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)args;
  (void)argc;
  return is_el(n) ? js_mk_cstring(ctx, dom_local_name(dom_qname(n))) : js_null();
}

static js_value ns_error(js_ctx *ctx, const char *msg)
{
  return js_throw_error(ctx, ctx->error_proto, "NamespaceError", msg);
}
/* the qualified-name rules of createElementNS / setAttributeNS */
static const char *check_qname(const char *ns, const char *qname)
{
  int pl = dom_prefix_len(qname);
  if (pl && !*ns)
    return "a prefixed name needs a namespace";
  if (pl == 3 && !memcmp(qname, "xml", 3) && strcmp(ns, DOM_NS_XML))
    return "the xml prefix needs the XML namespace";
  return 0;
}

js_value doc_create_el_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char *ns = arg_dup(ctx, A(0)), *q;
  struct dom_node *n;
  const char *bad;
  js_value r;
  (void)t;
  if (!ns)
    return js_undef();
  q = strdup(arg_str(ctx, A(1)));
  bad = q ? check_qname(ns, q) : "out of memory";
  if (bad) {
    free(ns);
    free(q);
    return ns_error(ctx, bad);
  }
  n = dom_new_element(q);
  if (n) {
    dom_set_ns(n, ns);
    if (!strcmp(ns, DOM_NS_HTML)) {
      js_value cls = ce_lookup(n->tag);
      if (cls.type == JV_OBJ)
        ce_upgrade_node(n, cls);
    }
  }
  r = wrap_node(n);
  free(ns);
  free(q);
  return r;
}

js_value el_get_attr_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char *ns = arg_dup(ctx, A(0)), local[64];
  struct dom_attr *a;
  if (!ns)
    return js_undef();
  lower_into(local, sizeof local, arg_str(ctx, A(1)));
  a = is_el(n) ? dom_find_attr_ns(n, ns, local) : 0;
  free(ns);
  return a ? js_mk_cstring(ctx, a->value ? a->value : "") : js_null();
}
js_value el_has_attr_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value v = el_get_attr_ns(ctx, t, args, argc);
  return js_bool(v.type != JV_NULL);
}
js_value el_set_attr_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char *ns = arg_dup(ctx, A(0)), *q = 0, *v = 0;
  const char *bad;
  if (!ns)
    return js_undef();
  q = strdup(arg_str(ctx, A(1)));
  v = strdup(arg_str(ctx, A(2)));
  bad = q && v ? check_qname(ns, q) : "out of memory";
  if (!bad && is_el(n))
    dom_set_attr_ns(n, ns, q, v);
  free(ns);
  free(q);
  free(v);
  return bad ? ns_error(ctx, bad) : js_undef();
}
js_value el_rm_attr_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char *ns = arg_dup(ctx, A(0)), local[64];
  struct dom_attr *a;
  if (!ns)
    return js_undef();
  lower_into(local, sizeof local, arg_str(ctx, A(1)));
  a = is_el(n) ? dom_find_attr_ns(n, ns, local) : 0;
  if (a)
    dom_remove_attr(n, a->name);
  free(ns);
  return js_undef();
}

static void collect_ns(struct dom_node *root, int include_root, const char *ns, const char *local, js_object *out)
{
  struct dom_node *n = root;
  while (n) {
    if ((include_root || n != root) && is_el(n) && (!strcmp(local, "*") || !strcmp(dom_local_name(n->tag), local)) &&
        (!strcmp(ns, "*") || !strcmp(dom_ns(n), ns)))
      js_array_push(g_ctx, out, wrap_node(n));
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
static js_value by_tag_ns(js_ctx *ctx, struct dom_node *root, int include_root, js_value *args, int argc)
{
  char *ns = arg_dup(ctx, A(0)), local[64];
  js_object *arr;
  if (!ns)
    return js_undef();
  lower_into(local, sizeof local, arg_str(ctx, A(1)));
  gc_push_scope();
  arr = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (root)
    collect_ns(root, include_root, ns, local, arr);
  gc_pop_scope();
  free(ns);
  return js_obj_value(arr);
}
js_value el_by_tag_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return by_tag_ns(ctx, node_of(t), 0, args, argc);
}
js_value doc_by_tag_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return by_tag_ns(ctx, doc_root_of(t), 1, args, argc);
}

/* the element a namespace lookup starts from: a document uses its root element */
static const struct dom_node *lookup_start(js_value t)
{
  struct dom_node *n = node_of(t);
  if (!n && t.type == JV_OBJ)
    n = doc_root_of(t);
  if (n && n->type == DOM_ELEMENT && n->tag[0] == '#' && !node_is_comment(n))
    for (n = n->first_child; n && !is_el(n); n = n->next)
      ;
  else if (n && !is_el(n))
    n = n->parent;
  return n;
}
static const char *lookup_uri(const struct dom_node *n, const char *prefix)
{
  const char *u;
  if (prefix && !strcmp(prefix, "xml"))
    return DOM_NS_XML;
  if (prefix && !strcmp(prefix, "xmlns"))
    return DOM_NS_XMLNS;
  u = dom_scope_ns(n, prefix, prefix ? (int)strlen(prefix) : 0);
  return *u ? u : 0;
}
js_value node_lookup_ns_uri(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value p = A(0);
  const char *prefix = p.type == JV_NULL || p.type == JV_UNDEF || !*arg_str(ctx, p) ? 0 : arg_str(ctx, p);
  return str_or_null(ctx, lookup_uri(lookup_start(t), prefix));
}
js_value node_lookup_prefix(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *ns = A(0).type == JV_NULL || A(0).type == JV_UNDEF ? "" : arg_str(ctx, A(0));
  if (!*ns)
    return js_null();
  for (const struct dom_node *e = lookup_start(t); is_el(e); e = e->parent) {
    int pl = dom_prefix_len(e->tag);
    if (pl && !strcmp(dom_ns(e), ns))
      return js_mk_string(ctx, dom_qname(e), pl);
    for (const struct dom_attr *a = e->attrs; a; a = a->next)
      if (!memcmp(a->name, "xmlns:", 6) && a->value && !strcmp(a->value, ns))
        return js_mk_cstring(ctx, dom_attr_qname(e, a) + 6);
  }
  return js_null();
}
js_value node_is_default_ns(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *ns = A(0).type == JV_NULL || A(0).type == JV_UNDEF ? "" : arg_str(ctx, A(0));
  const char *def = lookup_uri(lookup_start(t), 0);
  return js_bool(!strcmp(def ? def : "", ns));
}

void namespace_install(void)
{
  def_acc(g_element_proto, "namespaceURI", el_namespace_uri, 0);
  def_acc(g_element_proto, "prefix", el_prefix, 0);
  def_acc(g_element_proto, "localName", el_local_name, 0);
  def_method(g_element_proto, "getAttributeNS", el_get_attr_ns, 2);
  def_method(g_element_proto, "setAttributeNS", el_set_attr_ns, 3);
  def_method(g_element_proto, "hasAttributeNS", el_has_attr_ns, 2);
  def_method(g_element_proto, "removeAttributeNS", el_rm_attr_ns, 2);
  def_method(g_element_proto, "getElementsByTagNameNS", el_by_tag_ns, 2);
  def_method(g_node_proto, "lookupNamespaceURI", node_lookup_ns_uri, 1);
  def_method(g_node_proto, "lookupPrefix", node_lookup_prefix, 1);
  def_method(g_node_proto, "isDefaultNamespace", node_is_default_ns, 1);
  def_method(g_document_proto, "createElementNS", doc_create_el_ns, 2);
  def_method(g_document_proto, "getElementsByTagNameNS", doc_by_tag_ns, 2);
}
