/* httpcache.h — libcurl: the HTTP caching *policy*, RFC 7234 (no storage, no I/O).
 *
 * This is the pure decision core an HTTP cache is built on: parse a response's
 * cache-relevant headers into a struct, decide whether the response may be
 * stored at all, and compute its freshness lifetime and current age so a cache
 * can tell fresh from stale. Where the bytes actually live, how they are keyed,
 * eviction — all of that is the caller's (see the browser's cache.c). Keeping
 * the fiddly, spec-driven arithmetic here means it is tested once, host-side,
 * instead of re-derived in every cache.
 *
 * Also exposes http_date_parse: the three HTTP-date grammars (RFC 7231
 * IMF-fixdate, the obsolete RFC 850 form, and asctime) -> Unix epoch seconds,
 * which Date/Expires/Last-Modified all use.
 */
#ifndef OWOS_LIBHTTPCACHE_H
#define OWOS_LIBHTTPCACHE_H

#include "headers.h"

/* Parse an HTTP-date to Unix epoch seconds (UTC), or 0 if unparseable. Accepts
 * "Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT", and
 * "Sun Nov  6 08:49:37 1994". */
long http_date_parse(const char *s);

/* Cache-relevant facts pulled from a response's headers (RFC 7234 §5). The
 * string pointers (etag / last_mod_str) point into the parsed `struct
 * http_headers` and are valid only as long as it is. */
struct http_cc {
	int  no_store;        /* Cache-Control: no-store — must not be stored      */
	int  no_cache;        /* no-cache — may store but revalidate before EVERY use */
	int  must_revalidate; /* must-revalidate — once stale, never serve stale   */
	int  is_private;      /* private — a *shared* cache mustn't store; a single-
	                       * user cache (ours) still may                        */
	int  has_max_age;     /* a max-age directive was present                   */
	long max_age;         /* its value in seconds                              */
	int  has_expires;     /* an Expires header was present (even if invalid)   */
	long expires;         /* its epoch, 0 if absent/unparseable                */
	long date;            /* Date header epoch, 0 if absent                    */
	long age;             /* Age header seconds, 0 if absent                   */
	long last_modified;   /* Last-Modified epoch, 0 if absent/unparseable      */
	const char *etag;         /* ETag value, or NULL                           */
	const char *last_mod_str; /* raw Last-Modified value, or NULL              */
};

/* Fill *cc from the parsed headers. */
void http_cc_parse(const struct http_headers *H, struct http_cc *cc);

/* May a `status` response with these directives be stored at all? True only for
 * a 200 that is not no-store. (Freshness/validator worth is a separate check —
 * see http_cc_freshness; a caller declines to store a 200 that is neither fresh
 * nor validatable.) */
int  http_cc_storable(const struct http_cc *cc, int status);

/* The response's freshness lifetime in seconds: max-age, else Expires - Date,
 * else a Last-Modified heuristic (10% of the resource's age, capped at a day),
 * else 0 (stale on arrival). `now` is epoch seconds, used only when Date is
 * absent for the Expires calculation. */
long http_cc_freshness(const struct http_cc *cc, long now);

/* The response's current age (RFC 7234 §4.2.3): how old the cached copy is now,
 * from the Age/Date headers plus how long it has resided in the cache.
 * `stored_at` is when the response was received (epoch seconds). */
long http_cc_current_age(const struct http_cc *cc, long stored_at, long now);

#endif /* OWOS_LIBHTTPCACHE_H */
