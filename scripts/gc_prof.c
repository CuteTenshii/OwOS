/* gc_prof.c — profile the JS GC on a DOM-like workload (host, no QEMU).
 *
 * Builds a large long-lived object tree (a stand-in for the retained DOM), then
 * runs a churny loop that allocates short-lived garbage (a stand-in for layout /
 * script work). That is the pathological shape for a full mark-sweep collector:
 * every minor collection re-traces the whole retained tree.
 *
 *   cc -std=gnu11 -O2 -Itests -Iuser/lib -Iuser/lib/js -Iuser/lib/math \
 *      -Iuser/lib/gc scripts/gc_prof.c user/lib/js/lexer.c user/lib/js/parser.c \
 *      user/lib/js/ast.c user/lib/js/value.c user/lib/js/interp.c \
 *      user/lib/js/builtins.c user/lib/js/bigint.c user/lib/math/math.c \
 *      user/lib/math/fmt.c user/lib/gc/gc.c -o /tmp/gc_prof && /tmp/gc_prof
 */
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include <stdio.h>
#include <time.h>

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  (void)s;
  (void)len; /* swallow console output */
}

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* A workload with a big retained tree and heavy short-lived churn. NODES controls
 * the retained DOM size; ITER the amount of subsequent "layout" allocation. */
static const char *SRC = "var NODES = 20000;\n"
                         "function makeNode(depth){\n"
                         "  var n = { tag:'div', depth:depth, attrs:{id:'x',cls:'y'}, kids:[] };\n"
                         "  return n;\n"
                         "}\n"
                         "var root = makeNode(0);\n"
                         "var all = [root];\n"
                         "// build a wide/deep retained tree of NODES nodes\n"
                         "var i = 1;\n"
                         "while (i < NODES) {\n"
                         "  var parent = all[(i>>1)];\n"
                         "  var n = makeNode(parent.depth+1);\n"
                         "  parent.kids.push(n);\n"
                         "  all.push(n);\n"
                         "  i++;\n"
                         "}\n"
                         "// churn: lots of short-lived work touching the live tree (layout-like)\n"
                         "var acc = 0;\n"
                         "for (var pass = 0; pass < 40; pass++) {\n"
                         "  for (var j = 0; j < all.length; j++) {\n"
                         "    var n = all[j];\n"
                         "    var box = { x:j, y:n.depth, w:100, h:20, node:n, style:{color:'red'} };\n"
                         "    acc += box.w + box.h + n.depth;\n"
                         "  }\n"
                         "}\n"
                         "console.log(acc);\n";

int main(void)
{
  js_arena a;
  js_parse_error err = {0, 0};
  js_node *prog;
  js_ctx *ctx;
  js_value result;
  gc_stats_t s;
  double t0, t1;

  js_arena_init(&a);
  ctx = js_ctx_new(con_print);
  prog = js_parse(SRC, &a, &err);
  if (!prog) {
    printf("PARSE ERROR: %s\n", err.msg ? err.msg : "?");
    return 1;
  }

  t0 = now_ms();
  if (js_run(ctx, prog, &result) != 0)
    printf("UNCAUGHT during run\n");
  t1 = now_ms();

  gc_get_stats(&s);
  printf("wall:        %.1f ms\n", t1 - t0);
  printf("collections: %lu  (minor %lu, major %lu)\n", s.collections, s.minors, s.majors);
  printf("marked:      %lu  (avg %.0f/collection)\n", s.marked, s.collections ? (double)s.marked / s.collections : 0.0);
  printf("swept:       %lu  (avg %.0f/collection)\n", s.swept, s.collections ? (double)s.swept / s.collections : 0.0);
  printf("freed:       %lu\n", s.freed);
  printf("gc time:     %.1f ms  (%.0f%% of wall)\n", s.ms, (t1 - t0) > 0 ? 100.0 * s.ms / (t1 - t0) : 0.0);
  printf("max pause:   %.2f ms\n", s.max_ms);
  printf("live bytes:  %u\n", gc_bytes());

  js_ctx_free(ctx);
  js_arena_free(&a);
  return 0;
}
