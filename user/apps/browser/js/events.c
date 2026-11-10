/* DOM events. */

#include "bindings.h"

/* ------------------------------------------------------- DOM events -------- */

/* The own "@ev" listener array of `target`, created on demand when `create`. */
static js_object *listeners_of(js_object *target, int create)
{
  pentry *pe = pmap_find_pub(target, "@ev");
  if (pe && pe->value.type == JV_OBJ)
    return pe->value.u.obj;
  if (!create)
    return 0;
  {
    js_object *arr;
    gc_push_scope();
    arr = (js_object *)gc_keep(js_new_array(g_ctx, 0));
    js_def(target, "@ev", js_obj_value(arr), 0);
    gc_pop_scope();
    return arr;
  }
}

int ev_flag(js_value ev, const char *k) { return js_to_boolean(js_get(g_ctx, ev, k)); }

static void remove_listener(js_ctx *ctx, js_object *obj, const char *type, js_value fn, bool capture);

/* Invoke the listeners on `obj` whose capture flag equals `want_capture` and whose type matches the */
void ev_fire_on(js_object *obj, js_value ev, int want_capture)
{
  js_object *list = listeners_of(obj, 0);
  js_value evty;
  unsigned i, n;
  gc_push_scope();
  if (list)
    gc_keep(list); /* a handler may replace @ev; pin this copy */
  js_set(g_ctx, ev, "currentTarget", js_obj_value(obj));
  evty = js_get(g_ctx, ev, "type");
  n = list ? js_array_len(list) : 0;
  for (i = 0; i < n && !ev_flag(ev, "@sip"); i++) {
    js_value e = js_get_index(g_ctx, js_obj_value(list), i);
    js_value ty, fn, a, src, sig, hf;
    if (e.type != JV_OBJ)
      continue;
    if (js_to_boolean(js_get(g_ctx, e, "capture")) != want_capture)
      continue;
    ty = js_get(g_ctx, e, "type");
    if (!(ty.type == JV_STR && evty.type == JV_STR && streq(ty.u.str->data, evty.u.str->data)))
      continue;
    sig = js_get(g_ctx, e, "signal");
    if (sig.type == JV_OBJ && js_to_boolean(js_get(g_ctx, sig, "aborted")))
      continue;
    fn = js_get(g_ctx, e, "fn");
    hf = js_is_callable(fn) ? fn : js_get(g_ctx, fn, "handleEvent");
    if (!js_is_callable(hf))
      continue;
    if (js_to_boolean(js_get(g_ctx, e, "once")))
      remove_listener(g_ctx, obj, ty.u.str->data, fn, want_capture);
    a = ev;
    src = js_get(g_ctx, e, "src");
    js_set_event_script(evty.type == JV_STR ? evty.u.str->data : 0, src.type == JV_STR ? src.u.str->data : 0);
    js_call(g_ctx, hf, js_is_callable(fn) ? js_obj_value(obj) : fn, &a, 1);
    if (g_ctx->has_exc)
      report_exc_v(g_ctx);
  }
  /* on<type> property handler (onload/onclick/onchange/…): a plain callable property */
  if (!want_capture && evty.type == JV_STR && evty.u.str->len > 0 && evty.u.str->len < 40) {
    char onname[48];
    int k = 0;
    const char *ty = evty.u.str->data;
    onname[k++] = 'o';
    onname[k++] = 'n';
    while (*ty && k < 47)
      onname[k++] = *ty++;
    onname[k] = 0;
    js_value h = js_get(g_ctx, js_obj_value(obj), onname);
    if (js_is_callable(h)) {
      js_value a = ev, r;
      js_set_event_script(evty.u.str->data, 0);
      r = js_call(g_ctx, h, js_obj_value(obj), &a, 1);
      if (g_ctx->has_exc)
        report_exc_v(g_ctx);
      else if (streq(evty.u.str->data, "beforeunload") && r.type != JV_UNDEF && r.type != JV_NULL)
        js_set(g_ctx, ev, "@dp", js_bool(1));
    }
  }
  gc_pop_scope();
}

/* The node `target` appears as to a listener on `cur` (NULL = window): shadow trees `cur` is outside of are hidden */
static js_object *retarget(struct dom_node *target, struct dom_node *cur, js_object *startobj)
{
  struct dom_node *a = target;
  for (;;) {
    struct dom_node *r = a;
    bool inside = false;
    while (r->parent)
      r = r->parent;
    if (!r->host)
      break;
    for (const struct dom_node *m = cur; m && !inside; m = dom_flat_parent(m))
      inside = m == r;
    if (inside)
      break;
    a = r->host;
  }
  return a == target ? startobj : wrap_node(a).u.obj;
}

