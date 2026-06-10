/* produce.c — writes a few lines to stdout (fd 1). In a pipeline its stdout
 * is the pipe, feeding the next stage. */

#include "owos.h"

int main(void)
{
	write(1, "hello from produce\n", 19);
	write(1, "second line\n", 12);
	write(1, "third line\n", 11);
	return 0;
}
