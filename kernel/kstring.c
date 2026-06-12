/* kstring.c — see kstring.h. */
#include "kstring.h"

void kmemcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
}

/* gcc lowers struct/array assignments (e.g. in fat.c) to implicit memcpy calls
 * even under -ffreestanding, so the freestanding kernel must provide one. */
void *memcpy(void *dst, const void *src, uint32_t n)
{
	kmemcpy(dst, src, n);
	return dst;
}
