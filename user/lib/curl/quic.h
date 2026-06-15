/* quic.h — a minimal QUIC v1 client (RFC 9000/9001) for libcurl's HTTP/3.
 *
 * Runs entirely in user space over a SK_UDP socket (net_connect/net_send/net_recv):
 * QUIC packet protection (AES-128-GCM payload + AES-ECB header protection), a
 * TLS 1.3 handshake carried in CRYPTO frames (the same key schedule the kernel
 * TLS stack uses, mirrored here over user/lib/crypto), and the frame types a
 * one-request client needs (CRYPTO/ACK/STREAM/PADDING/CONNECTION_CLOSE/…).
 *
 * Scope, on purpose (matching the rest of OwOS's pragmatic net stack — see the
 * TCP notes in CLAUDE.md): one connection at a time (file-static state, like
 * tls.c), TLS_AES_128_GCM_SHA256 + x25519 only, the server certificate is NOT
 * verified, in-order/lossless delivery is assumed (true over SLIRP/QEMU; a real
 * packet gap truncates rather than retransmits), and only client-initiated
 * bidirectional streams (the request) are buffered for reading — server uni
 * streams (HTTP/3 control + QPACK) are consumed and discarded.
 *
 * The HTTP/3 + QPACK layer (h3.c) sits on top and drives this API. */
#ifndef OWOS_LIBQUIC_H
#define OWOS_LIBQUIC_H

#include <stdint.h>

/* Negative results mirror http.h's HTTP_E* spirit. */
#define QUIC_OK        0
#define QUIC_ESOCK   (-1)   /* no NIC / no socket / send failed         */
#define QUIC_ERESOLVE (-2)  /* DNS resolution failed                    */
#define QUIC_EHANDSHAKE (-3)/* QUIC/TLS handshake failed                */
#define QUIC_EINTR   (-4)   /* Ctrl+C arrived mid-transfer              */
#define QUIC_ECLOSED (-5)   /* peer closed the connection / fatal error */

/* Open a QUIC connection to host:port and complete the TLS 1.3 handshake,
 * offering ALPN `alpn` (e.g. "h3"). Returns QUIC_OK or a negative QUIC_E*. */
int quic_connect(const char *host, uint16_t port, const char *alpn);

/* Send `len` bytes as STREAM frame(s) on stream `sid` (fin!=0 closes our send
 * side of the stream). Splits across 1-RTT packets as needed. 0 / -1. */
int quic_send(uint64_t sid, const void *data, uint32_t len, int fin);

/* Read up to `max` bytes of received, in-order data from client-bidi stream
 * `sid` into buf. Returns the byte count (0 if nothing buffered right now). If
 * *fin is non-NULL it is set to 1 once the peer's FIN for this stream has been
 * reached and all data drained. */
int quic_read(uint64_t sid, void *buf, uint32_t max, int *fin);

/* Pump the connection once: read a single inbound datagram (blocking up to the
 * socket's ~2s timeout), process its packets, and emit any pending ACKs.
 * Returns 1 if a datagram was processed, 0 on idle timeout (none arrived),
 * QUIC_EINTR on Ctrl+C, or QUIC_ECLOSED on a CONNECTION_CLOSE / fatal error. */
int quic_poll(void);

/* Send a CONNECTION_CLOSE (if the handshake completed) and drop the socket. */
void quic_close(void);

#endif /* OWOS_LIBQUIC_H */
