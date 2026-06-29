/* cors.h — Cross-Origin Resource Sharing + Cross-Origin-Resource-Policy logic.
 *
 * Pure policy: no transport, no globals. The browser's fetch()/XMLHttpRequest
 * path (the CORS protocol) and its sub-resource loader (CORP) drive it with the
 * already-parsed response headers and the two URLs whose origins are in play.
 *
 *   CORS governs whether a *cross-origin* fetch/XHR response may be read by the
 *   page: the request carries an Origin, a non-"simple" request is gated by a
 *   preflight OPTIONS, and the response must opt in via Access-Control-Allow-*.
 *
 *   CORP is the inverse guard for *no-cors* sub-resources (images, scripts,
 *   stylesheets, fonts, media, frames): a response's Cross-Origin-Resource-Policy
 *   can forbid a cross-origin document from using it at all.
 */
#ifndef OWOS_BROWSER_CORS_H
#define OWOS_BROWSER_CORS_H

struct http_headers;   /* headers.h */

/* Write `url`'s origin ("scheme://host[:port]", default port omitted) into out.
 * A non-http(s) or unparseable URL yields the opaque origin "null". */
void cors_origin(const char *url, char *out, int cap);

/* 1 if the two URLs share an origin (scheme + host + port all equal). */
int  cors_same_origin(const char *a, const char *b);

/* 1 if the two URLs are schemeful same-site: equal scheme and equal registrable
 * domain (approximated as the last two dotted labels of the host — so it treats
 * multi-label public suffixes like "co.uk" as registrable, a known limitation). */
int  cors_same_site(const char *a, const char *b);

/* Does a cross-origin CORS request need a preflight? It does unless it is a
 * "simple" request: method in {GET, HEAD, POST}, every author-set header (the
 * "Name: value\r\n" blob `author_hdrs`, or NULL) CORS-safelisted, and the request
 * Content-Type (`req_ctype`, or NULL) one of the form-submission types. */
int  cors_needs_preflight(const char *method, const char *req_ctype,
                          const char *author_hdrs);

/* Collect the non-safelisted author header names from an "Name: value\r\n" blob
 * into a lowercased, comma-separated list (the Access-Control-Request-Headers a
 * preflight announces). `out` is "" when every header is safelisted. */
void cors_request_header_names(const char *author_hdrs, char *out, int cap);

/* Validate a CORS response against the request `origin`. With credentials the
 * Access-Control-Allow-Origin must echo the origin exactly (never "*") and
 * Access-Control-Allow-Credentials must be "true"; without, "*" or the origin is
 * fine. Returns 1 = the response may be read. `reason` (optional) gets a short
 * cause on failure. */
int  cors_check_response(const struct http_headers *H, const char *origin,
                         int with_credentials, char *reason, int rcap);

/* Validate a preflight (OPTIONS) response: cors_check_response, plus the actual
 * request's method must be covered by Access-Control-Allow-Methods and every
 * non-safelisted author header by Access-Control-Allow-Headers (a literal "*"
 * wildcard counts only without credentials). Returns 1 = the real request may
 * proceed. */
int  cors_check_preflight(const struct http_headers *H, const char *origin,
                          const char *method, const char *author_hdrs,
                          int with_credentials, char *reason, int rcap);

/* Cross-Origin-Resource-Policy: may the document at `embedder_url` use a
 * sub-resource served from `resource_url` with response headers `H`? An absent or
 * unrecognised policy allows; "same-origin"/"same-site" require the embedder to
 * match. Returns 1 = allowed; `reason` (optional) gets a short cause on a block. */
int  corp_allows(const struct http_headers *H, const char *embedder_url,
                 const char *resource_url, char *reason, int rcap);

#endif /* OWOS_BROWSER_CORS_H */
