/* tls.h — a minimal TLS client (one connection), TLS 1.3 with 1.2 fallback.
 *
 * One ClientHello offers both versions; the server's choice decides the path:
 *   - TLS 1.3: TLS_AES_128_GCM_SHA256, x25519 key share, HKDF key schedule
 *   - TLS 1.2: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, x25519 ECDHE
 * Both run over the existing TCP client and mirror the tcp.h byte-stream
 * contract so curl can layer HTTP on top of either transport.
 *
 * SECURITY: the server certificate and its signature are NOT verified — this
 * protects against passive eavesdropping but not an active man-in-the-middle.
 * Same trust posture as the plaintext path, just with the wire encrypted.
 * Documented, deliberate; the natural next step is X.509 + RSA/ECDSA verify.
 */
#ifndef OWOS_TLS_H
#define OWOS_TLS_H

#include <stdint.h>

/* Open a TCP connection to ip:port and run the TLS handshake. `sni` is the
 * server hostname for the SNI extension (may be NULL). Returns 1 on a fully
 * established, key-confirmed session, 0 on any failure. */
int  tls_connect(const uint8_t ip[4], uint16_t port, const char *sni);

/* Application-data I/O over the established session. Same shape as tcp_*. */
int  tls_write(const void *data, uint32_t len);
int  tls_read(uint8_t *buf, uint32_t max, int *closed);
void tls_close(void);

#endif /* OWOS_TLS_H */
