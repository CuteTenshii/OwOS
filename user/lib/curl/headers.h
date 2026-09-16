/* headers.h — libcurl: HTTP response headers as a parsed key/value object.
 *
 * A client never walks the raw header block itself. It calls http_headers_parse
 * once to turn "HTTP/1.1 200 OK\r\nName: value\r\n…\r\n\r\n" (or the HTTP/2-
 * synthesized "HTTP/2 200\r\nname: value\r\n…" form) into a struct holding the
 * status code and every header as a {name, value} pair, then queries it by
 * name:
 *
 *     struct http_headers h;
 *     http_headers_parse(&h, block);
 *     const char *ct = http_headers_get(&h, "content-type");   // or NULL
 *     for (int i = 0; i < http_headers_count(&h, "set-cookie"); i++)
 *         use(http_headers_at(&h, "set-cookie", i));           // repeated
 * header http_headers_free(&h);
 *
 * Names are stored lowercased; values are trimmed, NUL-terminated heap copies —
 * so there is no length cap on a value (a giant Set-Cookie survives whole) and
 * no CR/LF gotchas. Lookups are case-insensitive. Repeated headers are kept in
 * order. Backed by malloc; call http_headers_free when done.
 */
#ifndef OWOS_LIBHEADERS_H
#define OWOS_LIBHEADERS_H

#include <stdbool.h>

struct http_field {
  char *name;
  char *value;
};

struct http_headers {
  int status;                /* status-line code, or 0                    */
  struct http_field *fields; /* parsed headers, in order                  */
  int n, cap;
};

/* Parse `block` into *h (zeroes it first). Returns 0, or -1 on OOM (partial
 * results are still usable and must still be freed). */
int http_headers_parse(struct http_headers *h, const char *block);

/* First value for `name` (case-insensitive), or NULL if absent. */
const char *http_headers_get(const struct http_headers *h, const char *name);

/* How many times `name` appears (for repeated headers like Set-Cookie). */
int http_headers_count(const struct http_headers *h, const char *name);

/* The idx-th value for `name` (0-based), or NULL if out of range. */
const char *http_headers_at(const struct http_headers *h, const char *name, int idx);

void http_headers_free(struct http_headers *h);

/* Value helpers (work on any header value string). */

/* Case-insensitive token test for a comma/space list — e.g.
 * http_header_has_token("gzip, chunked", "chunked"). */
bool http_header_has_token(const char *value, const char *token);

/* Leading unsigned decimal of `value` (after optional spaces). *ok (if
 * non-NULL) flags whether any digit was seen. */
unsigned http_parse_uint(const char *value, bool *ok);

#endif /* OWOS_LIBHEADERS_H */
