/* netconn.h — a byte-stream transport that is either a plain SK_TCP socket or a
 * user-space TLS connection (user/lib/tls), behind one handle. Lets a protocol
 * client (HTTP/1.1, HTTP/2, WebSocket) hold "TCP or TLS" without branching on
 * every I/O.
 */
#ifndef OWOS_USER_NETCONN_H
#define OWOS_USER_NETCONN_H

typedef struct netconn netconn;

#define NC_OPEN_RESOLVE 1
#define NC_OPEN_CONNECT 2
#define NC_OPEN_TLS 3

/* Open a connection to host:port. tls!=0 uses user/lib/tls (offer_h2 adds "h2"
 * to the ALPN offer, readable back with nc_alpn); tls==0 opens net_connect with
 * SK_TCP. Takes a matching idle connection from nc_preconnect first.
 * Returns a handle or NULL on failure. */
netconn *nc_open(const char *host, int port, int tls, int offer_h2);

/* Open a connection and park it idle for the next nc_open of the same host, port, tls and offer_h2 (single use,
 * dropped after ~10 s). Blocks while connecting: call it from a background thread. */
void nc_preconnect(const char *host, int port, int tls, int offer_h2);

/* Open without consulting the idle pool, e.g. to retry after a pooled connection turned out dead. */
netconn *nc_open_fresh(const char *host, int port, int tls, int offer_h2);

/* Like nc_open(host, port, tls, 1), but first reuses a kept-alive HTTP/1.1 connection parked by nc_release. */
netconn *nc_acquire(const char *host, int port, int tls);

/* Park `c` for reuse by nc_acquire: the caller must have read the whole response of an HTTP/1.1 exchange that
 * left the connection open, with nothing buffered. The pool owns `c` afterwards. */
void nc_release(netconn *c, const char *host, int port, int tls);

/* 1 if `c` came out of the idle pool: the peer may have closed it without our noticing, so a failure before any
 * response byte is worth one retry on nc_open_fresh. */
int nc_pooled(netconn *c);

/* 1 if the peer has closed `c` or it can't be reused: close_notify, FIN or reset, or unexpected data. With
 * allow_data (a protocol where the peer may speak first, like h2) one received TLS record is kept for nc_read.
 * Never blocks. */
int nc_peer_closed(netconn *c, int allow_data);

/* Generic idle entries for state wrapped around a connection (the h2 session): destroy(obj) frees it when it
 * expires or is evicted, closed(obj) (optional) says the peer has hung up. nc_idle_take hands out one live entry
 * of that kind, or NULL. */
enum { NC_IDLE_FRESH = 1, NC_IDLE_H1 = 2, NC_IDLE_H2 = 3, NC_IDLE_H3 = 4 };
void nc_idle_put(const char *host, int port, int tls, int kind, void *obj, void (*destroy)(void *),
                 int (*closed)(void *));
void *nc_idle_take(const char *host, int port, int tls, int kind);

int nc_last_error(void);

/* Wrap an already-open kernel socket fd as a plain (non-TLS) netconn. The
 * netconn owns the fd afterwards (nc_close closes it). NULL on alloc failure.
 */
netconn *nc_from_fd(int fd);

/* Blocking read: >0 bytes / 0 EOF / <0 error (NET_EINTR passes through). */
int nc_read(netconn *c, void *buf, int max);

/* Non-blocking read for a plain TCP conn (net_recv_nb: >0 / 0 would-block / -1
 * EOF/error / NET_EINTR). A TLS conn has no non-blocking read (the kernel
 * SK_TLS path never had one either), so this returns -1 for TLS. */
int nc_read_nb(netconn *c, void *buf, int max);

int nc_write(netconn *c, const void *buf, int len);

/* The negotiated ALPN protocol (owos.h ALPN_*). */
int nc_alpn(netconn *c);

/* Why the server certificate of a TLS conn was not accepted (e.g. "untrusted issuer"), or NULL when it
 * verified or the conn is plain TCP. The connection is usable either way; see tls_verify(). */
const char *nc_verify_message(netconn *c);

void nc_close(netconn *c);

#endif /* OWOS_USER_NETCONN_H */
