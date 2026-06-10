/* kstring.h — freestanding memory primitives shared across the kernel.
 *
 * Deliberately *not* named memcpy(): the kernel is built -nostdlib and this
 * takes a uint32_t length, so the k-prefix keeps the compiler from mistaking it
 * for the libc memcpy() it may emit implicit calls to. */
#ifndef OWOS_KSTRING_H
#define OWOS_KSTRING_H

#include <stdint.h>

void kmemcpy(void *dst, const void *src, uint32_t n);

#endif /* OWOS_KSTRING_H */
