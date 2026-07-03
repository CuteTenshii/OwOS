/* tcp.h — a minimal TCP client supporting several concurrent connections.
 *
 * Enough to open a handful of outbound connections at once, send a request on
 * each, and read the replies until the peers close — the happy path for
 * HTTP/1.0 GETs over QEMU's (near-lossless) SLIRP NAT. No retransmission
 * beyond resending the SYN, no out-of-order reassembly (in-order segments
 * only; the peer resends the rest), no congestion control.
 *
 * Each connection is a control block (TCB) drawn from a fixed table; the API
 * is handle-based. tcp_input() demultiplexes an inbound segment to the owning
 * TCB by its 4-tuple, so connections to different peers (or the same peer on
 * different local ports) stay independent.
 */
#ifndef OWOS_TCP_H
#define OWOS_TCP_H

#include <stdint.h>

/* Opaque per-connection handle, returned by tcp_connect(). */
typedef struct tcp_conn tcp_conn_t;

/* Inbound TCP segment from the IP layer (interrupt context). Demuxed to the
 * matching connection, if any. */
void tcp_input(const uint8_t src[4], const uint8_t *seg, uint32_t len);

/* tcp_read returns this when the caller's cooperative-interrupt latch is raised
 * (Ctrl+C), distinct from a reset (-1) or EOF (0). */
#define TCP_EINTR (-2)

/* Blocking client API, driven from command context. A handle is valid from a
 * successful tcp_connect() until tcp_close(); the read/write/close calls take
 * the connection they operate on. */
tcp_conn_t *tcp_connect(const uint8_t dst_ip[4], uint16_t dport); /* NULL = fail */
int  tcp_write(tcp_conn_t *c, const void *data, uint32_t len);    /* bytes, -1   */
int  tcp_read(tcp_conn_t *c, uint8_t *buf, uint32_t max, int *closed);
                                                                  /* bytes; sets
                                                                   * *closed=EOF */
/* Non-blocking variant: returns bytes now (>0), 0 if none yet, -1 on reset;
 * sets *closed at EOF. Lets one thread service many connections (no sleeping). */
int  tcp_read_nb(tcp_conn_t *c, uint8_t *buf, uint32_t max, int *closed);
void tcp_close(tcp_conn_t *c);

/* Server API, for headless TCP daemons (now user-space, via net_sock_*).
 * tcp_listen() marks
 * a port open: a SYN arriving on it is accepted from interrupt context (the
 * three-way handshake completes there), pre-allocating receive buffers so the
 * IRQ path never allocates. tcp_accept() runs in a daemon thread and blocks
 * until a fully-established, not-yet-handed-out connection exists on `port`,
 * then returns it as an ordinary handle for tcp_read/tcp_write/tcp_close. Set
 * *stop non-zero from another context to make a blocked accept return NULL.
 *
 * Reuses the same TCB table as the client side — an accepted connection is
 * structurally identical to a connected one once it reaches ESTAB. */
int          tcp_listen(uint16_t port);          /* 0 ok, -1 no slot/buffers */
void         tcp_unlisten(uint16_t port);
tcp_conn_t  *tcp_accept(uint16_t port);  /* NULL if interrupted by a signal */

#endif /* OWOS_TCP_H */
