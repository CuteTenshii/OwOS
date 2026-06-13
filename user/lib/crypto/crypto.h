/* crypto.h — userspace crypto lib. SHA-1 (for the torrent infohash + piece
 * verification); freestanding and allocation-free, ported from kernel/crypto.c.
 * Room to grow (SHA-256/HMAC/...) if a ring-3 TLS ever lands. */
#ifndef OWOS_USER_CRYPTO_H
#define OWOS_USER_CRYPTO_H

#include <stdint.h>

#define SHA1_BLOCK  64
#define SHA1_DIGEST 20

struct sha1 {
	uint32_t h[5];
	uint64_t total;            /* message length so far, in bytes */
	uint8_t  buf[SHA1_BLOCK];
	uint32_t buflen;
};

void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, uint32_t len);
void sha1_final(struct sha1 *s, uint8_t out[SHA1_DIGEST]);
void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST]);

#endif /* OWOS_USER_CRYPTO_H */
