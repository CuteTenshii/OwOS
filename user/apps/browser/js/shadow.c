/* Shadow DOM: attachShadow, ShadowRoot, <slot> and ElementInternals. */

#include "bindings.h"

js_object *g_shadowroot_proto;

static struct dom_node *element_of(js_value t)
{
  struct dom_node *n = node_of(t);
  return n && n->type == DOM_ELEMENT ? n : 0;
}

static bool can_attach_shadow(const char *tag)
{
  static const char *const ok[] = {"article", "aside", "blockquote", "body", "div",     "footer",
                                   "h1",      "h2",    "h3",         "h4",   "h5",      "h6",
                                   "header",  "main",  "nav",        "p",    "section", "span"};
  if (strchr(tag, '-'))
    return true;
  for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
    if (streq(tag, ok[i]))
      return true;
  return false;
}

static js_value el_attach_shadow(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *host = element_of(t);
  js_value mode;
  bool open;
  if (!host)
    return js_throw_type(ctx, "attachShadow: not an element");
  if (!can_attach_shadow(host->tag))
    return js_throw_error(ctx, ctx->error_proto, "NotSupportedError",
                          "attachShadow: this element cannot host a shadow root");
  mode = A(0).type == JV_OBJ ? js_get(ctx, A(0), "mode") : js_undef();
  if (mode.type != JV_STR || (!streq(mode.u.str->data, "open") && !streq(mode.u.str->data, "closed")))
    return js_throw_type(ctx, "attachShadow: mode must be \"open\" or \"closed\"");
  open = streq(mode.u.str->data, "open");
  if (host->shadow)
    return js_throw_error(ctx, ctx->error_proto, "NotSupportedError",
                          "attachShadow: the element already has a shadow root");
  if (!dom_new_shadow_root(host, open))
    return js_throw_range(ctx, "attachShadow: out of memory");
  return wrap_node(host->shadow);
}

static js_value el_shadow_root(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *host = element_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return host && host->shadow && host->shadow->shadow_open ? wrap_node(host->shadow) : js_null();
}

static js_value sr_host(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return root && root->host ? wrap_node(root->host) : js_null();
}
static js_value sr_mode(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *root = node_of(t);
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, root && root->shadow_open ? "open" : "closed");
}
static js_value sr_slot_assignment(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, "named");
}
static js_value sr_false(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(0);
}
static js_value sr_null(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_null();
}
static js_value sr_get_by_id(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return wrap_node(js_dom_find_id(node_of(t), arg_str(ctx, A(0))));
}

/* adoptedStyleSheets: kept as a plain array on the wrapper, read back by shadow_adopted_css() */
static js_value sr_get_adopted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value cur = t.type == JV_OBJ ? js_get(ctx, t, "@adopted") : js_undef();
  (void)args;
  (void)argc;
  if (js_is_array(cur))
    return cur;
  if (t.type == JV_OBJ) {
    gc_push_scope();
    cur = js_obj_value(js_new_array(ctx, 0));
    gc_keep(cur.u.obj);
    js_def(t.u.obj, "@adopted", cur, P_WRITABLE);
    gc_pop_scope();
  }
  return cur;
}
static js_value sr_set_adopted(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)argc;
  if (t.type == JV_OBJ && js_is_array(A(0))) {
    js_def(t.u.obj, "@adopted", A(0), P_WRITABLE);
    dom_mutation_seq++;
  }
  return js_undef();
}

/* Constructable stylesheets: the CSS text lives in "@css" and is read back when a tree's sheet is rebuilt */
static const char *sheet_text(js_ctx *ctx, js_value t)
{
  js_value v = t.type == JV_OBJ ? js_get(ctx, t, "@css") : js_undef();
  return v.type == JV_STR ? v.u.str->data : "";
}

