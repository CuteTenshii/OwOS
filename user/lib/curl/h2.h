/* h2.h — HTTP/2 client engine for libcurl (the protocol half lives here; curl
 * and http.c just drive it). Transport-agnostic: it runs over an already-open
 * netconn handle (user/lib/netconn) — plaintext for h2c "prior knowledge", or a
 * TLS handle once ALPN has negotiated "h2". Implements the connection preface,
 * the binary frame layer (SETTINGS/HEADERS/DATA/WINDOW_UPDATE/PING/GOAWAY/RST/
 * CONTINUATION) and HPACK (RFC 7541: static + dynamic table, integer +
 * Huffman).
 *
 * One request per connection at a time (a batch multiplexes several GETs), with
 * an optional request body. Errors reuse http.h's HTTP_E* codes, so callers handle
 * an h2 transfer and an HTTP/1.1 transfer the same way. */
#ifndef OWOS_LIBH2_H
#define OWOS_LIBH2_H

#include "http.h" /* http_body_fn + HTTP_E* status/error codes */

typedef struct netconn netconn; /* the TCP-or-TLS transport (user/lib/netconn) */

/* Perform one HTTP/2 request over the open transport `conn`. `scheme_https`
 * picks the :scheme pseudo-header ("https" vs "http"); `authority` is the
 * Host/:authority value (host or host:port); `path` is the :path. The decoded
 * response header lines ("name: value\r\n"…, blank-line terminated) are copied
 * into hdr (NUL-terminated, up to hdrcap-1 bytes) with *hdrlen set, mirroring
 * http_request(); body bytes stream to body(ctx,…). Returns the HTTP status
 * code, or a negative HTTP_E* code. Does not close `conn`. */
int h2_request(netconn *conn, int scheme_https, const char *authority, const char *method, const char *path, char *hdr,
               int hdrcap, int *hdrlen, http_body_fn body, void *ctx);

/* Richer entry point for callers that need more than a bare GET (the browser).
 *   user_agent    the User-Agent to send (NULL -> none; a caller wanting one
 *                 either sets this or puts a user-agent line in extra_headers).
 *   extra_headers a "name: value\r\n…" block of request headers (e.g. Cookie,
 *                 Accept, Accept-Language) — names are lowercased on encode, as
 *                 HTTP/2 requires; when set it replaces the default Accept.
 *                 Do NOT include connection-specific headers (Connection,
 *                 Transfer-Encoding) or Accept-Encoding (responses stay
 *                 uncompressed so the body needs no inflate).
 *   on_headers    if set, called once after the response header block is
 * decoded and BEFORE any body — `block`/`len` is the synthesized "HTTP/2
 * NNN\r\nname: value\r\n…" text (requires a non-NULL hdr buffer). Lets the
 * caller parse headers and pick a body sink before DATA arrives. Return 0 to
 * continue, non-zero to abort. */
struct h2_opts {
  const char *user_agent;
  const char *extra_headers;
  int (*on_headers)(void *ctx, const char *block, int len);
};

/* As h2_request, plus `opts` (may be NULL for the bare-GET behaviour). */
int h2_run(netconn *conn, int scheme_https, const char *authority, const char *method, const char *path,
           const struct h2_opts *opts, char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx);

/* Like h2_run but sends a request body in DATA frames — for POST/PUT over h2.
 * The body must fit the peer's default 65535 send-window; larger callers use
 * h1.1. */
int h2_run_body(netconn *conn, int scheme_https, const char *authority, const char *method, const char *path,
                const struct h2_opts *opts, const unsigned char *reqbody, int reqlen, char *hdr, int hdrcap,
                int *hdrlen, http_body_fn body, void *ctx);

/* One batched, MULTIPLEXED fetch: GET every reqs[i].path concurrently over a
 * single h2 connection (own stream each: 1, 3, 5, …), demuxing the interleaved
 * responses. All requests share one origin (`authority`/`scheme_https`) and one
 * `opts` (cookies/UA/accept), so it is for same-origin sub-resources.
 *
 * `on_done` (optional) is called the moment each request's stream COMPLETES
 * with a 200 — (cb_ctx, request index, body, blen) — so the caller can consume
 * it progressively (the browser commits each image as it lands). When on_done
 * is set the engine owns and frees the bodies (the callback copies what it
 * needs); reqs[i].body is left NULL. When on_done is NULL, each body instead
 * accumulates into a malloc'd reqs[i].body (CALLER FREES). Either way
 * .status/.failed are set
 * (.failed = reset/refused/overflow/non-200 — the caller can retry on h1.1).
 * If .hdr is non-NULL (caller supplies the buffer + .hdrcap), the stream's
 * response header block is captured into it ("HTTP/2 NNN\r\nname: value\r\n…",
 * NUL-terminated) with .hdrlen set — for a caller that must read the response
 * headers (e.g. an HTTP cache). Leave .hdr NULL to skip capture.
 * Returns 0 if the connection ran to completion, or a negative HTTP_E* on a
 * transport failure. `nreq` is capped at H2_FETCH_MAX. */
#define H2_FETCH_MAX 16
struct h2_fetch {
  const char *path;    /* in: :path to GET                                */
  int status;          /* out: HTTP status (0 if the stream never replied) */
  unsigned char *body; /* out: malloc'd response body (caller frees; NULL if on_done) */
  int blen;            /* out: its length                                 */
  int failed;          /* out: 1 = reset/refused/overflow/non-200          */
  char *hdr;           /* in (opt): response-header buffer; out: filled    */
  int hdrcap;          /* in: its capacity                                 */
  int hdrlen;          /* out: header block length written                 */
};
int h2_fetch_many(netconn *conn, int scheme_https, const char *authority, const struct h2_opts *opts,
                  struct h2_fetch *reqs, int nreq,
                  void (*on_done)(void *ctx, int i, const unsigned char *body, int blen), void *cb_ctx);

/* ------------------------------------------------------------- sessions
 * A session keeps one h2 connection open across requests: the preface is sent once, stream ids, flow-control
 * windows and the HPACK table carry on, and the connection waits in the idle pool (netconn.h) between uses. A
 * session serves one request at a time; concurrent callers each get their own. */
typedef struct h2_conn h2_session;

/* Wrap a connection whose ALPN selected h2 (nothing sent yet). Takes ownership of `conn`, closing it on failure. */
h2_session *h2_session_wrap(netconn *conn);

/* An idle session for host:port (TLS), or NULL. It may have been closed by the peer meanwhile: when
 * h2_session_used() and a request fails before any response, retry on a fresh connection. */
h2_session *h2_session_acquire(const char *host, int port);

/* Give a session back after a request: parked for reuse if it ended cleanly, else closed. */
void h2_session_release(h2_session *s, const char *host, int port);

/* Close a session and its connection. */
void h2_session_close(h2_session *s);

/* 1 if the session has already carried a request. */
int h2_session_used(const h2_session *s);

/* h2_run_body / h2_fetch_many over a session. */
int h2_session_run(h2_session *s, int scheme_https, const char *authority, const char *method, const char *path,
                   const struct h2_opts *opts, const unsigned char *reqbody, int reqlen, char *hdr, int hdrcap,
                   int *hdrlen, http_body_fn body, void *ctx);
int h2_session_fetch_many(h2_session *s, int scheme_https, const char *authority, const struct h2_opts *opts,
                          struct h2_fetch *reqs, int nreq,
                          void (*on_done)(void *ctx, int i, const unsigned char *body, int blen), void *cb_ctx);

#endif /* OWOS_LIBH2_H */
