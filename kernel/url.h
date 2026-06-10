/* url.h — a freestanding URL parser/builder.
 *
 * Splits a URL into its RFC 3986 components and reassembles them, with no
 * allocation: every part lives in a fixed buffer inside `struct url`. Handles
 *   scheme://[user[:pass]@]host[:port][/path][?query][#fragment]
 * plus bracketed IPv6 hosts ([::1]) and percent encode/decode helpers.
 *
 * It is intentionally lenient — the scheme and authority delimiters are
 * optional — so "example.com:8080/x" parses the same way curl's URL argument
 * does. See url.c.
 */
#ifndef OWOS_URL_H
#define OWOS_URL_H

#include <stdint.h>

#define URL_SCHEME_MAX   16
#define URL_USERINFO_MAX 64
#define URL_HOST_MAX     128
#define URL_PATH_MAX     256
#define URL_QUERY_MAX    256
#define URL_FRAGMENT_MAX 128

struct url {
	char     scheme[URL_SCHEME_MAX];      /* lowercased, no "://" ("" if absent) */
	char     userinfo[URL_USERINFO_MAX];  /* "user" or "user:pass" ("" if absent) */
	char     host[URL_HOST_MAX];          /* no surrounding brackets for IPv6     */
	char     path[URL_PATH_MAX];          /* leading '/' kept ("" if absent)       */
	char     query[URL_QUERY_MAX];        /* without the '?'                       */
	char     fragment[URL_FRAGMENT_MAX];  /* without the '#'                       */
	uint16_t port;                        /* scheme default, or the explicit value */
	uint8_t  has_port;                    /* port was written explicitly in input  */
	uint8_t  has_query;                   /* a '?' was present (even if empty)      */
	uint8_t  has_fragment;                /* a '#' was present (even if empty)      */
	uint8_t  is_ipv6;                     /* host was given in [..] bracket form    */
	uint8_t  is_tls;                      /* scheme implies TLS (https)             */
};

/* Parse `s` into `*u`. Returns 1 on success, 0 on a malformed URL (an empty
 * host, or an out-of-range/empty port). Unknown schemes parse fine but get no
 * default port (has_port stays 0 and port is 0). */
int url_parse(const char *s, struct url *u);

/* Reassemble `*u` into `out` (NUL-terminated). Returns the string length, or
 * -1 if it would not fit in `outsz`. Round-trips a parsed URL byte-for-byte
 * except for the scheme being lowercased and a defaulted port being dropped. */
int url_build(const struct url *u, char *out, int outsz);

/* Percent-encode/decode a component into `out`. Encoding leaves the RFC 3986
 * unreserved set (A-Z a-z 0-9 - _ . ~) untouched and %XX-escapes the rest.
 * Both return the output length, or -1 on overflow / a malformed %-escape. */
int url_encode(const char *s, char *out, int outsz);
int url_decode(const char *s, char *out, int outsz);

#endif /* OWOS_URL_H */
