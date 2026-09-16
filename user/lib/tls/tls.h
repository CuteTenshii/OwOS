/* tls.h — user-space TLS 1.3 client with a TLS 1.2 fallback.
 *
 * Ported from the kernel stack (kernel/tls.c) to run over the user socket API.
 * One ClientHello offers both versions; the server's choice decides the path:
 *   - TLS 1.3: TLS_AES_128_GCM_SHA256, x25519 key share, HKDF key schedule
 *   - TLS 1.2: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, x25519 ECDHE
 * The crypto primitives come from user/lib/crypto (crypto.h); transport is a
 * plain SK_TCP socket this library owns.
 *
 * SECURITY: the server certificate and its signature are NOT verified — this
 * protects against passive eavesdropping but not an active man-in-the-middle.
 * Documented, deliberate; the natural next step is X.509 + RSA/ECDSA verify.
 */
#ifndef OWOS_USER_TLS_H
#define OWOS_USER_TLS_H

typedef struct tls_conn tls_conn; /* opaque per-connection state */

/* Open a TLS client connection: resolves + TCP-connects to host:port (SK_TCP)
 * and runs the handshake with SNI=host. `offer_h2` adds "h2" to the ALPN offer
 * so the server may select HTTP/2 (read it back with tls_alpn). Returns a
 * handle or NULL on any failure. The certificate is NOT verified (see file
 * header). */
tls_conn *tls_client(const char *host, int port, int offer_h2);

/* Application-data I/O. read: >0 bytes / 0 EOF / <0 error (NET_EINTR passes
 * through as-is so the caller can tear down on Ctrl+C). write: bytes sent, or
 * <0 on error. */
int tls_read(tls_conn *c, void *buf, int max);
int tls_write(tls_conn *c, const void *buf, int len);

/* The ALPN protocol the server negotiated: ALPN_H2 / ALPN_HTTP11 / ALPN_NONE
 * (owos.h constants). Valid after a successful tls_client(). */
int tls_alpn(tls_conn *c);

void tls_close(tls_conn *c);

#endif /* OWOS_USER_TLS_H */
