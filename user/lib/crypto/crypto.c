/* crypto.c — SHA-1, ported verbatim from kernel/crypto.c (freestanding). */

#include "crypto.h"

void *memcpy(void *dst, const void *src, unsigned long n);   /* user libc */

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
		uint32_t t = rotl32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rotl32(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(struct sha1 *s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe; s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
	s->total = 0;
	s->buflen = 0;
}

void sha1_update(struct sha1 *s, const void *data, uint32_t len)
{
	const uint8_t *p = data;
	s->total += len;
	while (len) {
		uint32_t take = SHA1_BLOCK - s->buflen;
		if (take > len)
			take = len;
		memcpy(s->buf + s->buflen, p, take);
		s->buflen += take;
		p += take;
		len -= take;
		if (s->buflen == SHA1_BLOCK) {
			sha1_block(s, s->buf);
			s->buflen = 0;
		}
	}
}

void sha1_final(struct sha1 *s, uint8_t out[SHA1_DIGEST])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80;
	sha1_update(s, &pad, 1);
	uint8_t zero = 0;
	while (s->buflen != 56)
		sha1_update(s, &zero, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - i * 8));
	sha1_update(s, len, 8);
	for (int i = 0; i < 5; i++) {
		out[i * 4]     = (uint8_t)(s->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)s->h[i];
	}
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST])
{
	struct sha1 s;
	sha1_init(&s);
	sha1_update(&s, data, len);
	sha1_final(&s, out);
}
