/* kstring.h: freestanding string/memory primitives (libc names; the compiler emits memcpy calls). */
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

/* Equality idiom: streq(a, b) instead of !strcmp(a, b). */
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

/* Minimal printf over a byte sink: %s %c %d %i %u %x %X %p %%, optional 0 flag and width. 32-bit only. */
void kvformat(void (*sink)(char c, void *ctx), void *ctx, const char *fmt, va_list ap);

/* snprintf semantics (returns would-be length). No format attribute: uint32_t is unsigned long here. */
int ksnprintf(char *buf, uint32_t size, const char *fmt, ...);
int kvsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);

#endif /* OWOS_KSTRING_H */
