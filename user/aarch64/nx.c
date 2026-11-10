/* nx.c: jumps into its own data; the kernel must kill it (no execute permission). */
#include "rt.h"

static char buffer[16] = {0x1f, 0x20, 0x03, 0xd5}; /* a nop, never allowed to run */

int main(void)
{
  ((void (*)(void))buffer)();
  puts("nx: survived executing .data\n");
  return 0;
}
