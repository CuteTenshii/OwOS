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
 * SK_TCP. Returns a handle or NULL on failure. */
netconn *nc_open(const char *host, int port, int tls, int offer_h2);

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

void nc_close(netconn *c);

#endif /* OWOS_USER_NETCONN_H */
