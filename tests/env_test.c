/* Host test for the process environment store (kernel/env.c). */
#define OWOS_SUITE_TEST
#define OWOS_HEAP_H
#define OWOS_KSTRING_H

#include "test.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int g_live; /* allocations not yet freed */
static int g_fail_after = -1;

static void *kmalloc(uint32_t n)
{
	if (g_fail_after == 0)
		return 0;
	if (g_fail_after > 0)
		g_fail_after--;
	g_live++;
	return malloc(n);
}

static void kfree(void *p)
{
	if (p) {
		g_live--;
		free(p);
	}
}

#include "../kernel/env.c"

static bool entries_are(const struct env *e, const char *const *want, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		const char *got = env_entry(e, i);
		if (!got || strcmp(got, want[i]))
			return false;
	}
	return env_entry(e, n) == 0;
}

int main(void)
{
	struct env e = {0, 0};

	test("set, get and enumerate in insertion order");
	expect(env_get(&e, "A") == 0);
	expect(env_set(&e, "PATH", "/bin", true) == 0);
	expect(env_set(&e, "HOME", "/home/root", true) == 0);
	expect(env_set(&e, "EMPTY", "", true) == 0);
	expect_string(env_get(&e, "PATH"), "/bin");
	expect_string(env_get(&e, "HOME"), "/home/root");
	expect_string(env_get(&e, "EMPTY"), "");
	const char *want[] = {"PATH=/bin", "HOME=/home/root", "EMPTY="};
	expect(entries_are(&e, want, 3));

	test("a prefix is not a match");
	expect(env_get(&e, "PAT") == 0);
	expect(env_get(&e, "PATHX") == 0);
	expect(env_get(&e, "") == 0);

	test("overwrite replaces in place, no-overwrite keeps the old value");
	expect(env_set(&e, "HOME", "/root", true) == 0);
	expect_string(env_get(&e, "HOME"), "/root");
	expect(env_set(&e, "HOME", "/other", false) == 0);
	expect_string(env_get(&e, "HOME"), "/root");
	const char *after[] = {"PATH=/bin", "HOME=/root", "EMPTY="};
	expect(entries_are(&e, after, 3));
	expect(env_set(&e, "LONG", "a longer value than any before it", true) == 0);
	expect(env_set(&e, "PATH", "/bin:/usr/bin", true) == 0);
	expect_string(env_get(&e, "PATH"), "/bin:/usr/bin");
	expect_string(env_get(&e, "LONG"), "a longer value than any before it");

	test("names must be non-empty and free of '='");
	expect(env_set(&e, "", "x", true) == -1);
	expect(env_set(&e, "A=B", "x", true) == -1);
	expect(env_get(&e, "A") == 0);

	test("unset removes one entry and ignores unknown names");
	env_unset(&e, "HOME");
	env_unset(&e, "NOPE");
	expect(env_get(&e, "HOME") == 0);
	expect_string(env_get(&e, "EMPTY"), "");
	env_unset(&e, "PATH");
	env_unset(&e, "EMPTY");
	env_unset(&e, "LONG");
	expect(e.len == 0 && e.data == 0 && g_live == 0);

	test("copies are independent");
	expect(env_set(&e, "X", "1", true) == 0);
	struct env c = {0, 0};
	expect(env_copy(&c, &e) == 0);
	expect(env_set(&c, "X", "2", true) == 0);
	expect_string(env_get(&e, "X"), "1");
	expect_string(env_get(&c, "X"), "2");
	env_free(&c);
	env_free(&e);
	expect(g_live == 0);

	test("a block from user space keeps only well-formed entries");
	static const char block[] = "A=1\0=bad\0NOEQ\0B=two words\0\0C=cut";
	expect(env_from_block(&e, block, sizeof block - 6) == 0); /* ends before C=cut's NUL */
	const char *kept[] = {"A=1", "B=two words"};
	expect(entries_are(&e, kept, 2));
	env_free(&e);
	expect(env_from_block(&e, "", 0) == 0 && e.len == 0);

	test("allocation failure leaves the environment intact");
	expect(env_set(&e, "K", "v", true) == 0);
	g_fail_after = 0;
	expect(env_set(&e, "K", "w", true) == -1);
	expect(env_set(&e, "L", "w", true) == -1);
	g_fail_after = -1;
	expect_string(env_get(&e, "K"), "v");
	expect(env_get(&e, "L") == 0);
	env_free(&e);
	expect(g_live == 0);
	return 0;
}
