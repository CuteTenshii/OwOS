/* owos.h — host test shim (scripts/). NOT the real kernel/user owos.h.
 *
 * The real user/lib/owos.h is a freestanding libc for the -m32 guest and can't
 * compile on the host. httpcache.c and headers.c pull only a handful of decls
 * from it; this shim supplies exactly those over the host libc so the pure
 * caching-policy code can be unit-tested natively (see httpcache_test.c). */
#ifndef OWOS_HOST_TEST_SHIM_H
#define OWOS_HOST_TEST_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Case-insensitive ASCII helpers, matching user/lib/owos.h's inline versions. */
static inline char lower(char c)
{ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int starts_ci(const char *s, const char *pre)
{ while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; } return 1; }
static inline int streq_ci(const char *a, const char *b)
{ while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) == lower(*b); }

#endif /* OWOS_HOST_TEST_SHIM_H */
