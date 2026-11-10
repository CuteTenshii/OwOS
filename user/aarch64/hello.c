/* hello.c: first real AArch64 user program; checks the loader's segments and the syscall path. */
#include "rt.h"

static int initialized = 1234;
static char zeroed[8192];
static const char banner[] = "hello from an ELF user program, pid ";

static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void)
{
  int bad = 0;

  puts(banner);
  put_u64((uint64_t)getpid());
  puts("\n");

  if (initialized != 1234)
    bad |= 1;
  for (unsigned i = 0; i < sizeof zeroed; i++)
    if (zeroed[i])
      bad |= 2;
  zeroed[sizeof zeroed - 1] = 7;
  initialized++;
  if (initialized != 1235 || zeroed[sizeof zeroed - 1] != 7)
    bad |= 4;
  if (fib(20) != 6765)
    bad |= 8;

  uint64_t t0 = msec();
  sleep_ms(30);
  if (msec() - t0 < 30)
    bad |= 16;

  puts(bad ? "hello: FAILED\n" : "hello: all checks passed\n");
  return bad;
}
