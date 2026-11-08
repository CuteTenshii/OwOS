/* Test runner: every suite registers itself from a constructor, then each runs in its own forked process
 * (crashes and hangs stay contained, suites run in parallel). Output is printed in suite-name order.
 *
 *   owos-tests [-v] [-l] [-jN] [suite...]
 *
 * -v shows the sections of passing suites and the slowest suites, -l lists the suites, -jN sets the number of
 * parallel suites (default: OWOS_TEST_JOBS, else the CPU count). With names, only those suites run; exit
 * status 2 means none of the names matched. OWOS_TEST_TIMEOUT (seconds) bounds each suite; unset means no limit. */
#include "test.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static struct owos_test_case *tests;
static int failures;
static const char *section;

void owos_test_register(struct owos_test_case *test)
{
  test->next = tests;
  tests = test;
}

static double suite_start;
static double now(void);

void owos_test(const char *name, void (*fn)(void))
{
  section = name;
  printf("  TEST %s  +%.2fs\n", name, now() - suite_start);
  fflush(stdout);
  fn();
  section = NULL;
}

void owos_expect(int condition, const char *file, int line, const char *expr)
{
  if (!condition) {
    printf("  FAIL %s:%d: %s%s%s\n", file, line, section ? section : "", section ? ": " : "", expr);
    failures++;
  }
}

void owos_expect_string(const char *got, const char *want, const char *file, int line, const char *expr)
{
  if (got && want && strcmp(got, want) == 0)
    return;
  owos_expect(0, file, line, expr);
  printf("    got:  %s%s%s\n    want: %s%s%s\n", got ? "\"" : "", got ? got : "(null)", got ? "\"" : "",
         want ? "\"" : "", want ? want : "(null)", want ? "\"" : "");
}

void owos_expect_int(long long got, long long want, const char *file, int line, const char *expr)
{
  if (got == want)
    return;
  owos_expect(0, file, line, expr);
  printf("    got:  %lld (0x%llx)\n    want: %lld (0x%llx)\n", got, (unsigned long long)got, want,
         (unsigned long long)want);
}

static void *xalloc(size_t count, size_t size)
{
  void *p = calloc(count ? count : 1, size);
  if (!p) {
    fputs("test runner: out of memory\n", stderr);
    exit(1);
  }
  return p;
}

void owos_parallel(int workers, void (*fn)(int id, int workers, void *ctx), void *ctx)
{
  if (workers <= 0)
    workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (workers <= 0)
    workers = 1;
  pid_t *pids = xalloc((size_t)workers, sizeof *pids);
  fflush(stdout);
  for (int i = 0; i < workers; i++) {
    pids[i] = fork();
    if (pids[i] == 0) {
      failures = 0;
      fn(i, workers, ctx);
      fflush(stdout);
      _exit(failures != 0);
    }
  }
  for (int i = 0; i < workers; i++) {
    int wstatus = 0;
    waitpid(pids[i], &wstatus, 0);
    if (pids[i] < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
      printf("  worker %d of %d failed\n", i, workers);
      failures++;
    }
  }
  free(pids);
}

struct result {
  int status; /* 0 pass, 1 failed expectations or nonzero return */
  double seconds;
};

static double now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int by_name(const void *a, const void *b)
{
  return strcmp((*(struct owos_test_case *const *)a)->name, (*(struct owos_test_case *const *)b)->name);
}

static void run_child(struct owos_test_case *test, FILE *log, struct result *res, unsigned timeout)
{
  double start = suite_start = now();
  dup2(fileno(log), STDOUT_FILENO);
  dup2(fileno(log), STDERR_FILENO);
  if (timeout)
    alarm(timeout);
  int rc = test->run();
  fflush(stdout);
  res->seconds = now() - start;
  res->status = rc != 0 || failures != 0;
  _exit(0);
}

static void print_log(FILE *log)
{
  int c;
  fseek(log, 0, SEEK_SET);
  while ((c = fgetc(log)) != EOF)
    putchar(c);
}

static const char *describe(int wstatus, struct result *res, unsigned timeout, char *buf, size_t cap)
{
  if (WIFSIGNALED(wstatus)) {
    int sig = WTERMSIG(wstatus);
    if (sig == SIGALRM)
      snprintf(buf, cap, "timed out after %us", timeout);
    else
      snprintf(buf, cap, "killed by signal %d (%s)", sig, strsignal(sig));
    return buf;
  }
  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    snprintf(buf, cap, "exited with status %d", WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
    return buf;
  }
  return res->status ? "failed" : 0;
}

