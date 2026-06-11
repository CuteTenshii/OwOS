/* crashme.c — deliberately faults, to exercise the ring-3 crash reporter.
 * A few call frames deep so the crash window shows a real backtrace. */
#include "owos.h"

static void boom(void)
{
	volatile int *p = (volatile int *)0;   /* null write -> page fault */
	*p = 0xdead;
}

static void level2(void) { boom(); }
static void level1(void) { level2(); }

int main(void)
{
	puts("crashme: about to fault...\n");
	level1();
	puts("crashme: (never reached)\n");
	return 0;
}
