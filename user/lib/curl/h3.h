/* h3.h — HTTP/3 client engine for libcurl (RFC 9114 framing + RFC 9204 QPACK),
 * riding the QUIC transport in quic.c. Errors reuse http.h's HTTP_E*
 * codes so a caller handles an HTTP/3, HTTP/2 and HTTP/1.1 transfer the same
 * way. One request on stream 0; QPACK uses the static table only (we advertise
 * a zero-capacity dynamic table), so responses decode without dynamic state. */
#ifndef OWOS_LIBH3_H
#define OWOS_LIBH3_H

#include "http.h"   /* http_body_fn + HTTP_E* status/error codes */

/* Perform one HTTP/3 (always https) request: open a QUIC connection to
 * host:port, send the request on stream 0, and stream the response body to
 * body(ctx,…). `authority` is the :authority (host or host:port); `path` the
 * :path; `extra_headers` an optional "name: value\r\n…" block (curl -H). The
 * decoded response header block ("HTTP/3 NNN\r\nname: value\r\n…", blank-line
 * terminated) is copied into hdr (NUL-terminated, up to hdrcap-1) with *hdrlen
 * set, mirroring http_request(). Returns the HTTP status or a negative HTTP_E*. */
int h3_run(const char *host, int port, const char *authority,
           const char *method, const char *path, const char *extra_headers,
           char *hdr, int hdrcap, int *hdrlen,
           http_body_fn body, void *ctx);

#endif /* OWOS_LIBH3_H */
