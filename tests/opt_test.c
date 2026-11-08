/* Host test for the command-line option parser (user/lib/opt.c). */
#define OWOS_SUITE_TEST
#include "opt.h"
#include "test.h"
#include <string.h>

enum { O_VERB = 1, O_COUNT, O_OUT, O_LONGONLY };

static const struct opt tbl[] = {
    {'v', "verbose", OPT_FLAG, O_VERB, 0, "print more"},
    {'x', 0, OPT_FLAG, 'x', 0, "exclude nothing"},
    {'c', "count", OPT_ARG, O_COUNT, "N", "repeat N times"},
    {'o', "output", OPT_ARG, O_OUT},
    {0, "http3", OPT_FLAG, O_LONGONLY, 0, "use HTTP/3"},
    {0, "proxy", OPT_ARG, 0, "URL", "send requests through URL"},
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

static int ids[16];
static const char *vals[16];
static int n;

static void check_short_flags(void)
{
  char *a1[] = {"prog", "-v", "host", "-c", "5", "-ofile", "tail", 0};
  n = run(a1, ids, vals, 16);
  expect(n == 5);
  expect(ids[0] == O_VERB);
  expect(ids[1] == OPT_POS && !strcmp(vals[1], "host"));
  expect(ids[2] == O_COUNT && !strcmp(vals[2], "5"));
  expect(ids[3] == O_OUT && !strcmp(vals[3], "file"));
  expect(ids[4] == OPT_POS && !strcmp(vals[4], "tail"));
}

static void check_clustered(void)
{
  char *a2[] = {"prog", "-vxc7", 0};
  n = run(a2, ids, vals, 16);
  expect(n == 3);
  expect(ids[0] == O_VERB && ids[1] == 'x');
  expect(ids[2] == O_COUNT && !strcmp(vals[2], "7"));
}

static void check_long_options(void)
{
  char *a3[] = {"prog", "--count=9", "--output", "f.bin", "--verbose", "--http3", 0};
  n = run(a3, ids, vals, 16);
  expect(n == 4);
  expect(ids[0] == O_COUNT && !strcmp(vals[0], "9"));
  expect(ids[1] == O_OUT && !strcmp(vals[1], "f.bin"));
  expect(ids[2] == O_VERB && ids[3] == O_LONGONLY);
}

static void check_double_dash(void)
{
  char *a4[] = {"prog", "--", "-v", "-", 0};
  n = run(a4, ids, vals, 16);
  expect(n == 2);
  expect(ids[0] == OPT_POS && !strcmp(vals[0], "-v"));
  expect(ids[1] == OPT_POS && !strcmp(vals[1], "-"));
  char *a5[] = {"prog", "-", "-v", 0};
  n = run(a5, ids, vals, 16);
  expect(n == 2 && ids[0] == OPT_POS && ids[1] == O_VERB);
}

static void check_errors(void)
{
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
}

static void check_usage(void)
{
  char out[512];
  int len = opt_usage(out, sizeof out, "prog", "FILE...", tbl, OPT_N(tbl));
  expect(!strcmp(out, "usage: prog [-vx] [-c N] [-o ARG] [--http3] [--proxy URL] FILE...\n"));
  expect(len == (int)strlen(out));
}

static void check_help(void)
{
  char out[512];
  int len = opt_help(out, sizeof out, "prog", "FILE...", "Fetch FILE.", tbl, OPT_N(tbl));
  expect(!strcmp(out, "usage: prog [-vx] [-c N] [-o ARG] [--http3] [--proxy URL] FILE...\n"
                      "\n"
                      "Fetch FILE.\n"
                      "\n"
                      "options:\n"
                      "  -v, --verbose     print more\n"
                      "  -x                exclude nothing\n"
                      "  -c, --count N     repeat N times\n"
                      "  -o, --output ARG\n"
                      "  --http3           use HTTP/3\n"
                      "  --proxy URL       send requests through URL\n"));
  expect(len == (int)strlen(out));
}

static void check_truncation(void)
{
  char small[16], out[512];
  int len = opt_usage(small, sizeof small, "prog", "FILE...", tbl, OPT_N(tbl));
  expect(len > (int)sizeof small && strlen(small) == sizeof small - 1 && !strncmp(small, "usage: prog [-v", 15));
  expect(opt_usage(small, 0, "prog", 0, tbl, OPT_N(tbl)) == len - 8);
  const struct opt none[] = {{'h', 0, OPT_FLAG, 'h'}};
  opt_usage(out, sizeof out, "x", 0, none, 1);
  expect(!strcmp(out, "usage: x [-h]\n"));
  opt_help(out, sizeof out, "x", 0, 0, none, 0);
  expect(!strcmp(out, "usage: x\n"));
}

static void check_opt_uint(void)
{
  unsigned v = 0;
  expect(opt_uint("0", &v) == 0 && v == 0);
  expect(opt_uint("4294967295", &v) == 0 && v == 4294967295u);
  expect(opt_uint("4294967296", &v) == -1);
  expect(opt_uint("", &v) == -1);
  expect(opt_uint("12a", &v) == -1);
  expect(opt_uint("-1", &v) == -1);
}

int main(void)
{
  test("short flags, values and positionals interleave", check_short_flags);
  test("clustered flags end in a valued option", check_clustered);
  test("long options, =value and separate value", check_long_options);
  test("-- ends options; lone - is positional", check_double_dash);
  test("errors: unknown, missing value, value on a flag", check_errors);
  test("usage lists clustered flags, valued options and the synopsis", check_usage);
  test("help aligns descriptions and tolerates a missing one", check_help);
  test("truncation keeps the buffer terminated and reports the full length", check_truncation);
  test("opt_uint is strict", check_opt_uint);
  return 0;
}
