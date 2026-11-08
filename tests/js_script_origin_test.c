/* js_script_origin_test.c — verify the script-origin frame (js_set_script).
 *
 * The gap this covers: a deferred callback armed at a script's TOP LEVEL has no JS
 * function on the stack, so the frame snapshot is empty and a throw in the callback
 * never names the script that armed it. js_set_script installs an origin label that
 * the engine emits as the outermost frame of every captured segment, so the stack
 * bottoms out at its script even with zero function frames — and each async hop is
 * labelled with the script that scheduled it.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      tests/js_script_origin_test.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/js_script_origin_test \
 *      && /tmp/js_script_origin_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  printf("%.*s\n", len, s);
}

static js_object *g_cb; /* GC-rooted */
static char *g_async;

static js_value native_schedule(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  if (argc > 0 && js_is_callable(args[0])) {
    if (g_async)
      free(g_async);
    g_cb = args[0].u.obj;
    g_async = js_async_capture(ctx);
  }
  return js_undef();
}

static void expect_contains(const char *hay, const char *needle) { expect(hay && strstr(hay, needle) != NULL); }

static js_ctx *ctx;

/* fire it under the timer-callback origin, exactly like js_timers_pump */
static void check_origin_propagation(void)
{
  js_value stack;
  {
    char *prev = js_async_enter(ctx, g_async);
    js_set_script(ctx, "<timer callback>");
    js_call(ctx, js_obj_value(g_cb), js_obj_value(ctx->global), 0, 0);
    js_async_leave(ctx, prev);
  }
  expect(ctx->has_exc);
  if (!ctx->has_exc)
    return;

  stack = js_get(ctx, ctx->exc, "stack");
  expect(stack.type == JV_STR);
  if (stack.type != JV_STR)
    return;
  printf("\nthrown error .stack:\n%s\n", stack.u.str->data);

  /* The whole chain: the error, the callback's origin, the scheduled-from boundary,
   * and — the actual goal — the SCRIPT that armed the timer, despite no JS frame. */
  expect_contains(stack.u.str->data, "late");
  expect_contains(stack.u.str->data, "at <timer callback>");
  expect_contains(stack.u.str->data, "scheduled from");
  expect_contains(stack.u.str->data, "at https://example.com/app.js");
}

/* run one script under a given origin label, then drain the (single) callback the
 * loop scheduled, under the callback's own origin label — like the timer pump. */
int main(void)
{
  js_arena a;
  js_parse_error err = {0, 0};
  js_node *prog;
  js_value result;

  /* setTimeout armed at top level: NO function frame on the stack at arm time. */
  const char *src = "schedule(function () { throw new Error('late'); });\n";

  js_arena_init(&a);
  ctx = js_ctx_new(con_print);
  gc_add_root((void **)&g_cb);
  js_def_native(ctx, ctx->global, "schedule", native_schedule, 1);

  js_set_script(ctx, "https://example.com/app.js"); /* the running page script */
  prog = js_parse(src, &a, &err);
  if (!prog) {
    expect(0);
    return 0;
  }
  if (js_run(ctx, prog, &result) != 0) {
    expect(0);
    return 0;
  }

  expect(g_cb && g_async);
  if (!g_cb || !g_async)
    return 0;
  printf("captured async stack (armed at top level):\n%s\n", g_async);

  test("top-level script origin propagation", check_origin_propagation);

  if (g_async)
    free(g_async);
  js_ctx_free(ctx);
  js_arena_free(&a);
  return 0;
}
