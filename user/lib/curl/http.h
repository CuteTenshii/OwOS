/* http.h — libcurl: a tiny ring-3 HTTP/1.1 client over the net_* syscalls.
 *
 * The protocol (URL parse, request build, status line, header block, and body
 * framing — chunked / Content-Length / read-until-close) lives here so both the
 * `curl` program and libtorrent's tracker share one implementation. The body is
 * delivered incrementally to a sink callback, so a caller can stream it to the
 * console, a file, or a growing buffer. Every blocking step honors the
 * cooperative Ctrl+C interrupt and returns HTTP_EINTR. */
#ifndef OWOS_LIBHTTP_H
#define OWOS_LIBHTTP_H

/* Negative status codes; a non-negative return is the HTTP status (e.g. 200). */
#define HTTP_EPARSE   (-2)   /* malformed URL                                  */
#define HTTP_ERESOLVE (-3)   /* DNS lookup failed                              */
#define HTTP_ECONNECT (-4)   /* TCP/TLS connect failed                         */
#define HTTP_ESEND    (-5)   /* sending the request failed                     */
#define HTTP_ENORESP  (-6)   /* connection closed before a full header block   */
#define HTTP_EINTR    (-7)   /* Ctrl+C arrived mid-transfer                    */
#define HTTP_EABORT   (-8)   /* the body sink asked to stop                    */

/* Body sink: called with successive chunks of the response body. Return 0 to
 * keep going, non-zero to abort the transfer (http_request then returns
 * HTTP_EABORT). */
typedef int (*http_body_fn)(void *ctx, const unsigned char *buf, int len);

/* Perform one request. `method` defaults to "GET" when NULL. When `hdr` is
 * non-NULL the raw response header block is copied there (NUL-terminated, up to
 * hdrcap-1 bytes) and *hdrlen set. Body bytes go to body(ctx, ...) as they
 * arrive (body/ctx may be NULL to discard). Returns the HTTP status code, or a
 * negative HTTP_E* code. */
int http_request(const char *method, const char *url,
                 char *hdr, int hdrcap, int *hdrlen,
                 http_body_fn body, void *ctx);

/* Same contract as http_request, but speaks HTTP/2 where possible: over http://
 * it uses h2c "prior knowledge" (sends the HTTP/2 connection preface straight
 * away); over https:// it falls back to HTTP/1.1 until TLS ALPN "h2" negotiation
 * is wired into the kernel. Returns the HTTP status or a negative HTTP_E* code. */
int http_request2(const char *method, const char *url,
                  char *hdr, int hdrcap, int *hdrlen,
                  http_body_fn body, void *ctx);

/* Convenience: GET, body to the sink. */
int http_get(const char *url, http_body_fn body, void *ctx);

#endif /* OWOS_LIBHTTP_H */
