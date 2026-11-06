/* Host test for the command-line option parser (user/lib/opt.c). */
#define OWOS_SUITE_TEST
#include "opt.h"
#include "test.h"
#include <string.h>

enum { O_VERB = 1, O_COUNT, O_OUT, O_LONGONLY };

static const struct opt tbl[] = {
    {'v', "verbose", OPT_FLAG, O_VERB}, {'x', 0, OPT_FLAG, 'x'},
    {'c', "count", OPT_ARG, O_COUNT},   {'o', "output", OPT_ARG, O_OUT},
    {0, "http3", OPT_FLAG, O_LONGONLY},
};

static int run(char **argv, int *ids, const char **vals, int max)
{
  struct optstate st;
  int argc = 0, n = 0, o;
  while (argv[argc])
    argc++;
  opt_init(&st, argc, argv);
  while ((o = opt_next(&st, tbl, OPT_N(tbl))) != OPT_DONE && n < max) {
    ids[n] = o;
    vals[n] = o == OPT_BAD ? st.bad : st.arg;
    n++;
  }
  return n;
}

int main(void)
{
  int ids[16];
  const char *vals[16];
  int n;

  test("short flags, values and positionals interleave");
  char *a1[] = {"prog", "-v", "host", "-c", "5", "-ofile", "tail", 0};
  n = run(a1, ids, vals, 16);
  expect(n == 5);
  expect(ids[0] == O_VERB);
  expect(ids[1] == OPT_POS && !strcmp(vals[1], "host"));
  expect(ids[2] == O_COUNT && !strcmp(vals[2], "5"));
  expect(ids[3] == O_OUT && !strcmp(vals[3], "file"));
  expect(ids[4] == OPT_POS && !strcmp(vals[4], "tail"));

  test("clustered flags end in a valued option");
  char *a2[] = {"prog", "-vxc7", 0};
  n = run(a2, ids, vals, 16);
  expect(n == 3);
  expect(ids[0] == O_VERB && ids[1] == 'x');
  expect(ids[2] == O_COUNT && !strcmp(vals[2], "7"));

  test("long options, =value and separate value");
  char *a3[] = {"prog", "--count=9", "--output", "f.bin", "--verbose", "--http3", 0};
  n = run(a3, ids, vals, 16);
  expect(n == 4);
  expect(ids[0] == O_COUNT && !strcmp(vals[0], "9"));
  expect(ids[1] == O_OUT && !strcmp(vals[1], "f.bin"));
  expect(ids[2] == O_VERB && ids[3] == O_LONGONLY);

  test("-- ends options; lone - is positional");
  char *a4[] = {"prog", "--", "-v", "-", 0};
  n = run(a4, ids, vals, 16);
  expect(n == 2);
  expect(ids[0] == OPT_POS && !strcmp(vals[0], "-v"));
  expect(ids[1] == OPT_POS && !strcmp(vals[1], "-"));
  char *a5[] = {"prog", "-", "-v", 0};
  n = run(a5, ids, vals, 16);
  expect(n == 2 && ids[0] == OPT_POS && ids[1] == O_VERB);

  test("errors: unknown, missing value, value on a flag");
  char *a6[] = {"prog", "-z", 0};
  n = run(a6, ids, vals, 16);
  expect(n == 1 && ids[0] == OPT_BAD && !strcmp(vals[0], "-z"));
  char *a7[] = {"prog", "-c", 0};
  n = run(a7, ids, vals, 16);
  expect(n == 1 && ids[0] == OPT_BAD);
  char *a8[] = {"prog", "--count", 0};
  n = run(a8, ids, vals, 16);
  expect(n == 1 && ids[0] == OPT_BAD && !strcmp(vals[0], "--count"));
  char *a9[] = {"prog", "--verbose=1", 0};
  n = run(a9, ids, vals, 16);
  expect(n == 1 && ids[0] == OPT_BAD);
  char *a10[] = {"prog", "--cou", 0};
  n = run(a10, ids, vals, 16);
  expect(n == 1 && ids[0] == OPT_BAD);

  test("opt_uint is strict");
  unsigned v = 0;
  expect(opt_uint("0", &v) == 0 && v == 0);
  expect(opt_uint("4294967295", &v) == 0 && v == 4294967295u);
  expect(opt_uint("4294967296", &v) == -1);
  expect(opt_uint("", &v) == -1);
  expect(opt_uint("12a", &v) == -1);
  expect(opt_uint("-1", &v) == -1);
  return 0;
}
