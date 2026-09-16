#include "notification.h"
#include "../jsint.h"
#include "notify.h"

static js_ctx *ctx;
static js_object *proto;
static js_object *queue;

void notif_app_name(char *buf, int cap)
{
  strlcpy(buf, "Browser", cap);
}

static const char *option_string(js_value options, const char *name, const char *fallback)
{
  js_value value;
  if (options.type != JV_OBJ)
    return fallback;
  value = js_get(ctx, options, name);
  if (value.type == JV_UNDEF || value.type == JV_NULL)
    return fallback;
  return arg_str(ctx, value);
}

static int option_bool(js_value options, const char *name)
{
  return options.type == JV_OBJ && js_to_boolean(js_get(ctx, options, name));
}

static js_value notification_ctor(js_ctx *c, js_value thisv, js_value *args, int argc)
{
  js_object *object;
  js_value options, value;
  const char *title, *body, *dir;
  char app[NOTIFY_APP_MAX];
  (void)thisv;
  if (argc < 1)
    return js_throw_type(c, "Failed to construct 'Notification': 1 argument "
                            "required, but only 0 present.");
  title = arg_str(c, A(0));
  options = A(1);
  body = option_string(options, "body", "");
  dir = option_string(options, "dir", "auto");
  gc_push_scope();
  object = (js_object *)gc_keep(js_new_object(c, proto));
  js_def(object, "title", js_mk_cstring(c, title), P_ENUMERABLE);
  js_def(object, "body", js_mk_cstring(c, body), P_ENUMERABLE);
  js_def(object, "icon", js_mk_cstring(c, option_string(options, "icon", "")), P_ENUMERABLE);
  js_def(object, "badge", js_mk_cstring(c, option_string(options, "badge", "")), P_ENUMERABLE);
  js_def(object, "image", js_mk_cstring(c, option_string(options, "image", "")), P_ENUMERABLE);
  js_def(object, "tag", js_mk_cstring(c, option_string(options, "tag", "")), P_ENUMERABLE);
  js_def(object, "lang", js_mk_cstring(c, option_string(options, "lang", "")), P_ENUMERABLE);
  js_def(object, "dir", js_mk_cstring(c, dir), P_ENUMERABLE);
  js_def(object, "data", options.type == JV_OBJ ? js_get(c, options, "data") : js_null(), P_ENUMERABLE);
  js_def(object, "requireInteraction", js_bool(option_bool(options, "requireInteraction")), P_ENUMERABLE);
  js_def(object, "silent", js_bool(option_bool(options, "silent")), P_ENUMERABLE);
  js_def(object, "renotify", js_bool(option_bool(options, "renotify")), P_ENUMERABLE);
  js_def(object, "onclick", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(object, "onclose", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(object, "onerror", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(object, "onshow", js_null(), P_WRITABLE | P_ENUMERABLE);
  js_def(object, "@shown", js_bool(0), P_WRITABLE);
  js_def(object, "@close_req", js_bool(0), P_WRITABLE);
  js_def(object, "@closed", js_bool(0), P_WRITABLE);
  if (perm_state_now(PERM_NOTIFICATIONS) == PERM_GRANTED && title[0]) {
    struct notification notification = {0};
    strlcpy(app, "Browser", sizeof app);
    notification.app = app;
    notification.title = title;
    notification.body = body[0] ? body : 0;
    notify_send(&notification);
  }
  if (queue)
    js_array_push(c, queue, js_obj_value(object));
  value = js_obj_value(object);
  gc_pop_scope();
  return value;
}

static js_value notification_close(js_ctx *c, js_value target, js_value *args, int argc)
{
  (void)c;
  (void)args;
  (void)argc;
  if (target.type == JV_OBJ)
    js_set(ctx, target, "@close_req", js_bool(1));
  return js_undef();
}

static js_value notification_permission(js_ctx *c, js_value target, js_value *args, int argc)
{
  (void)target;
  (void)args;
  (void)argc;
  return js_mk_cstring(c, perm_state_str(perm_state_now(PERM_NOTIFICATIONS)));
}

static js_value notification_request_permission(js_ctx *c, js_value target, js_value *args, int argc)
{
  js_value permission = js_mk_cstring(c, perm_state_str(worker_permission(PERM_NOTIFICATIONS)));
  (void)target;
  if (argc > 0 && js_is_callable(args[0]))
    js_call(c, args[0], js_undef(), &permission, 1);
  return js_promise_resolved(c, permission);
}

void js_notification_setup(js_ctx *c, js_object *win, js_object *p, js_object *q)
{
  js_object *constructor, *permission;
  ctx = c;
  proto = p;
  queue = q;
  def_method(proto, "close", notification_close, 0);
  def_method(proto, "addEventListener", el_add_listener, 3);
  def_method(proto, "removeEventListener", el_remove_listener, 3);
  def_method(proto, "dispatchEvent", el_dispatch, 1);
  constructor = js_new_native(c, notification_ctor, "Notification", 1);
  js_def(constructor, "prototype", js_obj_value(proto), 0);
  js_def(proto, "constructor", js_obj_value(constructor), P_WRITABLE | P_CONFIGURABLE);
  permission = js_new_native(c, notification_permission, "permission", 0);
  js_def_accessor(constructor, "permission", permission, 0, P_CONFIGURABLE);
  js_def_native(c, constructor, "requestPermission", notification_request_permission, 1);
  js_def(constructor, "maxActions", js_number(NOTIFY_MAX_BUTTONS), P_ENUMERABLE);
  js_def(win, "Notification", js_obj_value(constructor), P_WRITABLE | P_CONFIGURABLE);
}

int js_notifications_pump(void)
{
  int fired = 0;
  unsigned i, count;
  if (!ctx || !queue)
    return 0;
  count = js_array_len(queue);
  for (i = 0; i < count; i++) {
    js_value value = js_get_index(ctx, js_obj_value(queue), i);
    if (value.type != JV_OBJ)
      continue;
    if (!js_to_boolean(js_get(ctx, value, "@shown"))) {
      js_set(ctx, value, "@shown", js_bool(1));
      emit_event(value.u.obj, "show", js_undef());
      fired++;
    }
    if (js_to_boolean(js_get(ctx, value, "@close_req")) && !js_to_boolean(js_get(ctx, value, "@closed"))) {
      js_set(ctx, value, "@closed", js_bool(1));
      emit_event(value.u.obj, "close", js_undef());
      fired++;
    }
  }
  return fired;
}

void js_notification_teardown(void)
{
  ctx = 0;
  proto = 0;
  queue = 0;
}