static void fire_at(js_object *obj, struct dom_node *cur, struct dom_node *target, js_object *startobj, js_value ev,
                    int capture)
{
  if (target)
    js_set(g_ctx, ev, "target", js_obj_value(retarget(target, cur, startobj)));
  ev_fire_on(obj, ev, capture);
}

/* Dispatch `ev` starting at wrapper `startobj` (its host node anchors the path) */
bool dispatch_from(js_object *startobj, js_value ev)
{
  js_object *path[128];
  struct dom_node *pnode[128];
  int np = 0, i;
  struct dom_node *target = (struct dom_node *)startobj->host;
  bool composed = ev_flag(ev, "composed");
  js_object *parr;
  gc_push_scope();
  gc_keep(startobj);
  /* hold ev across the calls below */
  if (ev.type == JV_OBJ)
    gc_keep(ev.u.obj);
  js_set(g_ctx, ev, "target", js_obj_value(startobj));

  if (target) {
    for (struct dom_node *n = target; n && np < 126; n = n->parent ? n->parent : (composed ? n->host : 0))
      if (n->wrapper) {
        pnode[np] = n;
        path[np++] = (js_object *)n->wrapper;
      }
  } else {
    pnode[np] = 0;
    path[np++] = startobj; /* window/detached: just itself */
  }
  /* window (global) is the outermost target, after the document */
  if (target && g_ctx->global && np < 127 && path[np ? np - 1 : 0] != g_ctx->global) {
    pnode[np] = 0;
    path[np++] = g_ctx->global;
  }

  parr = (js_object *)gc_keep(js_new_array(g_ctx, 0));
  for (i = 0; i < np; i++)
    js_array_push(g_ctx, parr, js_obj_value(path[i]));
  js_def(ev.u.obj, "@path", js_obj_value(parr), P_WRITABLE);

  for (i = np - 1; i >= 1 && !ev_flag(ev, "@sp"); i--)
    fire_at(path[i], pnode[i], target, startobj, ev, 1); /* capture */
  if (!ev_flag(ev, "@sp")) {
    fire_at(path[0], pnode[0], target, startobj, ev, 1);
    fire_at(path[0], pnode[0], target, startobj, ev, 0);
  } /* target */
  for (i = 1; i < np && !ev_flag(ev, "@sp"); i++)
    fire_at(path[i], pnode[i], target, startobj, ev, 0); /* bubble */

  if (target)
    js_set(g_ctx, ev, "target", js_obj_value(retarget(target, 0, startobj)));
  js_def(ev.u.obj, "@path", js_obj_value(js_new_array(g_ctx, 0)), P_WRITABLE);
  gc_pop_scope();
  return ev_flag(ev, "@dp");
}

js_value ev_composed_path(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value p = t.type == JV_OBJ ? js_get(ctx, t, "@path") : js_undef();
  js_object *copy;
  (void)args;
  (void)argc;
  gc_push_scope();
  copy = (js_object *)gc_keep(js_new_array(ctx, 0));
  if (js_is_array(p))
    for (unsigned i = 0; i < js_array_len(p.u.obj); i++)
      js_array_push(ctx, copy, js_get_index(ctx, p, i));
  gc_pop_scope();
  return js_obj_value(copy);
}

js_value ev_prevent(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ) {
    js_set(ctx, t, "@dp", js_bool(1));
    js_set(ctx, t, "defaultPrevented", js_bool(1));
  }
  return js_undef();
}
js_value ev_stop_immediate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ) {
    js_set(ctx, t, "@sp", js_bool(1));
    js_set(ctx, t, "@sip", js_bool(1));
  }
  return js_undef();
}
js_value ev_stop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)args;
  (void)argc;
  if (t.type == JV_OBJ)
    js_set(ctx, t, "@sp", js_bool(1));
  return js_undef();
}

