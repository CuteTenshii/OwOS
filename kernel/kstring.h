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
uint32_t strlen(const char *s);   /* byte count before the terminating NUL */

#endif /* OWOS_KSTRING_H */
