/* h3.h — HTTP/3 client engine for libcurl (RFC 9114 framing + RFC 9204 QPACK),
 * riding the QUIC transport in quic.c. Errors reuse http.h's HTTP_E*
 * codes so a caller handles an HTTP/3, HTTP/2 and HTTP/1.1 transfer the same
 * way. QPACK uses the static table only (we advertise a zero-capacity dynamic
 * table), so responses decode without dynamic state. */
#ifndef OWOS_LIBH3_H
#define OWOS_LIBH3_H

#include "h2.h" /* struct h2_fetch, the request/result record batches share with h2; http_body_fn + HTTP_E* */
#include "http.h"

/* Perform one HTTP/3 (always https) request: open a QUIC connection to
 * host:port, send the request, and stream the response body to body(ctx,…).
 * `authority` is the :authority (host or host:port); `path` the :path;
 * `extra_headers` an optional "name: value\r\n…" block (curl -H). The decoded
 * response header block ("HTTP/3 NNN\r\nname: value\r\n…", blank-line
 * terminated) is copied into hdr (NUL-terminated, up to hdrcap-1) with *hdrlen
 * set, mirroring http_request(). Returns the HTTP status or a negative HTTP_E*.
 */
int h3_run(const char *host, int port, const char *authority, const char *method, const char *path,
           const char *extra_headers, char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx);

/* ------------------------------------------------------------- sessions
 * A session keeps one QUIC connection open across requests: streams 0, 4, 8, … in turn, or a batch at once. It
 * waits in the idle pool (netconn.h) between uses and serves one caller at a time. */
typedef struct h3 h3_session;

/* Per-request hooks. on_headers gets every header block before the body: interim 1xx responses (103 Early Hints)
 * and then the final one, as "HTTP/3 NNN\r\nname: value\r\n…" without the blank line; non-zero aborts. */
struct h3_opts {
  int (*on_headers)(void *ctx, const char *block, int len);
  void *ctx;
};

/* Server push. With a handler set, new sessions invite the server to push up to 8 responses and a finished push
 * that matches its PUSH_PROMISE is handed over: the connection's authority (host[:port]), the promised request's
 * scheme, authority and path, the response's header block ("HTTP/3 NNN\r\n…") and body. The handler decides
 * whether to keep it. Pushes arrive while a request is in flight; a session without a handler never receives any. */
typedef void (*h3_push_fn)(void *ctx, const char *conn_authority, const char *scheme, const char *authority,
                           const char *path, const char *hdr, const unsigned char *body, int blen);
void h3_set_push_handler(h3_push_fn fn, void *ctx);

/* Connect (QUIC handshake + HTTP/3 setup); 0 or a negative HTTP_E*. */
int h3_session_open(h3_session **out, const char *host, int port);

/* An idle session for host:port, or NULL. It may have been closed by the peer meanwhile: when h3_session_used()
 * and a request fails before any response, retry on a fresh connection. */
h3_session *h3_session_acquire(const char *host, int port);

/* Give a session back: parked for reuse if it is healthy, else closed. */
void h3_session_release(h3_session *s, const char *host, int port);

void h3_session_close(h3_session *s);
int h3_session_used(const h3_session *s);
int h3_session_reusable(const h3_session *s);

/* The server announced with GOAWAY that it will take no more requests: the session is not reused, and a request it
 * had not processed fails with HTTP_ESEND so the caller can send it again elsewhere. */

/* One request on a session. reqbody (reqlen > 0) is sent as the request body. Returns the status or a negative
 * HTTP_E*; HTTP_ESEND also covers a body larger than the server lets us send, so callers can use another
 * protocol. */
int h3_session_run(h3_session *s, const char *authority, const char *method, const char *path,
                   const char *extra_headers, const unsigned char *reqbody, int reqlen, const struct h3_opts *opts,
                   char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx);

/* GET every reqs[i].path on its own stream at once; the contract of h2_fetch_many. */
int h3_session_fetch_many(h3_session *s, const char *authority, const char *extra_headers, struct h2_fetch *reqs,
                          int nreq, void (*on_done)(void *ctx, int i, const unsigned char *body, int blen),
                          void *cb_ctx);

#endif /* OWOS_LIBH3_H */