/* Build an Event object on g_event_proto */
js_value make_event(const char *type, int x, int y, js_value init)
{
  js_object *e;
  bool bubbles = true, cancelable = true;
  js_value detail = js_undef();
  if (init.type == JV_OBJ) {
    js_value b = js_get(g_ctx, init, "bubbles");
    js_value c = js_get(g_ctx, init, "cancelable");
    if (b.type != JV_UNDEF)
      bubbles = js_to_boolean(b);
    if (c.type != JV_UNDEF)
      cancelable = js_to_boolean(c);
    detail = js_get(g_ctx, init, "detail");
  }
  gc_push_scope();
  if (init.type == JV_OBJ)
    gc_keep(init.u.obj);
  e = (js_object *)gc_keep(js_new_object(g_ctx, g_event_proto));
  js_def(e, "type", js_mk_cstring(g_ctx, type), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "bubbles", js_bool(bubbles), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "cancelable", js_bool(cancelable), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "defaultPrevented", js_bool(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "composed", js_bool(1), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "target", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "currentTarget", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "clientX", js_number(x), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "clientY", js_number(y), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "detail", detail, P_WRITABLE | P_ENUMERABLE);
  js_def(e, "eventPhase", js_number(0), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "timeStamp", js_number(perf_now_ms()), P_WRITABLE | P_ENUMERABLE);
  js_def(e, "@dp", js_bool(0), P_WRITABLE);
  js_def(e, "@sp", js_bool(0), P_WRITABLE);
  js_def(e, "@sip", js_bool(0), P_WRITABLE);
  gc_pop_scope();
  return js_obj_value(e);
}

js_value el_add_listener(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char type[64];
  js_value fn = A(1), opts = argc >= 3 ? A(2) : js_undef();
  bool capture = false, once = false;
  js_object *list;
  unsigned i, n;
  if (t.type != JV_OBJ || fn.type != JV_OBJ || !(js_is_callable(fn) || js_is_callable(js_get(ctx, fn, "handleEvent"))))
    return js_undef();
  strlcpy(type, arg_str(ctx, A(0)), sizeof type);
  if (opts.type == JV_BOOL) {
    capture = opts.u.boolean;
  } else if (opts.type == JV_OBJ) {
    js_value sig = js_get(ctx, opts, "signal");
    capture = js_to_boolean(js_get(ctx, opts, "capture"));
    once = js_to_boolean(js_get(ctx, opts, "once"));
    if (sig.type == JV_OBJ && js_to_boolean(js_get(ctx, sig, "aborted")))
      return js_undef();
  }
  gc_push_scope();
  gc_keep(t.u.obj);
  gc_keep(fn.u.obj);
  list = listeners_of(t.u.obj, 1);
  gc_keep(list);
  n = js_array_len(list);
  for (i = 0; i < n; i++) { /* the same type, callback and capture is added once */
    js_value e = js_get_index(ctx, js_obj_value(list), i), ty = js_get(ctx, e, "type");
    if (ty.type == JV_STR && streq(ty.u.str->data, type) && js_strict_eq(js_get(ctx, e, "fn"), fn) &&
        js_to_boolean(js_get(ctx, e, "capture")) == capture) {
      gc_pop_scope();
      return js_undef();
    }
  }
  {
    js_object *e = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
    js_def(e, "type", js_mk_cstring(ctx, type), 0);
    js_def(e, "fn", fn, 0);
    js_def(e, "capture", js_bool(capture), 0);
    js_def(e, "once", js_bool(once), 0);
    if (opts.type == JV_OBJ)
      js_def(e, "signal", js_get(ctx, opts, "signal"), 0);
    js_def(e, "src", js_mk_cstring(ctx, js_cur_script()), 0);
    js_array_push(ctx, list, js_obj_value(e));
  }
  gc_pop_scope();
  g_has_listeners = true;
  return js_undef();
}
static void remove_listener(js_ctx *ctx, js_object *obj, const char *type, js_value fn, bool capture)
{
  js_object *list = listeners_of(obj, 0), *nl;
  unsigned i, n;
  if (!list)
    return;
  gc_push_scope();
  gc_keep(obj);
  gc_keep(list);
  if (fn.type == JV_OBJ)
    gc_keep(fn.u.obj);
  nl = (js_object *)gc_keep(js_new_array(ctx, 0));
  n = js_array_len(list);
  for (i = 0; i < n; i++) {
    js_value e = js_get_index(ctx, js_obj_value(list), i);
    js_value ty = js_get(ctx, e, "type"), ef = js_get(ctx, e, "fn");
    bool ec = js_to_boolean(js_get(ctx, e, "capture"));
    bool match = ty.type == JV_STR && streq(ty.u.str->data, type) && js_strict_eq(ef, fn) && ec == capture;
    if (!match)
      js_array_push(ctx, nl, e);
  }
  js_def(obj, "@ev", js_obj_value(nl), 0);
  gc_pop_scope();
}
js_value el_remove_listener(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  char type[64];
  bool capture = false;
  if (t.type != JV_OBJ)
    return js_undef();
  strlcpy(type, arg_str(ctx, A(0)), sizeof type);
  if (argc >= 3) {
    js_value o = A(2);
    if (o.type == JV_BOOL)
      capture = o.u.boolean;
    else if (o.type == JV_OBJ)
      capture = js_to_boolean(js_get(ctx, o, "capture"));
  }
  remove_listener(ctx, t.u.obj, type, A(1), capture);
  return js_undef();
}
js_value el_dispatch(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value ev = A(0);
  (void)argc;
  if (t.type != JV_OBJ || ev.type != JV_OBJ)
    return js_bool(1);
  return js_bool(!dispatch_from(t.u.obj, ev));
}
js_value event_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value ev;
  (void)thisv;
  if (argc < 1)
    return js_throw_type(ctx, "Failed to construct 'Event': 1 argument required, but only 0 present.");
  ev = make_event(arg_str(ctx, A(0)), 0, 0, A(1));
  js_set(ctx, ev, "composed", js_bool(A(1).type == JV_OBJ && js_to_boolean(js_get(ctx, A(1), "composed"))));
  return ev;
}

