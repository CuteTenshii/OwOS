/* One js_ctx per OS thread, each over its own GC heap, running collections at the same time. */
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "mutex.h"
#include "parser.h"
#include "value.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>

#define NTHREADS 8
#define ROUNDS 30
#define SHARED_BYTES 64

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
  const char *script;
  unsigned char *shared; /* SharedArrayBuffer store handed to this thread, or NULL */
  js_irq irq;            /* lets the test stop the thread; used when `stoppable` */
  int stoppable;
};

static void *worker(void *arg)
{
  struct job *job = arg;
  gc_heap *heap = gc_heap_new();
  js_arena arena;
  js_parse_error error = {0, 0};
  js_node *program;
  js_value value;
  js_ctx *ctx;
  gc_heap_attach(heap);
  ctx = js_ctx_new(con_print);
  ctx->can_block = 1;
  if (job->stoppable)
    ctx->irq = &job->irq;
  if (job->shared)
    js_def(ctx->global, "sab", js_sab_wrap(ctx, job->shared, SHARED_BYTES), 0);
  gc_set_threshold(2048);
  js_def(ctx->global, "id", js_number(job->id), 0);
  js_arena_init(&arena);
  program = js_parse(job->script, &arena, &error);
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

static void run_jobs(struct job *jobs, int n)
{
  pthread_t threads[NTHREADS];
  int i;
  for (i = 0; i < n; i++)
    pthread_create(&threads[i], NULL, worker, &jobs[i]);
  for (i = 0; i < n; i++)
    pthread_join(threads[i], NULL);
}

static void check_parallel_contexts(void)
{
  struct job jobs[NTHREADS] = {0};
  int i;
  for (i = 0; i < NTHREADS; i++) {
    jobs[i].id = i + 1;
    jobs[i].ok = 0;
    jobs[i].script = script;
    jobs[i].shared = NULL;
  }
  run_jobs(jobs, NTHREADS);
  for (i = 0; i < NTHREADS; i++) {
    expect(jobs[i].ok);
    expect_eq((long long)jobs[i].result, ROUNDS * (190 * jobs[i].id + 6) + 299);
  }
}

/* One store, several heaps: counts survive contention and a sleeping Atomics.wait is woken by another thread. */
static void check_shared_buffer(void)
{
  static const char counter_script[] = "var i = new Int32Array(sab);\n"
                                       "for (var n = 0; n < 5000; n++) Atomics.add(i, 0, 1);\n"
                                       "result = 1;\n";
  static const char waiter_script[] = "var i = new Int32Array(sab);\n"
                                      "var r = Atomics.wait(i, 1, 0);\n"
                                      "Atomics.store(i, 2, Atomics.load(i, 1) * 2);\n"
                                      "result = r === 'ok' || r === 'not-equal' ? 1 : 0;\n";
  static const char notifier_script[] = "var i = new Int32Array(sab);\n"
                                        "Atomics.store(i, 1, 21);\n"
                                        "Atomics.notify(i, 1);\n"
                                        "result = 1;\n";
  struct job jobs[NTHREADS] = {0};
  unsigned char *store = js_sab_alloc(SHARED_BYTES);
  int i, *words = (int *)store;
  expect(store != NULL);
  for (i = 0; i < NTHREADS; i++) {
    jobs[i].id = i;
    jobs[i].ok = 0;
    jobs[i].script = i == 0 ? waiter_script : i == 1 ? notifier_script : counter_script;
    jobs[i].shared = store;
  }
  run_jobs(jobs, NTHREADS);
  for (i = 0; i < NTHREADS; i++) {
    expect(jobs[i].ok);
    expect_eq((long long)jobs[i].result, 1);
  }
  expect_eq(words[0], (NTHREADS - 2) * 5000);
  expect_eq(words[2], 42);
  js_sab_release(store);
}

/* A stop request ends a script that would never return: a busy loop, or an Atomics.wait with no waker */
static void check_stop_request(void)
{
  static const char *const scripts[] = {"try { while (true) {} } catch (e) { result = 1 } finally { result = 1 }",
                                        "var i = new Int32Array(sab); Atomics.wait(i, 0, 0); result = 1"};
  int k;
  for (k = 0; k < 2; k++) {
    struct job job = {0};
    pthread_t thread;
    unsigned char *store = js_sab_alloc(SHARED_BYTES);
    job.script = scripts[k];
    job.shared = store;
    job.stoppable = 1;
    pthread_create(&thread, NULL, worker, &job);
    usleep(30000);
    __atomic_store_n(&job.irq.stop, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&job.irq.waiting, __ATOMIC_ACQUIRE))
      futex_wake(__atomic_load_n(&job.irq.waiting, __ATOMIC_ACQUIRE), 1 << 30);
    pthread_join(thread, NULL);
    expect(!job.ok); /* the script was cut short: neither catch nor finally nor the next statement ran */
    js_sab_release(store);
  }
}

static jmp_buf oom_env;
static void recover_from_oom(void *arg)
{
  (void)arg;
  longjmp(oom_env, 1);
}
static void *oom_thread(void *arg)
{
  int *recovered = arg;
  gc_heap *heap = gc_heap_new();
  gc_heap_attach(heap);
  gc_heap_on_oom(heap, recover_from_oom, 0);
  if (setjmp(oom_env) == 0)
    gc_oom();
  else
    *recovered = 1;
  gc_heap_detach();
  gc_heap_free(heap);
  return NULL;
}

/* An allocation failure in a thread's heap unwinds to the thread instead of aborting the process */
static void check_oom_hook(void)
{
  pthread_t thread;
  int recovered = 0;
  pthread_create(&thread, NULL, oom_thread, &recovered);
  pthread_join(thread, NULL);
  expect(recovered);
}

int main(void)
{
  test("contexts on separate threads and heaps", check_parallel_contexts);
  test("SharedArrayBuffer and Atomics across threads", check_shared_buffer);
  test("a stop request ends a running or blocked script", check_stop_request);
  test("heap out-of-memory hook unwinds to the thread", check_oom_hook);
  return 0;
}