int main(int argc, char **argv)
{
  int n = 0, verbose = 0, list = 0, nwant = 0;
  long jobs = 0;
  const char *env = getenv("OWOS_TEST_JOBS");
  const char *tmo = getenv("OWOS_TEST_TIMEOUT");
  unsigned timeout = tmo ? (unsigned)atoi(tmo) : 0;
  const char **want = xalloc((size_t)argc, sizeof *want);

  if (env)
    jobs = atol(env);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v"))
      verbose = 1;
    else if (!strcmp(argv[i], "-l"))
      list = 1;
    else if (!strncmp(argv[i], "-j", 2))
      jobs = atol(argv[i] + 2);
    else
      want[nwant++] = argv[i];
  }
  if (jobs <= 0)
    jobs = sysconf(_SC_NPROCESSORS_ONLN);
  if (jobs <= 0)
    jobs = 1;

  int total = 0;
  for (struct owos_test_case *t = tests; t; t = t->next)
    total++;
  struct owos_test_case **suites = xalloc((size_t)total, sizeof *suites);
  for (struct owos_test_case *t = tests; t; t = t->next) {
    int keep = nwant == 0;
    for (int i = 0; i < nwant; i++)
      keep |= !strcmp(want[i], t->name);
    if (keep)
      suites[n++] = t;
  }
  qsort(suites, (size_t)n, sizeof suites[0], by_name);
  if (list) {
    for (int i = 0; i < n; i++)
      puts(suites[i]->name);
    return 0;
  }
  if (nwant > 0 && n == 0)
    return 2;

  struct result *res = mmap(0, sizeof *res * (size_t)n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  FILE **logs = xalloc((size_t)n, sizeof *logs);
  pid_t *pids = xalloc((size_t)n, sizeof *pids);
  int *wst = xalloc((size_t)n, sizeof *wst);
  int *done = xalloc((size_t)n, sizeof *done);
  if (res == MAP_FAILED)
    return 1;

  int started = 0, running = 0, printed = 0, passed = 0, failed = 0;
  double begin = now();
  fflush(stdout);
  while (printed < n) {
    while (started < n && running < jobs) {
      logs[started] = tmpfile();
      fflush(stdout);
      pid_t pid = fork();
      if (pid == 0) {
        failures = 0;
        run_child(suites[started], logs[started], &res[started], timeout);
      }
      pids[started++] = pid;
      running++;
    }
    int wstatus;
    pid_t pid = waitpid(-1, &wstatus, 0);
    if (pid < 0)
      break;
    for (int i = 0; i < started; i++)
      if (pids[i] == pid && !done[i]) {
        done[i] = 1;
        wst[i] = wstatus;
        running--;
      }
    while (printed < n && done[printed]) {
      int i = printed++;
      char why[96];
      const char *bad = describe(wst[i], &res[i], timeout, why, sizeof why);
      if (!bad) {
        passed++;
        printf("PASS %s (%.2fs)\n", suites[i]->name, res[i].seconds);
        if (verbose)
          print_log(logs[i]);
      } else {
        failed++;
        printf("TEST %s\n", suites[i]->name);
        print_log(logs[i]);
        printf("FAIL %s (%s)\n", suites[i]->name, bad);
      }
      fclose(logs[i]);
    }
  }

  if (verbose && n > 0) {
    int *order = xalloc((size_t)n, sizeof *order);
    for (int i = 0; i < n; i++)
      order[i] = i;
    for (int i = 0; i < n && i < 5; i++)
      for (int j = i + 1; j < n; j++)
        if (res[order[j]].seconds > res[order[i]].seconds) {
          int t = order[i];
          order[i] = order[j];
          order[j] = t;
        }
    printf("\nslowest:\n");
    for (int i = 0; i < n && i < 5; i++)
      printf("  %6.2fs  %s\n", res[order[i]].seconds, suites[order[i]]->name);
  }
  printf("\n%d passed, %d failed (%.2fs, %ld jobs)\n", passed, failed, now() - begin, jobs);
  return failed != 0 || passed == 0;
}