/* Concatenated CSS of the sheets in `holder`'s adoptedStyleSheets, or NULL when there are none */
static char *adopted_css_of(js_value holder)
{
  js_value arr;
  sbuf sb = {0, 0, 0, 0};
  if (!g_ctx || holder.type != JV_OBJ || !holder.u.obj)
    return 0;
  arr = js_get(g_ctx, holder, "@adopted");
  if (!js_is_array(arr))
    return 0;
  for (unsigned i = 0; i < js_array_len(arr.u.obj); i++) {
    sb_puts(&sb, sheet_text(g_ctx, js_get_index(g_ctx, arr, i)));
    sb_putc(&sb, '\n');
  }
  return sb.b;
}
char *shadow_adopted_css(const struct dom_node *root)
{
  return root->wrapper ? adopted_css_of(js_obj_value((js_object *)root->wrapper)) : 0;
}
char *document_adopted_css(void) { return g_document ? adopted_css_of(js_obj_value(g_document)) : 0; }

static js_value cs_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value proto = ctx->native_data, sheet;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  sheet =
      js_obj_value((js_object *)gc_keep(js_new_object(ctx, proto.type == JV_OBJ ? proto.u.obj : ctx->object_proto)));
  js_def(sheet.u.obj, "@css", js_mk_cstring(ctx, ""), P_WRITABLE);
  gc_pop_scope();
  return sheet;
}
static js_value cs_replace_sync(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  if (t.type == JV_OBJ) {
    js_def(t.u.obj, "@css", js_mk_cstring(ctx, arg_str(ctx, A(0))), P_WRITABLE);
    dom_mutation_seq++;
  }
  return t;
}
static js_value cs_replace(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  cs_replace_sync(ctx, t, args, argc);
  return js_promise_resolved(ctx, t);
}
static js_value cs_insert_rule(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  sbuf sb = {0, 0, 0, 0};
  (void)argc;
  if (t.type != JV_OBJ)
    return js_number(0);
  sb_puts(&sb, sheet_text(ctx, t));
  sb_puts(&sb, arg_str(ctx, A(0)));
  sb_putc(&sb, '\n');
  js_def(t.u.obj, "@css", js_mk_cstring(ctx, sb.b ? sb.b : ""), P_WRITABLE);
  free(sb.b);
  dom_mutation_seq++;
  return js_number(0);
}
/* cssRules: one {cssText} entry per top-level rule of the sheet text */
static js_value cs_css_rules(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *s = sheet_text(ctx, t), *start = s;
  js_object *out;
  int depth = 0;
  char quote = 0;
  (void)args;
  (void)argc;
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  for (; *s; s++) {
    if (quote) {
      if (*s == '\\' && s[1])
        s++;
      else if (*s == quote)
        quote = 0;
      continue;
    }
    if (*s == '"' || *s == '\'')
      quote = *s;
    else if (*s == '{')
      depth++;
    else if ((*s == '}' && --depth == 0) || (*s == ';' && depth == 0)) {
      js_object *rule = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
      while (*start == ' ' || *start == '\n' || *start == '\t' || *start == '\r')
        start++;
      js_def(rule, "cssText", js_mk_string(ctx, start, (int)(s + 1 - start)), P_ENUMERABLE | P_WRITABLE);
      js_array_push(ctx, out, js_obj_value(rule));
      start = s + 1;
    }
  }
  gc_pop_scope();
  return js_obj_value(out);
}

/* ------------------------------------------------------------ slotchange ---- */

/* Fire slotchange on every slot queued by shadow_slot_changed(), once the current script step is done */
static js_value slotchange_flush(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value list = js_get(ctx, js_obj_value(g_shadowroot_proto), "@dirty");
  (void)thisv;
  (void)args;
  (void)argc;
  if (!js_is_array(list))
    return js_undef();
  gc_push_scope();
  gc_keep(list.u.obj);
  js_def(g_shadowroot_proto, "@dirty", js_obj_value(js_new_array(ctx, 0)), P_WRITABLE);
  for (unsigned i = 0; i < js_array_len(list.u.obj); i++) {
    js_value slot = js_get_index(ctx, list, i), ev = make_event("slotchange", 0, 0, js_undef());
    gc_keep(ev.u.obj);
    js_set(ctx, ev, "cancelable", js_bool(0));
    js_set(ctx, ev, "composed", js_bool(0));
    if (slot.type == JV_OBJ)
      dispatch_from(slot.u.obj, ev);
  }
  gc_pop_scope();
  return js_undef();
}

