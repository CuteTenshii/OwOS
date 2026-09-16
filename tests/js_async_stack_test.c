/* js_async_stack_test.c — verify async stack chaining (js_async_capture/enter/leave).
 *
 * Simulates a deferred callback the way the browser's timer queue does: a native
 * `schedule(cb)` snapshots the current frames at the arming site; after the top-level
 * script returns, the harness installs that snapshot and fires the callback. A throw
 * inside the callback must produce a .stack that names the *arming* site, not just the
 * callback body.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      tests/js_async_stack_test.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/js_async_stack_test \
 *      && /tmp/js_async_stack_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "parser.h"
#include "interp.h"
#include "value.h"
#include "gc.h"

static void con_print(js_ctx *ctx, int level, const char *s, int len) {
    (void)ctx; (void)level;
    printf("%.*s\n", len, s);
}

/* one deferred callback + the frames captured when it was armed */
static js_object *g_cb;          /* GC-rooted */
static char      *g_async;

static js_value native_schedule(js_ctx *ctx, js_value thisv, js_value *args, int argc) {
    (void)thisv;
    if (argc > 0 && js_is_callable(args[0])) {
        if (g_async) free(g_async);
        g_cb = args[0].u.obj;
        g_async = js_async_capture(ctx);   /* chains any async parent already installed */
    }
    return js_undef();
}

static void expect_contains(const char *hay, const char *needle) {
	expect(hay && strstr(hay, needle) != NULL);
}

int main(void) {
    js_arena a;
    js_parse_error err = {0, 0};
    js_node *prog;
    js_ctx *ctx;
    js_value result;
    js_value stack;

    /* boom is armed two frames deep by armer(); boom itself arms boom2 while it runs
     * as a deferred callback, so boom2's stack should chain BOTH deferral hops. */
    const char *src =
        "function inner(){ schedule(function boom(){\n"
        "    schedule(function boom2(){ throw new Error('kaboom'); });\n"
        "}); }\n"
        "function armer(){ inner(); }\n"
        "armer();\n";

    js_arena_init(&a);
    ctx = js_ctx_new(con_print);
    gc_add_root((void **)&g_cb);
    js_def_native(ctx, ctx->global, "schedule", native_schedule, 1);

    prog = js_parse(src, &a, &err);
    if (!prog) { expect(0); return 0; }
    if (js_run(ctx, prog, &result) != 0) { expect(0); return 0; }

    /* drive the "event loop": fire each scheduled callback with its arming frames
     * installed, exactly like the timer pump does around js_call, until the queue
     * drains or a callback throws. */
	test("deferred callback stack chain");
    while (g_cb && g_async && !ctx->has_exc) {
        js_object *cb = g_cb;
        char *async = g_async;
        char *prev;
        g_cb = 0; g_async = 0;
        printf("firing a deferred callback; installed async stack:\n%s\n", async);
        prev = js_async_enter(ctx, async);
        js_call(ctx, js_obj_value(cb), js_obj_value(ctx->global), 0, 0);
        js_async_leave(ctx, prev);
        free(async);
    }
    expect(ctx->has_exc);
    if (!ctx->has_exc) return 0;

    stack = js_get(ctx, ctx->exc, "stack");
    expect(stack.type == JV_STR);
    if (stack.type != JV_STR) return 0;

    printf("\nthrown error .stack:\n%s\n", stack.u.str->data);

    /* the throwing frame, plus BOTH deferral hops chained back to the original armer */
    expect_contains(stack.u.str->data, "kaboom");
    expect_contains(stack.u.str->data, "at boom2");
    expect_contains(stack.u.str->data, "at boom");
    expect_contains(stack.u.str->data, "at inner");
    expect_contains(stack.u.str->data, "at armer");
    {   /* two "scheduled from" boundaries, one per deferral hop */
        const char *p = strstr(stack.u.str->data, "scheduled from");
        int n = 0;
        while (p) { n++; p = strstr(p + 1, "scheduled from"); }
        expect(n == 2);
    }

    if (g_async) free(g_async);
    js_ctx_free(ctx);
    js_arena_free(&a);
    return 0;
}
