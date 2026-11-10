/* rt.c: the memory primitives the compiler calls implicitly, plus tiny output helpers. */
#include "rt.h"

void *memcpy(void *dst, const void *src, size_t n)
{
  uint8_t *d = dst;
  const uint8_t *s = src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *dst, int c, size_t n)
{
  uint8_t *d = dst;
  while (n--)
    *d++ = (uint8_t)c;
  return dst;
}

size_t strlen(const char *s)
{
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

void puts(const char *s) { write(1, s, strlen(s)); }

void put_u64(uint64_t v)
{
  char buf[21];
  int i = sizeof buf;
  do
    buf[--i] = (char)('0' + v % 10);
  while (v /= 10);
  write(1, buf + i, sizeof buf - (size_t)i);
}