static void slot_dirty(const struct dom_node *slot)
{
  js_value list, set_timeout, fn;
  bool first;
  if (!g_ctx || !g_shadowroot_proto || !slot)
    return;
  gc_push_scope();
  list = js_get(g_ctx, js_obj_value(g_shadowroot_proto), "@dirty");
  if (!js_is_array(list)) {
    list = js_obj_value(js_new_array(g_ctx, 0));
    js_def(g_shadowroot_proto, "@dirty", list, P_WRITABLE);
  }
  gc_keep(list.u.obj);
  first = js_array_len(list.u.obj) == 0;
  {
    js_value w = wrap_node((struct dom_node *)slot);
    for (unsigned i = 0; i < js_array_len(list.u.obj); i++)
      if (js_get_index(g_ctx, list, i).u.obj == w.u.obj) {
        gc_pop_scope();
        return;
      }
    js_array_push(g_ctx, list.u.obj, w);
  }
  set_timeout = js_get(g_ctx, js_obj_value(g_ctx->global), "setTimeout");
  if (first && js_is_callable(set_timeout)) { /* promise jobs run synchronously here, so defer with a timer */
    js_value argv[2];
    argv[0] = js_obj_value((js_object *)gc_keep(js_new_native(g_ctx, slotchange_flush, "slotchange", 0)));
    argv[1] = js_number(0);
    fn = js_call(g_ctx, set_timeout, js_obj_value(g_ctx->global), argv, 2);
    (void)fn;
  }
  gc_pop_scope();
}

/* Queue slotchange for every slot in `n`'s subtree that has assigned nodes (a slot gained by the tree) */
static void slots_gained(const struct dom_node *n, int depth)
{
  if (n->type != DOM_ELEMENT || depth > 64)
    return;
  if (streq(n->tag, "slot")) {
    const struct dom_node *host = dom_slot_host(n);
    for (const struct dom_node *c = host ? host->first_child : 0; c; c = c->next)
      if (dom_slot_accepts(n, c)) {
        slot_dirty(n);
        break;
      }
  }
  for (const struct dom_node *c = n->first_child; c; c = c->next)
    slots_gained(c, depth + 1);
}

/* Called after `c` was inserted: its slot (as a light child) or the slots it brings (in a shadow tree) changed */
void shadow_node_inserted(struct dom_node *c)
{
  if (c->parent && c->parent->shadow)
    slot_dirty(dom_assigned_slot(c));
  if (c->type == DOM_ELEMENT && c->parent)
    slots_gained(c, 0);
}
/* Called just before `c` is detached from its parent */
void shadow_node_removing(struct dom_node *c)
{
  if (c->parent && c->parent->shadow)
    slot_dirty(dom_assigned_slot(c));
}

/* ------------------------------------------------------------- <slot> ---- */

static js_value slot_get_name(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t);
  const char *v = n ? dom_get_attr(n, "name") : 0;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value slot_set_name(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t);
  (void)argc;
  if (n) {
    dom_set_attr(n, "name", arg_str(ctx, A(0)));
    slots_gained(n, 0);
  }
  return js_undef();
}

/* Push the nodes assigned to `slot` (its fallback content when none), expanding nested slots when `flatten` */
static void collect_assigned(js_ctx *ctx, const struct dom_node *slot, bool elements_only, bool flatten, js_object *out,
                             int depth)
{
  const struct dom_node *host = dom_slot_host(slot);
  bool any = false;
  if (!host || depth > 16)
    return;
  for (struct dom_node *c = host->first_child; c; c = c->next) {
    if (!dom_slot_accepts(slot, c))
      continue;
    any = true;
    if (flatten && c->type == DOM_ELEMENT && streq(c->tag, "slot")) {
      collect_assigned(ctx, c, elements_only, true, out, depth + 1);
      continue;
    }
    if (!elements_only || c->type == DOM_ELEMENT)
      js_array_push(ctx, out, wrap_node(c));
  }
  if (!any && flatten)
    for (struct dom_node *c = slot->first_child; c; c = c->next)
      if (!elements_only || c->type == DOM_ELEMENT)
        js_array_push(ctx, out, wrap_node(c));
}
static js_value slot_assigned(js_ctx *ctx, js_value t, js_value *args, int argc, bool elements_only)
{
  struct dom_node *slot = element_of(t);
  js_object *out;
  bool flatten = A(0).type == JV_OBJ && js_to_boolean(js_get(ctx, A(0), "flatten"));
  gc_push_scope();
  out = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (slot)
    collect_assigned(ctx, slot, elements_only, flatten, out, 0);
  gc_pop_scope();
  return js_obj_value(out);
}
static js_value slot_assigned_nodes(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return slot_assigned(ctx, t, args, argc, false);
}
static js_value slot_assigned_elements(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return slot_assigned(ctx, t, args, argc, true);
}

