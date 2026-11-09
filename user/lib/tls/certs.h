/* certs.h — server certificate checking for TLS 1.3 and 1.2 handshakes, shared by tls.c and the QUIC handshake
 * (which carries TLS 1.3 in CRYPTO frames). The chain is verified against the root bundle and the host name, and
 * the handshake signature against the leaf's key. */
#ifndef OWOS_TLS_CERTS_H
#define OWOS_TLS_CERTS_H

#include <stdint.h>

#include "x509.h"

#define CERTS_CHAIN_MAX 8
#define CERTS_BUF_MAX 24576u

struct certs {
  int verify; /* X509_* code; X509_OK = chain and signature good */
  uint32_t nchain;
  struct x509 chain[CERTS_CHAIN_MAX];
  uint8_t buf[CERTS_BUF_MAX]; /* the DER the chain entries point into */
};

/* Nothing seen yet: the verdict is X509_E_UNTRUSTED until a chain checks out. */
void certs_init(struct certs *c);

/* Take the server's Certificate message body (TLS 1.3 adds a request context and per-entry extensions) and verify
 * the chain against the roots and `host`. */
void certs_take(struct certs *c, const char *host, const uint8_t *body, uint32_t bl, int tls13);

/* Check the handshake signature of `scheme` over msg against the leaf's key. */
void certs_check_sig(struct certs *c, uint16_t scheme, const uint8_t *msg, uint32_t msglen, const uint8_t *sig,
                     uint32_t siglen);

/* The TLS 1.3 CertificateVerify body (scheme, length, signature) against `th`, the transcript hash through the
 * Certificate message. */
void certs_check_cv(struct certs *c, const uint8_t th[32], const uint8_t *body, uint32_t bl);

/* Record a failure, unless one is already recorded. */
void certs_fail(struct certs *c, int err);

/* NULL when the verdict is good, else a short description. */
const char *certs_message(const struct certs *c);

/* Use this store instead of the root bundle on disk (tests). */
void certs_set_roots(const struct x509_store *roots);

#endif /* OWOS_TLS_CERTS_H */
