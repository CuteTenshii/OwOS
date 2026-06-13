/* kstring.c — see kstring.h. */
#include "kstring.h"

/* gcc lowers struct/array assignments (e.g. in fat.c) to implicit memcpy calls
 * even under -ffreestanding, so the freestanding kernel must provide one — with
 * the standard signature, since that's what the compiler emits calls to.
 *
 * A byte-loop memcpy is only a footgun at -O2+, where the loop-idiom pass would
 * rewrite it into a self-call; this kernel builds at -O0, so the loop lives here
 * directly rather than behind a differently-named worker. */
void *memcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

/* The other primitive gcc emits implicitly (e.g. for `struct x = {0}`). */
void *memset(void *dst, int c, uint32_t n)
{
	uint8_t *d = dst;
	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

uint32_t strlen(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}
