/* aeabi.c: the run-time ABI memory helpers ARM compilers call for block copies and fills, on top of kstring.
 * __aeabi_memset takes its arguments in a different order from memset. */
#include "kstring.h"
#include <stddef.h>

void __aeabi_memcpy(void *dst, const void *src, size_t n);
void __aeabi_memcpy4(void *dst, const void *src, size_t n);
void __aeabi_memcpy8(void *dst, const void *src, size_t n);
void __aeabi_memmove(void *dst, const void *src, size_t n);
void __aeabi_memmove4(void *dst, const void *src, size_t n);
void __aeabi_memmove8(void *dst, const void *src, size_t n);
void __aeabi_memset(void *dst, size_t n, int c);
void __aeabi_memset4(void *dst, size_t n, int c);
void __aeabi_memset8(void *dst, size_t n, int c);
void __aeabi_memclr(void *dst, size_t n);
void __aeabi_memclr4(void *dst, size_t n);
void __aeabi_memclr8(void *dst, size_t n);

void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memmove(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove4(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove8(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memset(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memset4(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memset8(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }
