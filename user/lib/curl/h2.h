/* h2.h — HTTP/2 client engine for libcurl (the protocol half lives here; curl
 * and http.c just drive it). Transport-agnostic: it runs over an already-open
 * net_connect() handle — plaintext for h2c "prior knowledge", or a TLS handle
 * once ALPN has negotiated "h2". Implements the connection preface, the binary
 * frame layer (SETTINGS/HEADERS/DATA/WINDOW_UPDATE/PING/GOAWAY/RST/CONTINUATION)
 * and HPACK (RFC 7541: static + dynamic table, integer + Huffman string coding).
 *
 * One request, one stream (stream 1), GET/HEAD/etc. with no request body — the
 * shape libcurl needs. Errors reuse http.h's HTTP_E* codes, so callers handle
 * an h2 transfer and an HTTP/1.1 transfer the same way. */
#ifndef OWOS_LIBH2_H
#define OWOS_LIBH2_H

#include "http.h"   /* http_body_fn + HTTP_E* status/error codes */

/* Perform one HTTP/2 request over the open transport `sock`. `scheme_https`
 * picks the :scheme pseudo-header ("https" vs "http"); `authority` is the
 * Host/:authority value (host or host:port); `path` is the :path. The decoded
 * response header lines ("name: value\r\n"…, blank-line terminated) are copied
 * into hdr (NUL-terminated, up to hdrcap-1 bytes) with *hdrlen set, mirroring
 * http_request(); body bytes stream to body(ctx,…). Returns the HTTP status
 * code, or a negative HTTP_E* code. Does not close `sock`. */
int h2_request(int sock, int scheme_https, const char *authority,
               const char *method, const char *path,
               char *hdr, int hdrcap, int *hdrlen,
               http_body_fn body, void *ctx);

#endif /* OWOS_LIBH2_H */
