/* x509.h — X.509 certificate parsing and chain verification (RFC 5280) for the TLS client.
 *
 * Parsed certificates point into the caller's DER buffer. Supported: RSA (PKCS#1 v1.5), ECDSA P-256/P-384
 * and Ed25519 signatures; subjectAltName host matching with single-label wildcards. Not supported: name
 * constraints, CRL/OCSP revocation, SHA-1 or RSASSA-PSS certificate signatures. */
#ifndef OWOS_PKI_X509_H
#define OWOS_PKI_X509_H

#include <stdint.h>

enum {
  X509_OK = 0,
  X509_E_PARSE,       /* malformed certificate */
  X509_E_UNSUPPORTED, /* algorithm or critical extension we do not implement */
  X509_E_WEAK,        /* RSA key under 2048 bits */
  X509_E_UNTRUSTED,   /* no path to a trusted root (or no trust store) */
  X509_E_SIG,         /* a signature in the chain does not verify */
  X509_E_EXPIRED,
  X509_E_NOT_YET,
  X509_E_NOT_CA,   /* an issuer is not allowed to sign certificates */
  X509_E_HOSTNAME, /* leaf does not cover the requested name */
  X509_E_PURPOSE,  /* leaf is not valid for TLS server authentication */
};

const char *x509_strerror(int err);

enum { X509_KEY_NONE, X509_KEY_RSA, X509_KEY_P256, X509_KEY_P384, X509_KEY_ED25519 };

enum {
  X509_SIG_NONE,
  X509_SIG_RSA_SHA256,
  X509_SIG_RSA_SHA384,
  X509_SIG_RSA_SHA512,
  X509_SIG_ECDSA_SHA256,
  X509_SIG_ECDSA_SHA384,
  X509_SIG_ECDSA_SHA512,
  X509_SIG_ED25519,
};

struct x509 {
  const uint8_t *der;
  uint32_t derlen;
  const uint8_t *tbs; /* signed bytes (whole TBSCertificate TLV) */
  uint32_t tbslen;
  const uint8_t *sig; /* signature bits, unused-bits byte stripped */
  uint32_t siglen;
  int sig_alg;
  const uint8_t *issuer; /* whole Name TLVs, compared bytewise */
  uint32_t issuerlen;
  const uint8_t *subject;
  uint32_t subjectlen;
  int64_t not_before, not_after; /* Unix seconds */
  int key_type;
  const uint8_t *key; /* EC: 04||X||Y, Ed25519: 32 bytes, RSA: modulus (key) and exponent (rsa_e) */
  uint32_t keylen;
  const uint8_t *rsa_e;
  uint32_t rsa_elen;
  int is_ca;        /* basicConstraints cA */
  int pathlen;      /* basicConstraints pathLenConstraint, -1 if absent */
  int has_ku;       /* keyUsage present */
  int ku_cert_sign; /* keyCertSign bit */
  int has_eku;
  int eku_server_auth; /* serverAuth or anyExtendedKeyUsage */
  const uint8_t *san;  /* GeneralNames contents */
  uint32_t sanlen;
  int unsupported_critical; /* a critical extension we do not understand */
};

/* Parse one DER certificate. Returns X509_OK or an X509_E_* code. */
int x509_parse(struct x509 *c, const uint8_t *der, uint32_t len);

/* Trusted roots, decoded from a PEM bundle. */
struct x509_store {
  struct x509 *certs;
  uint32_t count;
  uint8_t *der; /* backing storage for every certs[i].der */
};

/* Decode every CERTIFICATE block in pem[0..len) into s. Blocks that fail to parse are skipped.
 * Returns the number of roots loaded (0 leaves s empty). */
uint32_t x509_store_load_pem(struct x509_store *s, const uint8_t *pem, uint32_t len);
void x509_store_free(struct x509_store *s);

/* Verify the server chain certs[0..n) (leaf first) for `host` at time `now` (Unix seconds; 0 skips the
 * date checks, for a clock that is not set). A path to a root in `store` is required. */
int x509_verify_chain(const struct x509_store *store, const struct x509 *certs, uint32_t n, const char *host,
                      int64_t now);

/* Does `issuer`'s key verify c's signature? X509_OK, X509_E_SIG, X509_E_WEAK or X509_E_UNSUPPORTED. */
int x509_check_signature(const struct x509 *c, const struct x509 *issuer);

/* Verify a TLS handshake signature (SignatureScheme `scheme`) over msg with the leaf's key. 0 = valid. */
int x509_verify_tls_sig(const struct x509 *leaf, uint16_t scheme, const uint8_t *msg, uint32_t msglen,
                        const uint8_t *sig, uint32_t siglen);

#endif /* OWOS_PKI_X509_H */
