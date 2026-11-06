/* cors.h: CORS and Cross-Origin-Resource-Policy checks. Pure policy, no transport or globals. */
#ifndef OWOS_BROWSER_CORS_H
#define OWOS_BROWSER_CORS_H

struct http_headers; /* headers.h */

/* Write `url`'s origin into out; "null" for non-http(s) or unparseable URLs. */
void cors_origin(const char *url, char *out, int cap);

/* 1 if the two URLs share scheme, host and port. */
int cors_same_origin(const char *a, const char *b);

/* 1 if same scheme and registrable domain (last two host labels, so "co.uk" is wrong). */
int cors_same_site(const char *a, const char *b);

/* 1 unless the request is "simple" (GET/HEAD/POST, safelisted headers and content type). */
int cors_needs_preflight(const char *method, const char *req_ctype, const char *author_hdrs);

/* List non-safelisted author header names, lowercased and comma-separated; "" if none. */
void cors_request_header_names(const char *author_hdrs, char *out, int cap);

/* Validate a CORS response against `origin`. 1 = readable; `reason` (optional) set on failure. */
int cors_check_response(const struct http_headers *H, const char *origin, int with_credentials, char *reason, int rcap);

/* Validate a preflight response: response check plus allowed method and headers. 1 = may proceed. */
int cors_check_preflight(const struct http_headers *H, const char *origin, const char *method, const char *author_hdrs,
                         int with_credentials, char *reason, int rcap);

/* May `embedder_url` use `resource_url` under its Cross-Origin-Resource-Policy? 1 = allowed. */
int corp_allows(const struct http_headers *H, const char *embedder_url, const char *resource_url, char *reason,
                int rcap);

#endif /* OWOS_BROWSER_CORS_H */
