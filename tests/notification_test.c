/* Host test for the production Notification API module. */
#define OWOS_SUITE_TEST
#include "features/notification/notification.h"
#include "jsint.h"
#include "notify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static js_object *listeners;
static int sent;
static char last_title[64];

int perm_state_now(int feature)
{
  (void)feature;
  return PERM_GRANTED;
}
int worker_permission(int feature)
{
  (void)feature;
  return PERM_GRANTED;
}
const char *perm_state_str(int state) { return state == PERM_GRANTED ? "granted" : "denied"; }
void report_exc(js_ctx *ctx) { (void)ctx; }
int notify_send(const struct notification *n)
{
  sent++;
  snprintf(last_title, sizeof last_title, "%s", n->title);
  return 0;
}

js_value el_add_listener(js_ctx *ctx, js_value target, js_value *args, int argc)
{
  js_object *list, *record;
  if (target.type != JV_OBJ || argc < 2 || !js_is_callable(args[1]))
    return js_undef();
  list = listeners;
  record = js_new_object(ctx, ctx->object_proto);
  js_def(record, "type", args[0], 0);
  js_def(record, "fn", args[1], 0);
  js_array_push(ctx, list, js_obj_value(record));
  return js_undef();
}
js_value el_remove_listener(js_ctx *ctx, js_value target, js_value *args, int argc)
{
  (void)ctx;
  (void)target;
  (void)args;
  (void)argc;
  return js_undef();
}
js_value el_dispatch(js_ctx *ctx, js_value target, js_value *args, int argc)
{
  (void)ctx;
  (void)target;
  (void)args;
  (void)argc;
  return js_bool(0);
}
void emit_event(js_object *target, const char *type, js_value data)
{
  js_value event = js_obj_value(js_new_object(g_ctx, g_ctx->object_proto));
  js_value handler, value;
  unsigned i;
  js_def(event.u.obj, "type", js_mk_cstring(g_ctx, type), P_ENUMERABLE);
  js_def(event.u.obj, "target", js_obj_value(target), P_ENUMERABLE);
  if (data.type != JV_UNDEF)
    js_def(event.u.obj, "data", data, P_ENUMERABLE);
  handler = js_get(g_ctx, js_obj_value(target), type[0] == 's' ? "onshow" : "onclose");
  if (js_is_callable(handler))
    js_call(g_ctx, handler, js_obj_value(target), &event, 1);
  for (i = 0; i < js_array_len(listeners); i++) {
    value = js_get_index(g_ctx, js_obj_value(listeners), i);
    if (value.type == JV_OBJ && js_is_callable(js_get(g_ctx, value, "fn")))
      js_call(g_ctx, js_get(g_ctx, value, "fn"), js_obj_value(target), &event, 1);
  }
}

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  (void)s;
  (void)len;
}
static js_value test_expect(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  expect(argc > 0 && js_to_boolean(args[0]));
  return js_undef();
}
static js_value pump(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(js_notifications_pump());
}

static void run(const char *source)
{
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value result;
  js_arena_init(&arena);
  program = js_parse(source, &arena, &error);
  expect(program != 0);
  if (program)
    expect(js_run(g_ctx, program, &result) == 0);
  js_arena_free(&arena);
}

static void check_lifecycle(void)
{
  run("expect(Notification.permission==='granted');\n"
      "const n=new Notification('Hello',{body:'World'});\n"
      "expect(n.title==='Hello' && n.body==='World');\n"
      "let shows=0, closes=0; n.onshow=function(){shows++;}; n.onclose=function(){closes++;};\n"
      "expect(shows===0);\n"
      "__pump();\n"
      "expect(shows===1);\n"
      "n.close(); __pump(); expect(closes===1);\n");
  expect(sent == 1 && !strcmp(last_title, "Hello"));
}

int main(void)
{
  js_object *proto, *queue;
  g_ctx = js_ctx_new(con_print);
  proto = js_new_object(g_ctx, g_ctx->object_proto);
  queue = js_new_array(g_ctx, 0);
  listeners = js_new_array(g_ctx, 0);
  js_def_native(g_ctx, g_ctx->global, "expect", test_expect, 1);
  js_def_native(g_ctx, g_ctx->global, "__pump", pump, 0);
  js_notification_setup(g_ctx, g_ctx->global, proto, queue);
  test("production Notification lifecycle", check_lifecycle);
  js_notification_teardown();
  js_ctx_free(g_ctx);
  return 0;
}
