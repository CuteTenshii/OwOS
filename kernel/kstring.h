/* kstring.h — freestanding string/memory primitives shared across the kernel.
 *
 * Standard C library names on purpose: the compiler emits implicit calls to
 * memcpy for struct/array copies even under -ffreestanding, and these read like
 * normal C. size_t is uint32_t on -m32, so the uint32_t lengths/returns match
 * the builtins with no signature mismatch. */
#ifndef OWOS_KSTRING_H
#define OWOS_KSTRING_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, uint32_t n);
void *memmove(void *dst, const void *src, uint32_t n); /* handles overlap */
void *memset(void *dst, int c, uint32_t n);
int memcmp(const void *a, const void *b, uint32_t n);
uint32_t strlen(const char *s);                              /* byte count before the terminating NUL */
int strcmp(const char *a, const char *b);                    /* <0 / 0 / >0, like libc */
uint32_t strlcpy(char *dst, const char *src, uint32_t size); /* bounded copy (BSD), ret strlen(src) */

/* The codebase's idiom for "are these two strings equal?" — built on strcmp so
 * call sites read `if (streq(a, b))` instead of `if (!strcmp(a, b))`. */
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static inline char k_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int k_streq_ci(const char *a, const char *b)
{
  while (*a && k_lower(*a) == k_lower(*b)) {
    a++;
    b++;
  }
  return k_lower(*a) == k_lower(*b);
}
static inline bool k_eq_ci_n(const char *a, const char *b, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (k_lower(a[i]) != k_lower(b[i]))
      return false;
  return true;
}

/* --------------------------------------------------------------- formatting */

/* A minimal freestanding printf, shared by ksnprintf and con_printf. It writes
 * one byte at a time through `sink` so callers needn't size a buffer. Supported
 * conversions: %s (NULL -> "(null)"), %c, %d/%i, %u, %x, %X, %p, %%; with an
 * optional '0' flag and a decimal field width (e.g. %08x, %2u). No floats, no
 * length modifiers — everything here is 32-bit. */
void kvformat(void (*sink)(char c, void *ctx), void *ctx, const char *fmt, va_list ap);

/* Format into a fixed buffer (always NUL-terminated when size > 0). Returns the
 * number of characters that would have been written had the buffer been large
 * enough (C99 snprintf semantics), so truncation is detectable.
 *
 * No __attribute__((format(printf))): %u/%x/%X here consume a uint32_t, which
 * this toolchain types as `unsigned long`, so the standard printf archetype
 * would flag every 32-bit arg as a (harmless) int-vs-long rank mismatch. */
int ksnprintf(char *buf, uint32_t size, const char *fmt, ...);
int kvsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);

#endif /* OWOS_KSTRING_H */
