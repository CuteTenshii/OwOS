/* HTMLDialogElement and the popover API. */

#include "bindings.h"
#include "dom_query.h"

/* ------------------------------------------------------- dialog + popover - */

/* State lives in attributes: open, dialog-modal, dialog-rv (returnValue), popover-open. */

static bool fire_state_event(struct dom_node *n, const char *type, const char *old_state, const char *new_state)
{
  js_value tgt = wrap_node(n), ev;
  bool prevented = false;
  if (!g_page_live || tgt.type != JV_OBJ)
    return false;
  gc_push_scope();
  gc_keep(tgt.u.obj);
  ev = make_event(type, 0, 0, js_undef());
  if (ev.type == JV_OBJ) {
    gc_keep(ev.u.obj);
    if (new_state) {
      js_def(ev.u.obj, "oldState", js_mk_cstring(g_ctx, old_state), P_WRITABLE | P_ENUMERABLE);
      js_def(ev.u.obj, "newState", js_mk_cstring(g_ctx, new_state), P_WRITABLE | P_ENUMERABLE);
    }
    prevented = dispatch_from(tgt.u.obj, ev);
  }
  gc_pop_scope();
  return prevented;
}

static bool popover_is_auto(const struct dom_node *p)
{
  const char *v = dom_get_attr(p, "popover");
  return v && !streq_ci(v, "manual") && !streq_ci(v, "hint");
}

/* "auto" popovers that stay open only while the click lands inside them or `b` invoked them */
static bool pop_outside(const struct dom_node *p, const struct dom_node *a, const struct dom_node *b)
{
  return popover_is_auto(p) && p != b && !dom_is_ancestor(p, a);
}
static bool pop_inside(const struct dom_node *p, const struct dom_node *a, const struct dom_node *b)
{
  (void)b;
  return p != a && dom_is_ancestor(a, p);
}

static void popover_set(struct dom_node *n, bool open);

/* Hide every open popover for which `pick(p, a, b)` holds */
static void popover_sweep(bool (*pick)(const struct dom_node *, const struct dom_node *, const struct dom_node *),
                          const struct dom_node *a, const struct dom_node *b)
{
  struct dom_node *hit[32], *p;
  int nhit = 0;
  for (p = g_doc_root; p && nhit < 32; p = tree_next(p))
    if (p->type == DOM_ELEMENT && dom_get_attr(p, "popover-open") && pick(p, a, b))
      hit[nhit++] = p;
  for (int i = 0; i < nhit; i++)
    popover_set(hit[i], false);
}

static void popover_set(struct dom_node *n, bool open)
{
  if (!dom_get_attr(n, "popover") || (dom_get_attr(n, "popover-open") != 0) == open)
    return;
  if (fire_state_event(n, "beforetoggle", open ? "closed" : "open", open ? "open" : "closed") && open)
    return;
  if (open) {
    popover_sweep(pop_outside, n, n);
    dom_set_attr(n, "popover-open", "");
  } else {
    popover_sweep(pop_inside, n, 0);
    dom_remove_attr(n, "popover-open");
  }
  fire_state_event(n, "toggle", open ? "closed" : "open", open ? "open" : "closed");
}

js_value el_show_popover(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n)
    popover_set(n, true);
  return js_undef();
}
js_value el_hide_popover(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  if (n)
    popover_set(n, false);
  return js_undef();
}
js_value el_toggle_popover(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  bool want;
  if (!n || !dom_get_attr(n, "popover"))
    return js_bool(0);
  want = argc > 0 && A(0).type != JV_UNDEF ? js_to_boolean(A(0)) : !dom_get_attr(n, "popover-open");
  popover_set(n, want);
  return js_bool(dom_get_attr(n, "popover-open") != 0);
}
js_value el_get_popover(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *v = n ? dom_get_attr(n, "popover") : 0;
  (void)args;
  (void)argc;
  if (!v)
    return js_null();
  return js_mk_cstring(ctx, streq_ci(v, "manual") ? "manual" : streq_ci(v, "hint") ? "hint" : "auto");
}
js_value el_set_popover(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (!n)
    return js_undef();
  if (A(0).type == JV_NULL)
    dom_remove_attr(n, "popover");
  else
    dom_set_attr(n, "popover", arg_str(ctx, A(0)));
  return js_undef();
}

/* A click's default action: light-dismiss auto popovers, then run a popovertarget invoker */
static void popover_click(struct dom_node *t)
{
  struct dom_node *inv, *pop = 0;
  for (inv = t; inv; inv = inv->parent)
    if (inv->type == DOM_ELEMENT && (streq(inv->tag, "button") || streq(inv->tag, "input")) &&
        dom_get_attr(inv, "popovertarget"))
      break;
  if (inv && !dom_get_attr(inv, "disabled")) {
    pop = js_dom_find_id(g_doc_root, dom_get_attr(inv, "popovertarget"));
    if (pop && !dom_get_attr(pop, "popover"))
      pop = 0;
  }
  popover_sweep(pop_outside, t, pop);
  if (pop) {
    const char *act = dom_get_attr(inv, "popovertargetaction");
    bool is_open = dom_get_attr(pop, "popover-open") != 0;
    if (act && streq_ci(act, "show"))
      popover_set(pop, true);
    else if (act && streq_ci(act, "hide"))
      popover_set(pop, false);
    else
      popover_set(pop, !is_open);
  }
}

