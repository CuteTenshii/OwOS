/* crypto.h — the minimal primitives a TLS 1.2 client needs.
 *
 * Just enough for one cipher suite, TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
 *   - SHA-256 + HMAC-SHA256 + the TLS 1.2 PRF (P_SHA256) for the key schedule
 *   - AES-128 in GCM for record protection
 *   - X25519 for the ephemeral ECDH key agreement
 *
 * All freestanding, no allocation, constant-stack. See crypto.c.
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

/* ------------------------------------------------------------------ SHA-512 */
/* One-shot only (all the kernel needs: Ed25519 in ed25519.c hashes whole
 * messages). RFC 6234 / FIPS 180-4. */

#define SHA512_DIGEST 64

void sha512(const void *data, uint32_t len, uint8_t out[SHA512_DIGEST]);

/* -------------------------------------------------------------------- SHA-1 */

#define SHA1_BLOCK 64
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

/* ------------------------------------------------------------- HMAC + PRF */

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const uint8_t *msg, uint32_t msglen,
                 uint8_t out[SHA256_DIGEST]);

/* TLS 1.2 PRF (RFC 5246 §5), instantiated with P_SHA256. Expands
 * secret+label+seed into `outlen` bytes. label/seed are concatenated as the
 * PRF seed. */
void tls12_prf(const uint8_t *secret, uint32_t secretlen,
               const char *label,
               const uint8_t *seed, uint32_t seedlen,
               uint8_t *out, uint32_t outlen);

/* ------------------------------------------------------------------- HKDF */
/* HKDF-SHA256 (RFC 5869), the TLS 1.3 key-derivation core. Extract compresses
 * input keying material to a 32-byte pseudorandom key; Expand stretches it. */
void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                  const uint8_t *ikm, uint32_t ikmlen,
                  uint8_t prk[SHA256_DIGEST]);
void hkdf_expand(const uint8_t prk[SHA256_DIGEST],
                 const uint8_t *info, uint32_t infolen,
                 uint8_t *out, uint32_t outlen);

/* ------------------------------------------------------------- AES-128-GCM */

struct aes128 {
	uint32_t rk[44];           /* 11 round keys, 4 words each */
};

void aes128_init(struct aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const struct aes128 *a, const uint8_t in[16],
                          uint8_t out[16]);

/* AES-128-GCM AEAD. iv is the 12-byte nonce. On seal, `tag` (16 bytes) is
 * produced; on open it is verified and the call returns 0 on success, -1 on
 * authentication failure. aad may be NULL when aadlen is 0. in/out may alias. */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out,
                     uint8_t tag[16]);
int  aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out,
                     const uint8_t tag[16]);

/* ------------------------------------------------------------------ X25519 */

/* Compute the 32-byte shared point: out = scalar * point. `point` is a peer
 * public key (or the base point 9 to derive our own public key). RFC 7748. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* ------------------------------------------------------------- self-test */

/* Runs known-answer tests for every primitive; prints PASS/FAIL per group and
 * returns 0 iff all passed. Wired to the `cryptotest` shell command. */
int crypto_selftest(void);

#endif /* OWOS_CRYPTO_H */
