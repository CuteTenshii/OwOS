/* hello.c — the classic fork + exec + wait. Fork a child, have the child
 * replace itself with the `worker` program, and wait for it to finish. */

#include "owos.h"

int main(void)
{
	puts("[hello] forking a child to run 'worker'...\n");

	int pid = fork();
	if (pid == 0) {
		execp("worker");                /* become the worker program          */
		puts("[hello] exec failed\n");  /* only reached if it failed          */
		exit(1);
	}

	int status = waitpid(pid);          /* block until the worker exits        */
	puts("[hello] worker (pid ");
	print_uint((unsigned)pid);
	puts(") exited with status ");
	print_uint((unsigned)status);
	puts("\n");
	return 0;
}
