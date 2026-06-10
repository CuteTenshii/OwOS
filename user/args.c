/* args.c — print the command-line arguments the kernel handed us. */

#include "owos.h"

int main(int argc, char **argv)
{
	puts("argc = ");
	print_uint((unsigned)argc);
	puts("\n");
	for (int i = 0; i < argc; i++) {
		puts("  argv[");
		print_uint((unsigned)i);
		puts("] = \"");
		puts(argv[i]);
		puts("\"\n");
	}
	return argc;
}
