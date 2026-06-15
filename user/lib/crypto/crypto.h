/* crypto.h — userspace crypto lib, ported from kernel/crypto.c (freestanding,
 * allocation-free). SHA-1 backs the torrent infohash + piece verification;
 * SHA-256/HMAC/HKDF + AES-128-GCM + X25519 back the QUIC/TLS-1.3 client
 * (user/lib/curl/quic.c) — QUIC's key schedule and packet protection need the
 * same primitives the kernel TLS stack uses, but in user space. */
#ifndef OWOS_USER_CRYPTO_H
#define OWOS_USER_CRYPTO_H

#include <stdint.h>

/* -------------------------------------------------------------------- SHA-1 */

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

/* ------------------------------------------------------------------ SHA-256 */

#define SHA256_BLOCK  64
#define SHA256_DIGEST 32

struct sha256 {
	uint32_t h[8];
	uint64_t total;
	uint8_t  buf[SHA256_BLOCK];
	uint32_t buflen;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, uint32_t len);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]);
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST]);

/* --------------------------------------------------------- HMAC + HKDF */

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const uint8_t *msg, uint32_t msglen,
                 uint8_t out[SHA256_DIGEST]);

/* HKDF-SHA256 (RFC 5869), the TLS 1.3 / QUIC key-derivation core. */
void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                  const uint8_t *ikm, uint32_t ikmlen,
                  uint8_t prk[SHA256_DIGEST]);
void hkdf_expand(const uint8_t prk[SHA256_DIGEST],
                 const uint8_t *info, uint32_t infolen,
                 uint8_t *out, uint32_t outlen);

/* --------------------------------------------------------- AES-128-GCM */

struct aes128 {
	uint32_t rk[44];           /* 11 round keys, 4 words each */
};

void aes128_init(struct aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const struct aes128 *a, const uint8_t in[16],
                          uint8_t out[16]);

/* AES-128-GCM AEAD. iv is the 12-byte nonce. seal produces `tag` (16 bytes);
 * open verifies it, returning 0 on success or -1 on auth failure. aad may be
 * NULL when aadlen is 0. in/out may alias. */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out,
                     uint8_t tag[16]);
int  aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out,
                     const uint8_t tag[16]);

/* ------------------------------------------------------------------ X25519 */

/* out = scalar * point. `point` is a peer public key (or the base point 9 to
 * derive our own public key). RFC 7748. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

#endif /* OWOS_USER_CRYPTO_H */
