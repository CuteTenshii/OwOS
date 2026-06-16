/* kstring.h — freestanding string/memory primitives shared across the kernel.
 *
 * Standard C library names on purpose: the compiler emits implicit calls to
 * memcpy for struct/array copies even under -ffreestanding, and these read like
 * normal C. size_t is uint32_t on -m32, so the uint32_t lengths/returns match
 * the builtins with no signature mismatch. */
#ifndef OWOS_KSTRING_H
#define OWOS_KSTRING_H

#include <stdint.h>

void    *memcpy(void *dst, const void *src, uint32_t n);
void    *memmove(void *dst, const void *src, uint32_t n);  /* handles overlap */
void    *memset(void *dst, int c, uint32_t n);
int      memcmp(const void *a, const void *b, uint32_t n);
uint32_t strlen(const char *s);   /* byte count before the terminating NUL */
int      strcmp(const char *a, const char *b);  /* <0 / 0 / >0, like libc */
uint32_t strlcpy(char *dst, const char *src, uint32_t size);  /* bounded copy (BSD), ret strlen(src) */

/* The codebase's idiom for "are these two strings equal?" — built on strcmp so
 * call sites read `if (streq(a, b))` instead of `if (!strcmp(a, b))`. */
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

#endif /* OWOS_KSTRING_H */
