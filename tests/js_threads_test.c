/* One js_ctx per OS thread, each over its own GC heap, running collections at the same time. */
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "value.h"
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 8
#define ROUNDS 30

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  fprintf(stderr, "[console] %.*s\n", len, s);
}

static const char script[] = "var sum = 0, kept = [];\n"
                             "function* g(n) { for (var i = 0; i < n; i++) yield i * id; }\n"
                             "for (var r = 0; r < 30; r++) {\n"
                             "  var a = [];\n"
                             "  for (var i = 0; i < 300; i++) a.push({k: i, s: 'x' + i + id});\n"
                             "  kept.push(a);\n"
                             "  for (var v of g(20)) sum += v;\n"
                             "  sum += 'a1b22c333'.replace(/\\d+/g, function (m) { return m.length }).length;\n"
                             "  if (kept.length > 3) kept.shift();\n"
                             "}\n"
                             "result = sum + kept[kept.length - 1][299].k;\n";

struct job {
  int id;
  int ok;
  double result;
};

static void *worker(void *arg)
{
  struct job *job = arg;
  char stack_probe;
  gc_heap *heap = gc_heap_new();
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value value;
  js_ctx *ctx;
  gc_heap_attach(heap, &stack_probe - (1 << 20), &stack_probe + 4096);
  ctx = js_ctx_new(con_print);
  gc_set_threshold(2048);
  js_def(ctx->global, "id", js_number(job->id), 0);
  js_arena_init(&arena);
  program = js_parse(script, &arena, &error);
  if (program && js_run(ctx, program, &value) == 0) {
    value = js_get(ctx, js_obj_value(ctx->global), "result");
    job->result = js_to_number(ctx, value);
    job->ok = 1;
  }
  js_arena_free(&arena);
  js_ctx_free(ctx);
  gc_heap_detach();
  gc_heap_free(heap);
  return NULL;
}

static void check_parallel_contexts(void)
{
  pthread_t threads[NTHREADS];
  struct job jobs[NTHREADS];
  int i;
  for (i = 0; i < NTHREADS; i++) {
    jobs[i].id = i + 1;
    jobs[i].ok = 0;
    pthread_create(&threads[i], NULL, worker, &jobs[i]);
  }
  for (i = 0; i < NTHREADS; i++)
    pthread_join(threads[i], NULL);
  for (i = 0; i < NTHREADS; i++) {
    expect(jobs[i].ok);
    expect_eq((long long)jobs[i].result, ROUNDS * (190 * jobs[i].id + 6) + 299);
  }
}

int main(void)
{
  test("contexts on separate threads and heaps", check_parallel_contexts);
  return 0;
}