static js_value el_get_slot_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t);
  const char *v = n ? dom_get_attr(n, "slot") : 0;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, v ? v : "");
}
static js_value el_set_slot_attr(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t);
  (void)argc;
  if (n) {
    shadow_node_removing(n);
    dom_set_attr(n, "slot", arg_str(ctx, A(0)));
    shadow_node_inserted(n);
  }
  return js_undef();
}
static js_value el_assigned_slot(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t), *slot;
  (void)ctx;
  (void)args;
  (void)argc;
  if (!n || !n->parent || !n->parent->shadow || !n->parent->shadow->shadow_open)
    return js_null();
  slot = dom_assigned_slot(n);
  return slot ? wrap_node(slot) : js_null();
}

/* template.content: a fragment that takes over the template's children, now and whenever it gains new ones */
static js_value tpl_content(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t), *frag;
  js_value fv;
  (void)args;
  (void)argc;
  if (!n || !streq(n->tag, "template"))
    return js_undef();
  fv = js_get(ctx, t, "@content");
  if (fv.type != JV_OBJ) {
    frag = dom_new_element("#frag");
    if (!frag)
      return js_throw_range(ctx, "template.content: out of memory");
    fv = wrap_node(frag);
    js_def(t.u.obj, "@content", fv, P_WRITABLE);
  }
  frag = node_of(fv);
  while (frag && n->first_child) {
    struct dom_node *k = n->first_child;
    dom_remove_child(n, k);
    dom_append_child(frag, k);
  }
  return fv;
}

/* --------------------------------------------------- ElementInternals ---- */

static js_value ei_noop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
static js_value ei_true(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(1);
}

static js_value el_attach_internals(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = element_of(t);
  js_object *ei, *validity;
  js_value set_ctor;
  (void)args;
  (void)argc;
  if (!n || !strchr(n->tag, '-'))
    return js_throw_error(ctx, ctx->error_proto, "NotSupportedError", "attachInternals: not a custom element");
  if (js_has_own(t.u.obj, "@internals"))
    return js_throw_error(ctx, ctx->error_proto, "NotSupportedError", "attachInternals: already attached");
  gc_push_scope();
  ei = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  validity = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def(validity, "valid", js_bool(1), P_ENUMERABLE | P_WRITABLE);
  js_def(t.u.obj, "@internals", js_obj_value(ei), 0);
  js_def(ei, "shadowRoot", n->shadow ? wrap_node(n->shadow) : js_null(), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "form", js_null(), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "role", js_null(), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "labels", js_obj_value(js_new_array(ctx, 0)), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "validity", js_obj_value(validity), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "validationMessage", js_mk_cstring(ctx, ""), P_ENUMERABLE | P_WRITABLE);
  js_def(ei, "willValidate", js_bool(1), P_ENUMERABLE | P_WRITABLE);
  set_ctor = js_get(ctx, js_obj_value(ctx->global), "Set");
  if (js_is_callable(set_ctor))
    js_def(ei, "states", js_construct(ctx, set_ctor, 0, 0), P_ENUMERABLE | P_WRITABLE);
  js_def_native(ctx, ei, "setFormValue", ei_noop, 2);
  js_def_native(ctx, ei, "setValidity", ei_noop, 3);
  js_def_native(ctx, ei, "checkValidity", ei_true, 0);
  js_def_native(ctx, ei, "reportValidity", ei_true, 0);
  gc_pop_scope();
  return js_obj_value(ei);
}

