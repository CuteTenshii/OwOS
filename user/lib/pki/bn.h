/* bn.h — fixed-size modular arithmetic for public-key verification (RSA, ECDSA).
 * Little-endian 32-bit limbs, Montgomery multiplication. Operands here are public
 * (certificates, signatures), so nothing is constant-time. */
#ifndef OWOS_PKI_BN_H
#define OWOS_PKI_BN_H

#include <stdint.h>

#define BN_MAX_LIMBS 128 /* 4096 bits */

struct bn_mont {
  uint32_t len;              /* limbs in the modulus */
  uint32_t n[BN_MAX_LIMBS];  /* modulus */
  uint32_t rr[BN_MAX_LIMBS]; /* R^2 mod n, R = 2^(32*len) */
  uint32_t n0;               /* -n^-1 mod 2^32 */
};

/* Set up arithmetic modulo the big-endian integer mod[0..len). Leading zero bytes are ignored.
 * Fails (-1) for an even or tiny modulus, or one over 4096 bits. */
int bn_mont_init(struct bn_mont *m, const uint8_t *mod, uint32_t len);

/* Big-endian bytes <-> m->len limbs. from_be fails (-1) if the value does not fit. to_be right-aligns
 * into out[0..len) and fails if it does not fit. */
int bn_from_be(const struct bn_mont *m, uint32_t *x, const uint8_t *be, uint32_t len);
int bn_to_be(const struct bn_mont *m, uint8_t *out, uint32_t len, const uint32_t *x);

int bn_cmp(const struct bn_mont *m, const uint32_t *a, const uint32_t *b);
int bn_is_zero(const struct bn_mont *m, const uint32_t *a);

/* r = a*b/R mod n. r may alias a or b. */
void bn_mul(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
/* r = a +/- b mod n for a, b < n. r may alias. */
void bn_add(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
void bn_sub(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);

/* Into / out of the Montgomery domain (x < n). */
void bn_to_mont(const struct bn_mont *m, uint32_t *r, const uint32_t *x);
void bn_from_mont(const struct bn_mont *m, uint32_t *r, const uint32_t *x);

/* r = a^e in the Montgomery domain; e is elen little-endian limbs. */
void bn_pow(const struct bn_mont *m, uint32_t *r, const uint32_t *a, const uint32_t *e, uint32_t elen);

/* r = a^-1 mod n in the Montgomery domain, for prime n. */
void bn_inv(const struct bn_mont *m, uint32_t *r, const uint32_t *a);

#endif /* OWOS_PKI_BN_H */
