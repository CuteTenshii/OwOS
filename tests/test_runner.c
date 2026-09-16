#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "test.h"

static struct owos_test_case *tests;
static int failures;
static const char *section;
static const char *sections[64];
static int section_count;

void owos_test_register(struct owos_test_case *test)
{
	test->next = tests;
	tests = test;
}

void owos_test_section(const char *name)
{
	section = name;
	if (section_count < (int)(sizeof sections / sizeof sections[0]))
		sections[section_count++] = name;
	printf("  TEST %s\n", name);
}

void owos_expect(int condition, const char *file, int line, const char *expr)
{
	if (!condition) {
		printf("  FAIL %s:%d: %s%s%s\n", file, line,
		       section ? section : "", section ? ": " : "", expr);
		failures++;
	}
}

void owos_expect_string(const char *got, const char *want, const char *file, int line, const char *expr)
{
	owos_expect(got && want && strcmp(got, want) == 0, file, line, expr);
}

int main(void)
{
	struct owos_test_case *test;
	int passed = 0;

	for (test = tests; test; test = test->next) {
		int before = failures;
		FILE *log = tmpfile();
		int saved = dup(STDOUT_FILENO);
		int result;
		section = 0;
		section_count = 0;
		printf("TEST %s\n", test->name);
		fflush(stdout);
		dup2(fileno(log), STDOUT_FILENO);
		result = test->run();
		fflush(stdout);
		dup2(saved, STDOUT_FILENO);
		close(saved);
		if (result == 0 && failures == before) {
			for (int i = 0; i < section_count; i++)
				printf("  TEST %s\n", sections[i]);
			printf("PASS %s\n", test->name);
			passed++;
		} else {
			if (result != 0 && failures == before) failures++;
			fseek(log, 0, SEEK_SET);
			while ((result = fgetc(log)) != EOF) putchar(result);
			printf("FAIL %s\n", test->name);
		}
		fclose(log);
	}

	printf("\n%d passed, %d failed\n", passed, (int)(0 + failures));
	return failures != 0 || passed == 0;
}
