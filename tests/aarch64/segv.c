/* segv.c: writes to its own read-only data; the kernel must kill it. */
#include "rt.h"

static const char readonly[] = "constant";

int main(void)
{
  *(volatile char *)readonly = 1;
  puts("segv: survived a write to .rodata\n");
  return 0;
}
