#ifndef OWOS_TEST_H
#define OWOS_TEST_H

struct owos_test_case {
	const char *name;
	int (*run)(void);
	struct owos_test_case *next;
};

void owos_test_register(struct owos_test_case *test);
void owos_test_section(const char *name);
void owos_expect(int condition, const char *file, int line, const char *expr);
void owos_expect_string(const char *got, const char *want, const char *file, int line, const char *expr);
/* Run fn(id, workers, ctx) in `workers` forked processes (<= 0: one per CPU) and wait for all. A worker that
 * fails an expectation or crashes fails the calling suite. Results must come back through the exit status or
 * shared memory (mmap MAP_SHARED), since each worker has its own copy of every global. */
void owos_parallel(int workers, void (*fn)(int id, int workers, void *ctx), void *ctx);
void owos_expect_int(long long got, long long want, const char *file, int line, const char *expr);

#define expect(condition) \
	owos_expect((condition), __FILE__, __LINE__, #condition)
#define test(name) owos_test_section(name)
#define expect_string(got, want) \
	owos_expect_string((got), (want), __FILE__, __LINE__, #got " == " #want)
/* Integer equality that reports both values on failure. */
#define expect_eq(got, want) \
	owos_expect_int((long long)(got), (long long)(want), __FILE__, __LINE__, #got " == " #want)

#ifdef OWOS_TEST_NAME
#define OWOS_JOIN_(a, b) a##b
#define OWOS_JOIN(a, b) OWOS_JOIN_(a, b)
#define OWOS_STRING_(a) #a
#define OWOS_STRING(a) OWOS_STRING_(a)
#define OWOS_TEST_ENTRY OWOS_JOIN(owos_main_, OWOS_TEST_NAME)

int OWOS_TEST_ENTRY(void);

static struct owos_test_case OWOS_JOIN(owos_case_, OWOS_TEST_NAME) = {
	OWOS_STRING(OWOS_TEST_NAME), OWOS_TEST_ENTRY, 0
};

static void OWOS_JOIN(owos_register_, OWOS_TEST_NAME)(void) __attribute__((constructor));
static void OWOS_JOIN(owos_register_, OWOS_TEST_NAME)(void)
{
	owos_test_register(&OWOS_JOIN(owos_case_, OWOS_TEST_NAME));
}
#endif

#endif