/* A click on a <details>'s first <summary> toggles it */
static void details_click(struct dom_node *t)
{
  struct dom_node *sum = t, *det, *c;
  while (sum && !(sum->type == DOM_ELEMENT && streq(sum->tag, "summary")))
    sum = sum->parent;
  det = sum ? sum->parent : 0;
  if (!det || !streq(det->tag, "details"))
    return;
  for (c = det->first_child; c; c = c->next)
    if (c->type == DOM_ELEMENT && streq(c->tag, "summary"))
      break;
  if (c != sum)
    return;
  if (dom_get_attr(det, "open"))
    dom_remove_attr(det, "open");
  else
    dom_set_attr(det, "open", "");
  fire_state_event(det, "toggle", 0, 0);
}

static bool is_dialog(const struct dom_node *n) { return n && streq(n->tag, "dialog"); }

static void dialog_close(struct dom_node *d, const char *ret)
{
  if (!dom_get_attr(d, "open"))
    return;
  dom_remove_attr(d, "open");
  dom_remove_attr(d, "dialog-modal");
  if (ret)
    dom_set_attr(d, "dialog-rv", ret);
  fire_state_event(d, "close", 0, 0);
}

/* <form method=dialog> submission closes the enclosing dialog; false if `form` isn't one */
bool js_dialog_form_submit(struct dom_node *form, struct dom_node *submitter)
{
  const char *m = dom_get_attr(form, "method");
  struct dom_node *d = form;
  if (!m || !streq_ci(m, "dialog"))
    return false;
  while (d && !is_dialog(d))
    d = d->parent;
  if (d)
    dialog_close(d, submitter && submitter != form ? dom_get_attr(submitter, "value") : 0);
  return true;
}

/* Escape: close the topmost open modal dialog (after a cancelable `cancel`) or auto popover */
bool js_escape_key(void)
{
  struct dom_node *top = 0;
  for (struct dom_node *p = g_doc_root; p; p = tree_next(p)) {
    if (p->type != DOM_ELEMENT)
      continue;
    if ((is_dialog(p) && dom_get_attr(p, "open") && dom_get_attr(p, "dialog-modal")) ||
        (dom_get_attr(p, "popover-open") && popover_is_auto(p)))
      top = p;
  }
  if (!top)
    return false;
  if (!is_dialog(top))
    popover_set(top, false);
  else if (!fire_state_event(top, "cancel", 0, 0))
    dialog_close(top, 0);
  return true;
}

static js_value dialog_show(js_value t, bool modal)
{
  struct dom_node *d = node_of(t);
  if (is_dialog(d) && !dom_get_attr(d, "open")) {
    dom_set_attr(d, "open", "");
    if (modal)
      dom_set_attr(d, "dialog-modal", "");
  }
  return js_undef();
}
js_value dlg_show(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return dialog_show(t, false);
}
js_value dlg_show_modal(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)args;
  (void)argc;
  return dialog_show(t, true);
}
js_value dlg_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  if (is_dialog(d))
    dialog_close(d, argc > 0 && A(0).type != JV_UNDEF ? arg_str(ctx, A(0)) : 0);
  return js_undef();
}
js_value dlg_request_close(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  if (is_dialog(d) && dom_get_attr(d, "open") && !fire_state_event(d, "cancel", 0, 0))
    dialog_close(d, argc > 0 && A(0).type != JV_UNDEF ? arg_str(ctx, A(0)) : 0);
  return js_undef();
}
js_value dlg_get_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  (void)ctx;
  (void)args;
  (void)argc;
  return js_bool(d && dom_get_attr(d, "open") != 0);
}
js_value dlg_set_open(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  (void)ctx;
  if (d && js_to_boolean(A(0)))
    dom_set_attr(d, "open", "");
  else if (d)
    dom_remove_attr(d, "open");
  return js_undef();
}
js_value dlg_get_rv(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  const char *v = d ? dom_get_attr(d, "dialog-rv") : 0;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, v ? v : "");
}
js_value dlg_set_rv(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *d = node_of(t);
  if (d)
    dom_set_attr(d, "dialog-rv", arg_str(ctx, A(0)));
  return js_undef();
}

/* C entry: the worker resolved a content click to `target`; fire `type` on it */
int js_dispatch_dom_event(struct dom_node *target, const char *type, int x, int y)
{
  js_value tgt, ev;
  bool prevented;
  if (!g_page_live || !target)
    return 0;
  gc_push_scope();
  tgt = wrap_node(target);
  if (tgt.type != JV_OBJ) {
    gc_pop_scope();
    return 0;
  }
  gc_keep(tgt.u.obj);
  ev = make_event(type, x, y, js_undef());
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  prevented = dispatch_from(tgt.u.obj, ev);
  if (!prevented && streq(type, "click")) {
    popover_click(target);
    details_click(target);
  }
  gc_pop_scope();
  return prevented;
}

int js_has_listeners(void) { return g_page_live && g_has_listeners; }