/* Re-parent an event onto a window interface's prototype (ErrorEvent, MessageEvent) if the page has it */
void event_adopt_iface(js_value ev, const char *iface)
{
  js_value ctor = js_get(g_ctx, js_obj_value(g_ctx->global), iface), proto;
  if (ev.type != JV_OBJ || ctor.type != JV_OBJ)
    return;
  proto = js_get(g_ctx, ctor, "prototype");
  if (proto.type == JV_OBJ)
    ev.u.obj->proto = proto.u.obj;
}

/* EventTarget(): `new` builds a bare target; `super()` from a subclass keeps its own `this` */
js_value eventtarget_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value proto = ctx->native_data;
  (void)args;
  (void)argc;
  if (thisv.type == JV_OBJ)
    return thisv;
  return js_obj_value(js_new_object(ctx, proto.type == JV_OBJ ? proto.u.obj : ctx->object_proto));
}

/* The init-dict fields carried by the Event subclasses */
static const char *const k_event_init_keys[] = {
    "view",          "detail",    "clientX",  "clientY",   "screenX",   "screenY",     "pageX",    "pageY",
    "movementX",     "movementY", "button",   "buttons",   "ctrlKey",   "shiftKey",    "altKey",   "metaKey",
    "relatedTarget", "key",       "code",     "keyCode",   "which",     "charCode",    "repeat",   "location",
    "deltaX",        "deltaY",    "deltaZ",   "deltaMode", "pointerId", "pointerType", "pressure", "isPrimary",
    "data",          "message",   "filename", "lineno",    "colno",     "error",       "origin",   "lastEventId",
    "source",        "ports",     "oldValue", "newValue",  "url",       "storageArea", "dataTransfer",
    "clipboardData",
};

/* Constructor shared by every Event subclass */
js_value event_sub_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_value ev, proto = ctx->native_data;
  (void)thisv;
  if (argc < 1)
    return js_throw_type(ctx, "Failed to construct event: 1 argument required, but only 0 present.");
  ev = make_event(arg_str(ctx, A(0)), 0, 0, A(1));
  if (ev.type != JV_OBJ)
    return ev;
  js_set(ctx, ev, "composed", js_bool(A(1).type == JV_OBJ && js_to_boolean(js_get(ctx, A(1), "composed"))));
  if (proto.type == JV_OBJ)
    ev.u.obj->proto = proto.u.obj;
  if (argc >= 2 && A(1).type == JV_OBJ) {
    unsigned i;
    gc_push_scope();
    gc_keep(ev.u.obj);
    gc_keep(A(1).u.obj);
    for (i = 0; i < sizeof k_event_init_keys / sizeof k_event_init_keys[0]; i++) {
      js_value v = js_get(ctx, A(1), k_event_init_keys[i]);
      if (v.type != JV_UNDEF)
        js_def(ev.u.obj, k_event_init_keys[i], v, P_WRITABLE | P_ENUMERABLE);
    }
    gc_pop_scope();
  }
  return ev;
}
