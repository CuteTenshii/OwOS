/* pk.h — signature verification primitives shared by X.509 and TLS: RSA and ECDSA. */
#ifndef OWOS_PKI_PK_H
#define OWOS_PKI_PK_H

#include "crypto.h"
#include <stdint.h>

enum pk_hash { PK_SHA256, PK_SHA384, PK_SHA512 };

#define PK_HASH_MAX SHA512_DIGEST

static inline uint32_t pk_hash_len(int alg) { return alg == PK_SHA256 ? 32 : alg == PK_SHA384 ? 48 : 64; }

/* Digest data[0..len) into out (pk_hash_len(alg) bytes). */
static inline void pk_hash(int alg, const void *data, uint32_t len, uint8_t out[PK_HASH_MAX])
{
  if (alg == PK_SHA256)
    sha256(data, len, out);
  else if (alg == PK_SHA384)
    sha384(data, len, out);
  else
    sha512(data, len, out);
}

/* RSA public-key signature checks over a digest. n and e are big-endian. 0 = valid, -1 = invalid. */
int rsa_verify_pkcs1(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen, int alg, const uint8_t *digest,
                     const uint8_t *sig, uint32_t siglen);
/* RSASSA-PSS with MGF1 over the same hash and salt length equal to the digest (the TLS 1.3 form). */
int rsa_verify_pss(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen, int alg, const uint8_t *digest,
                   const uint8_t *sig, uint32_t siglen);

enum pk_curve { PK_P256, PK_P384 };

/* ECDSA over a NIST curve. pub is the uncompressed point 04||X||Y; sig is the DER SEQUENCE of r and s.
 * The digest is truncated to the curve size. 0 = valid, -1 = invalid. */
int ecdsa_verify_der(int curve, const uint8_t *pub, uint32_t publen, const uint8_t *digest, uint32_t dlen,
                     const uint8_t *sig, uint32_t siglen);

#endif /* OWOS_PKI_PK_H */
