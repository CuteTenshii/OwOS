/* crypto.h — SHA-256, the one hash the kernel still needs.
 *
 * The only in-kernel consumer is users.c (password hashing in /etc/passwd).
 * The TLS/SSH primitives that used to live here (AES-GCM, X25519, HKDF, HMAC,
 * ChaCha20, SHA-512, Ed25519, the CSPRNG) moved to user space with the network
 * clients; see user/lib/crypto and user/lib/tls.
 *
 * Freestanding, no allocation, constant-stack. See crypto.c.
 */
#ifndef OWOS_CRYPTO_H
#define OWOS_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ SHA-256 */

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

struct sha256 {
	uint32_t h[8];
	uint64_t total;            /* message length so far, in bytes */
	uint8_t  buf[SHA256_BLOCK];
	uint32_t buflen;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, uint32_t len);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]);
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST]);

#endif /* OWOS_CRYPTO_H */
