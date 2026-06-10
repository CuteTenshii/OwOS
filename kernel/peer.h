/* peer.h — BitTorrent peer wire protocol, non-blocking.
 *
 * A peer connection is established once (TCP + handshake + `interested`) and
 * then driven by repeatedly calling peer_pump(): it consumes whatever bytes
 * have arrived (via the RX IRQ) without ever blocking, advancing an internal
 * message state machine. One thread can therefore service many connections at
 * once — `torrent get` round-robins peer_pump() across all of them, so pieces
 * download concurrently even though the kernel is single-core and cooperative.
 *
 * Leech-only: no seeding, no inbound connections. One piece in flight per
 * connection (blocks pipelined within it); pieces are returned in memory.
 */
#ifndef OWOS_PEER_H
#define OWOS_PEER_H

#include <stdint.h>

struct torrent;

typedef struct peer_conn peer_conn_t;

/* peer_pump() return codes. */
enum {
	PEER_WORKING = 0,   /* still downloading / idle (no event)              */
	PEER_PIECE   = 1,   /* the active piece finished AND verified (SHA-1)   */
	PEER_DEAD    = -1,  /* connection error — discard with peer_disconnect  */
	PEER_LACKS   = 2,   /* peer doesn't have the active piece — reassign it */
};

/* Connect + handshake (verifying `t`'s infohash) + `interested`. Blocking,
 * used at setup. Returns a handle, or NULL on failure. */
peer_conn_t *peer_connect(const struct torrent *t, const uint8_t ip[4],
                          uint16_t port);
void peer_disconnect(peer_conn_t *pc);

/* Begin downloading piece `index` into `out` (>= t->piece_length bytes, and it
 * must stay valid until the piece completes). Returns 0, or -1 if the peer is
 * already known not to have that piece. */
int  peer_start_piece(peer_conn_t *pc, const struct torrent *t,
                      uint32_t index, uint8_t *out);

/* Consume available bytes and advance the connection without blocking. See the
 * PEER_* codes. On PEER_PIECE the piece is in the `out` buffer passed to
 * peer_start_piece and the connection is idle (ready for another piece). */
int  peer_pump(peer_conn_t *pc, const struct torrent *t);

int  peer_busy(const peer_conn_t *pc);   /* 1 while a piece is in flight     */
uint32_t peer_piece(const peer_conn_t *pc); /* the active/just-finished index */

/* Does the peer advertise piece `index`? (Optimistic 'yes' until its bitfield
 * arrives.) Lets the caller assign pieces a peer actually has. */
int  peer_has_piece(const peer_conn_t *pc, uint32_t index);

/* Convenience: connect, download one piece (blocking), disconnect. Returns the
 * piece length on success, 0 if the peer lacks it, or -1 on error. */
int  peer_fetch_piece(const struct torrent *t, const uint8_t ip[4],
                      uint16_t port, uint32_t index, uint8_t *out);

#endif /* OWOS_PEER_H */
