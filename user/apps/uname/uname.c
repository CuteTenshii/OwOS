/* uname.c - print system identity (POSIX uname). uname [-asnrvmo] With no flag, prints the
 * kernel name (-s) */
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

static void usage(void) { puts("usage: uname [-asnrvmo]\n"); }

static void emit(const char *s, bool *first)
{
  if (!*first)
    write(1, " ", 1);
  write(1, s, (int)strlen(s));
  *first = false;
}

int main(int argc, char **argv)
{
  struct utsname u;
  bool s = false, n = false, r = false, v = false, m = false, o = false, any = false, first = true;

  static const struct opt opts[] = {
      {'a', "all", OPT_FLAG, 'a'},
      {'s', "kernel-name", OPT_FLAG, 's'},
      {'n', "nodename", OPT_FLAG, 'n'},
      {'r', "kernel-release", OPT_FLAG, 'r'},
      {'v', "kernel-version", OPT_FLAG, 'v'},
      {'m', "machine", OPT_FLAG, 'm'},
      {'o', "operating-system", OPT_FLAG, 'o'},
      {'h', "help", OPT_FLAG, 'h'},
  };
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int c; (c = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (c == OPT_POS) {
      printf("uname: invalid operand '%s'\n", st.arg);
      return 1;
    }
    if (c == OPT_BAD) {
      printf("uname: invalid option '%s'\n", st.bad);
      usage();
      return 1;
    }
    if (c == 'h') {
      usage();
      return 0;
    }
    if (c == 'a')
      s = n = r = v = m = o = true;
    else if (c == 's')
      s = true;
    else if (c == 'n')
      n = true;
    else if (c == 'r')
      r = true;
    else if (c == 'v')
      v = true;
    else if (c == 'm')
      m = true;
    else
      o = true;
    any = true;
  }

  if (uname(&u) != 0) {
    puts("uname: cannot get system identity\n");
    return 1;
  }
  if (!any)
    s = true; /* default: kernel name */

  if (s)
    emit(u.sysname, &first);
  if (n)
    emit(u.nodename, &first);
  if (r)
    emit(u.release, &first);
  if (v)
    emit(u.version, &first);
  if (m)
    emit(u.machine, &first);
  if (o)
    emit("OwOS", &first); /* operating system */
  write(1, "\n", 1);
  return 0;
}
