/* TEMP malloc self-test (will be reverted) */
#include "owos.h"

static void up(unsigned v){ char t[12]; int k=0; if(!v){puts("0");return;}
	while(v){t[k++]='0'+v%10;v/=10;} while(k--){char c=t[k];write(1,&c,1);} }

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	void *first = sbrk(0);
	up((unsigned)first); puts(" = initial brk\n");

	static void *ps[300];
	for (int i = 0; i < 300; i++) {
		unsigned n = (unsigned)((i % 700) + 1);
		ps[i] = malloc(n);
		if (!ps[i]) { puts("FAIL malloc i="); up(i); puts("\n"); return 1; }
		memset(ps[i], (i & 0xff), n);
	}
	for (int i = 0; i < 300; i++) {
		unsigned n = (unsigned)((i % 700) + 1);
		unsigned char *p = ps[i];
		for (unsigned k = 0; k < n; k++)
			if (p[k] != (i & 0xff)) { puts("FAIL pattern i="); up(i); puts("\n"); return 1; }
	}
	for (int i = 0; i < 300; i += 2) free(ps[i]);
	unsigned char *big = malloc(2*1024*1024);
	if (!big) { puts("FAIL big malloc\n"); return 1; }
	for (int k = 0; k < 2*1024*1024; k += 4096) big[k] = 0x5a;   /* touch every page */
	for (int k = 0; k < 2*1024*1024; k += 4096) if (big[k] != 0x5a) { puts("FAIL big touch\n"); return 1; }
	free(big);
	unsigned char *c = calloc(4096, 1);
	for (int i = 0; i < 4096; i++) if (c[i]) { puts("FAIL calloc\n"); return 1; }
	free(c);
	void *last = sbrk(0);
	puts("malloc PASS  brk grew by "); up((unsigned)((char*)last-(char*)first)); puts(" bytes\n");
	return 0;
}
