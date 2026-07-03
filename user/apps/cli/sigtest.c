/* sigtest.c — TEMPORARY signal-subsystem smoke test; remove after verifying. */

#include "owos.h"

static volatile int got;
static void onsig(int s) { got = s; printf("  handler ran for signal %d\n", s); }

int main(void)
{
	printf("sigtest: pid=%d\n", getpid());

	/* 1. Catch a self-raised signal: handler runs, then we resume here. */
	signal(SIGUSR1, onsig);
	got = 0;
	raise(SIGUSR1);
	printf("1) after raise(SIGUSR1): got=%d (expect 10)\n", got);

	/* 2. Block, raise (stays pending), unblock -> delivered on the unblock. */
	signal(SIGUSR2, onsig);
	got = 0;
	unsigned m = sigmask(SIGUSR2), old;
	sigprocmask(SIG_BLOCK, &m, &old);
	raise(SIGUSR2);
	printf("2a) while blocked: got=%d (expect 0)\n", got);
	sigprocmask(SIG_UNBLOCK, &m, &old);
	printf("2b) after unblock: got=%d (expect 12)\n", got);

	/* 3. Ignore -> nothing happens. */
	signal(SIGINT, SIG_IGN);
	raise(SIGINT);
	printf("3) ignored SIGINT, still alive\n");

	/* 4. Default action = terminate. Must NOT return; the parent sees exit 143. */
	printf("4) raising SIGTERM (default terminate); should not return...\n");
	raise(SIGTERM);
	printf("BUG: survived a default-terminate SIGTERM\n");
	return 0;
}
