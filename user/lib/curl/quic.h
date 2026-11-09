/* quic.h — a minimal QUIC v1 client (RFC 9000/9001) for libcurl's HTTP/3.
 *
 * Runs entirely in user space over a SK_UDP socket
 * (net_connect/net_send/net_recv): QUIC packet protection (AES-128-GCM payload
 * + AES-ECB header protection), a TLS 1.3 handshake carried in CRYPTO frames
 * (the same key schedule the kernel TLS stack uses, mirrored here over
 * user/lib/crypto), and the frame types a request/response client needs
 * (CRYPTO/ACK/STREAM/MAX_DATA/PADDING/CONNECTION_CLOSE/…).
 *
 * Scope, on purpose (matching the rest of OwOS's pragmatic net stack): each
 * connection is a heap object, so several can run at once and one can carry
 * many requests. TLS_AES_128_GCM_SHA256 + x25519 only, the server certificate
 * is NOT verified, in-order/lossless delivery is assumed (true over SLIRP/QEMU;
 * a real packet gap truncates rather than retransmits), and only
 * client-initiated bidirectional streams are buffered for reading — server uni
 * streams (HTTP/3 control + QPACK) are consumed and discarded. Flow control: we
 * keep the server's connection window topped up; what we send obeys the limits
 * from its transport parameters.
 *
 * The HTTP/3 + QPACK layer (h3.c) sits on top and drives this API. */
#ifndef OWOS_LIBQUIC_H
#define OWOS_LIBQUIC_H

#include <stdint.h>

/* Negative results mirror http.h's HTTP_E* spirit. */
#define QUIC_OK 0
#define QUIC_ESOCK (-1)      /* no NIC / no socket / send failed         */
#define QUIC_ERESOLVE (-2)   /* DNS resolution failed                    */
#define QUIC_EHANDSHAKE (-3) /* QUIC/TLS handshake failed                */
#define QUIC_EINTR (-4)      /* Ctrl+C arrived mid-transfer              */
#define QUIC_ECLOSED (-5)    /* peer closed the connection / fatal error */
#define QUIC_ECERT (-6)      /* the server certificate did not check out  */

typedef struct quic quic;

/* Open a QUIC connection to host:port and complete the TLS 1.3 handshake,
 * offering ALPN `alpn` (e.g. "h3"). On QUIC_OK *out is the connection;
 * otherwise a negative QUIC_E*. */
int quic_connect(quic **out, const char *host, uint16_t port, const char *alpn);

/* The next client-initiated bidirectional stream id, or -1 if the server's
 * stream limit is used up. */
int quic_open(quic *q, uint64_t *sid);

/* Send `len` bytes as STREAM frame(s) on stream `sid` (fin!=0 closes our send
 * side of the stream). Splits across 1-RTT packets as needed. 0, -1 on a send
 * failure, or -2 if it would exceed what the server allows us to send. */
int quic_send(quic *q, uint64_t sid, const void *data, uint32_t len, int fin);

/* Read up to `max` bytes of received, in-order data from client-bidi stream
 * `sid` into buf. Returns the byte count (0 if nothing buffered right now). If
 * *fin is non-NULL it is set to 1 once the peer's FIN for this stream has been
 * reached and all data drained. */
int quic_read(quic *q, uint64_t sid, void *buf, uint32_t max, int *fin);

/* Forget a finished stream so its slot can be reused. */
void quic_stream_done(quic *q, uint64_t sid);

/* Pump the connection once: read a single inbound datagram (blocking up to the
 * socket's ~2s timeout), process its packets, and emit any pending ACKs.
 * Returns 1 if a datagram was processed, 0 on idle timeout (none arrived),
 * QUIC_EINTR on Ctrl+C, or QUIC_ECLOSED on a CONNECTION_CLOSE / fatal error. */
int quic_poll(quic *q);

/* Process whatever datagrams are already queued, without waiting; 1 if the
 * peer has closed the connection. For an idle connection about to be reused. */
int quic_peer_closed(quic *q);

/* How many more request streams the server lets us open. */
int quic_streams_left(const quic *q);

/* The idle timeout the server announced in ms, 0 if none. */
unsigned quic_idle_ms(const quic *q);

/* Send a CONNECTION_CLOSE (if the handshake completed), drop the socket and free the connection. */
void quic_close(quic *q);

#endif /* OWOS_LIBQUIC_H */
