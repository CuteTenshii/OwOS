/* upper.c — reads stdin (fd 0), upper-cases it, writes stdout (fd 1). In a
 * pipeline its stdin is the previous stage's output. */

#include "owos.h"

int main(void)
{
	char buf[64];
	int n;
	while ((n = read(0, buf, sizeof(buf))) > 0) {
		for (int i = 0; i < n; i++)
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] = (char)(buf[i] - 32);
		write(1, buf, n);
	}
	return 0;
}