/* ------------------------------------------------------------ install ---- */

static js_value sr_illegal_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

static js_object *shadow_slot_proto(void)
{
  for (unsigned i = 0; i < g_nhtml_ifaces; i++)
    if (streq(g_html_ifaces[i].tags, "slot"))
      return g_html_ifaces[i].proto;
  return 0;
}

void shadow_install(js_ctx *ctx, js_object *win)
{
  js_object *sc, *slot_proto = shadow_slot_proto();

  def_method(g_element_proto, "attachShadow", el_attach_shadow, 1);
  def_method(g_element_proto, "attachInternals", el_attach_internals, 0);
  def_acc(g_element_proto, "shadowRoot", el_shadow_root, 0);
  def_acc(g_element_proto, "slot", el_get_slot_attr, el_set_slot_attr);
  def_acc(g_element_proto, "assignedSlot", el_assigned_slot, 0);

  g_shadowroot_proto = js_new_object(ctx, g_html_proto);
  gc_add_root((void **)&g_shadowroot_proto);
  def_acc(g_shadowroot_proto, "host", sr_host, 0);
  def_acc(g_shadowroot_proto, "mode", sr_mode, 0);
  def_acc(g_shadowroot_proto, "slotAssignment", sr_slot_assignment, 0);
  def_acc(g_shadowroot_proto, "delegatesFocus", sr_false, 0);
  def_acc(g_shadowroot_proto, "clonable", sr_false, 0);
  def_acc(g_shadowroot_proto, "serializable", sr_false, 0);
  def_acc(g_shadowroot_proto, "activeElement", sr_null, 0);
  def_acc(g_shadowroot_proto, "adoptedStyleSheets", sr_get_adopted, sr_set_adopted);
  def_method(g_shadowroot_proto, "getElementById", sr_get_by_id, 1);
  sc = js_new_native(ctx, sr_illegal_ctor, "ShadowRoot", 0);
  gc_push_scope();
  gc_keep(sc);
  js_def(sc, "prototype", js_obj_value(g_shadowroot_proto), 0);
  js_def(g_shadowroot_proto, "constructor", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
  js_def(win, "ShadowRoot", js_obj_value(sc), P_WRITABLE | P_CONFIGURABLE);
  gc_pop_scope();

  {
    js_object *proto = js_new_object(ctx, ctx->object_proto), *ctor;
    gc_push_scope();
    gc_keep(proto);
    def_method(proto, "replaceSync", cs_replace_sync, 1);
    def_method(proto, "replace", cs_replace, 1);
    def_method(proto, "insertRule", cs_insert_rule, 2);
    def_method(proto, "deleteRule", ei_noop, 1);
    def_acc(proto, "cssRules", cs_css_rules, 0);
    def_acc(proto, "rules", cs_css_rules, 0);
    ctor = (js_object *)gc_keep(js_new_native(ctx, cs_ctor, "CSSStyleSheet", 0));
    ctor->fn->data = js_obj_value(proto);
    js_def(ctor, "prototype", js_obj_value(proto), 0);
    js_def(proto, "constructor", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
    js_def(win, "CSSStyleSheet", js_obj_value(ctor), P_WRITABLE | P_CONFIGURABLE);
    gc_pop_scope();
  }
  def_acc(g_document_proto, "adoptedStyleSheets", sr_get_adopted, sr_set_adopted);

  for (unsigned i = 0; i < g_nhtml_ifaces; i++)
    if (streq(g_html_ifaces[i].tags, "template") && g_html_ifaces[i].proto)
      def_acc(g_html_ifaces[i].proto, "content", tpl_content, 0);

  if (slot_proto) {
    def_acc(slot_proto, "name", slot_get_name, slot_set_name);
    def_method(slot_proto, "assignedNodes", slot_assigned_nodes, 0);
    def_method(slot_proto, "assignedElements", slot_assigned_elements, 0);
  }
}
