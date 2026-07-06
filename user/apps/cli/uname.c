/* uname.c — print system identity (POSIX uname).
 *
 *   uname [-asnrvmo]
 *
 * With no flag, prints the kernel name (-s). Fields come from uname(2); the
 * machine arch is kernel-authoritative (this program is 32-bit ELF and can't
 * read the kernel's arch from its own build). */
#include "owos.h"

static void usage(void)
{
	puts("usage: uname [-asnrvmo]\n");
}

static void emit(const char *s, int *first)
{
	if (!*first) write(1, " ", 1);
	write(1, s, (int)strlen(s));
	*first = 0;
}

int main(int argc, char **argv)
{
	struct utsname u;
	int s = 0, n = 0, r = 0, v = 0, m = 0, o = 0, any = 0, first = 1;
	int i, j;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1]) { printf("uname: invalid operand '%s'\n", a); return 1; }
		for (j = 1; a[j]; j++) {
			switch (a[j]) {
			case 'a': s = n = r = v = m = o = 1; break;
			case 's': s = 1; break;
			case 'n': n = 1; break;
			case 'r': r = 1; break;
			case 'v': v = 1; break;
			case 'm': m = 1; break;
			case 'o': o = 1; break;
			case 'h': usage(); return 0;
			default:  printf("uname: invalid option -- '%c'\n", a[j]); usage(); return 1;
			}
		}
		any = 1;
	}

	if (uname(&u) != 0) { puts("uname: cannot get system identity\n"); return 1; }
	if (!any) s = 1;                 /* default: kernel name */

	if (s) emit(u.sysname,  &first);
	if (n) emit(u.nodename, &first);
	if (r) emit(u.release,  &first);
	if (v) emit(u.version,  &first);
	if (m) emit(u.machine,  &first);
	if (o) emit("OwOS",     &first); /* operating system */
	write(1, "\n", 1);
	return 0;
}
