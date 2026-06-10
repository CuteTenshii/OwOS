/* kstring.c — see kstring.h. */
#include "kstring.h"

void kmemcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
}
