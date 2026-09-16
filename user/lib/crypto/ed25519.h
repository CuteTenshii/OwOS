/* ed25519.h — Ed25519 signatures (+ SHA-512), user-space port of
 * kernel/ed25519. TweetNaCl-derived (public domain), trimmed to sign/verify.
 * RFC 8032. Backs a user-space SSH server host key. */
#ifndef OWOS_USER_ED25519_H
#define OWOS_USER_ED25519_H

#include <stdint.h>

/* Derive the 32-byte public key from a 32-byte private seed. */
void ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]);

/* Detached signature (64 bytes) over the message m[0..n) using the private
 * `seed` and its `pub` (as from ed25519_pubkey). n is expected small (the SSH
 * exchange hash is 32 bytes). */
void ed25519_sign(uint8_t sig[64], const uint8_t *m, uint32_t n, const uint8_t seed[32], const uint8_t pub[32]);

/* Verify a detached signature. 0 = valid, -1 = invalid. */
int ed25519_verify(const uint8_t sig[64], const uint8_t *m, uint32_t n, const uint8_t pub[32]);

#endif /* OWOS_USER_ED25519_H */
