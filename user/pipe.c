/* pipe.c — the classic pipe + fork. The child writes into the pipe; the
 * parent reads from it until EOF (when the child closes its write end). */

#include "owos.h"

int main(void)
{
	int fd[2];
	if (pipe(fd) < 0) {
		puts("pipe failed\n");
		return 1;
	}

	int pid = fork();
	if (pid == 0) {
		close(fd[0]);                       /* child only writes              */
		write(fd[1], "message through the pipe!\n", 26);
		write(fd[1], "...and a second line\n", 21);
		close(fd[1]);                       /* EOF for the reader             */
		exit(0);
	}

	close(fd[1]);                           /* parent only reads              */
	puts("[pipe] parent reading until EOF:\n");
	char buf[64];
	int n;
	while ((n = read(fd[0], buf, sizeof(buf))) > 0)
		write(1, buf, n);
	close(fd[0]);
	waitpid(pid);
	puts("[pipe] done\n");
	return 0;
}
