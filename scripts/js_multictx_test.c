/* js_multictx_test.c — verify that several js_ctx instances coexist over the one
 * shared GC heap: creating a second context must NOT wipe the first's objects,
 * and a collection driven by one must keep the other's world alive. This is the
 * invariant the browser's per-tab live JS relies on.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc \
 *      scripts/js_multictx_test.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/js_multictx_test && \
 *   /tmp/js_multictx_test
 */
#include <stdio.h>
#include <stdlib.h>
#include "ast.h"
#include "parser.h"
#include "interp.h"
#include "value.h"
#include "gc.h"

static int failures;

static void con_print(js_ctx *ctx, int level, const char *s, int len) {
    (void)ctx; (void)level;
    fprintf(stderr, "[console] %.*s\n", len, s);
}

/* Run one script in ctx; abort the test on parse/runtime error. */
static void run_in(js_ctx *ctx, const char *src) {
    js_arena a;
    js_parse_error err = {0, 0};
    js_node *prog;
    js_value result;
    js_arena_init(&a);
    prog = js_parse(src, &a, &err);
    if (!prog) { fprintf(stderr, "PARSE ERROR: %s\n", err.msg ? err.msg : "?"); failures++; js_arena_free(&a); return; }
    if (js_run(ctx, prog, &result) != 0) {
        js_value msg = js_get(ctx, ctx->exc, "message");
        fprintf(stderr, "UNCAUGHT: %s\n", msg.type == JV_STR ? msg.u.str->data : "(?)");
        failures++;
        ctx->has_exc = 0;
    }
    js_arena_free(&a);
    /* NOTE: the arena backing `prog` is freed here, mirroring the browser (each
     * script parsed into a scratch arena freed after the run). The objects the
     * script created live in the GC heap and must survive. */
}

/* Read globalThis.<obj>.v as a double; -1 on any miss. */
static double read_v(js_ctx *ctx, const char *obj) {
    js_value g = js_obj_value(ctx->global);
    js_value o = js_get(ctx, g, obj);
    if (o.type != JV_OBJ) return -1;
    js_value v = js_get(ctx, o, "v");
    return v.type == JV_NUM ? v.u.num : -1;
}

static void check(const char *what, double got, double want) {
    if (got == want) { printf("ok    %-28s = %g\n", what, got); }
    else { printf("FAIL  %-28s = %g (want %g)\n", what, got, want); failures++; }
}

int main(void) {
    /* Two contexts live at once. Historically the second js_ctx_new reset the
     * shared heap and destroyed the first; that is the regression under test. */
    js_ctx *A = js_ctx_new(con_print);
    run_in(A, "globalThis.a = { v: 111, tag: 'A' };");

    js_ctx *B = js_ctx_new(con_print);         /* must not disturb A */
    run_in(B, "globalThis.b = { v: 222, tag: 'B' };");

    /* Churn allocation in B and force collections; A's world must persist. */
    run_in(B, "for (let i = 0; i < 50000; i++) { let junk = { x: i, s: 'z' + i }; }");
    gc_collect();
    gc_collect();

    check("A.a.v after B churn+GC", read_v(A, "a"), 111);
    check("B.b.v",                  read_v(B, "b"), 222);

    /* A keeps running after B allocated heavily. */
    run_in(A, "globalThis.a.v = globalThis.a.v + 1;");
    check("A.a.v after A mutates",  read_v(A, "a"), 112);

    /* Freeing B must reclaim only B's world and leave A intact (no dangling
     * roots into the freed context — the js_ctx_free root-removal fix). */
    js_ctx_free(B);
    gc_collect();
    check("A.a.v after free(B)",    read_v(A, "a"), 112);

    /* A third context after a free still coexists. */
    js_ctx *C = js_ctx_new(con_print);
    run_in(C, "globalThis.c = { v: 333 };");
    gc_collect();
    check("A.a.v after new(C)",     read_v(A, "a"), 112);
    check("C.c.v",                  read_v(C, "c"), 333);

    js_ctx_free(A);
    js_ctx_free(C);

    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
