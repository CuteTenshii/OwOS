/* worker.c — a separate ring-3 program. `hello` replaces its image with this
 * one; it does a little work and exits with a distinctive status. */

#include "owos.h"

int main(void)
{
	puts("    [worker] now running (replaced hello's image)\n");
	for (int i = 0; i < 3; i++)
		puts("    [worker] working...\n");
	puts("    [worker] done, exiting with status 7\n");
	return 7;
}
