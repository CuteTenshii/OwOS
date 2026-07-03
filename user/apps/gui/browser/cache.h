/* cache.h — the browser's HTTP response cache (see cache.c).
 *
 * A persistent, size-bounded store of GET responses, honouring the response
 * cache headers (Cache-Control / Expires / Age / Date / ETag / Last-Modified)
 * through user/lib/curl/httpcache. Bodies live one-file-per-entry on disk
 * (owofs), so they are not bound by the SQLite jar's 4 KiB record limit; a small
 * metadata index is kept in memory. The HTTP transport (fetch.c, resource.c)
 * only ever calls the functions below — no caching arithmetic and no storage
 * detail leaks into the transport. */
#ifndef OWOS_BROWSER_CACHE_H
#define OWOS_BROWSER_CACHE_H

enum cache_verdict {
	CACHE_MISS = 0,     /* nothing usable — fetch normally                     */
	CACHE_FRESH,        /* a fresh copy was returned; skip the network         */
	CACHE_REVALIDATE,   /* a stale copy exists; `cond` holds conditional headers
	                     * to add to the request, then call cache_revalidated  */
};

/* Look up a GET for `url`. On CACHE_FRESH: *body is a malloc'd copy (caller
 * frees), *len its length, `ctype` the stored Content-Type, and `hdr` (if given)
 * the stored raw response header block. On CACHE_REVALIDATE: `cond` gets the
 * "If-None-Match"/"If-Modified-Since" request lines (each "Name: value\r\n").
 * Only GET is cached; any other method is CACHE_MISS. `now` is epoch seconds. */
int  cache_lookup(const char *method, const char *url, long now,
                  unsigned char **body, int *len,
                  char *ctype, int ctype_cap,
                  char *hdr, int hdr_cap,
                  char *cond, int cond_cap);

/* Offer a GET response for storage. `hdr_block` is the raw response headers,
 * `body`/`len` the final (already content-decoded) body, `ctype` its
 * Content-Type. No-ops unless the response is cacheable and worth keeping. Safe
 * to call from the parallel sub-resource threads — the disk write is deferred to
 * cache_flush(). */
void cache_store(const char *method, const char *url, int status, long now,
                 const char *hdr_block, const char *ctype,
                 const unsigned char *body, int len);

/* A conditional GET returned 304 Not Modified: refresh the stored entry's
 * freshness from `hdr_block` and hand back a fresh copy of the stored body.
 * Returns 1 with body/len/ctype/hdr filled, or 0 if the entry vanished (the
 * caller must then fetch unconditionally). */
int  cache_revalidated(const char *url, long now, const char *hdr_block,
                       unsigned char **body, int *len,
                       char *ctype, int ctype_cap, char *hdr, int hdr_cap);

/* Restore the cache index + bodies from disk at browser startup (best-effort). */
void cache_load(void);

/* Persist dirty entries and delete evicted files. MUST run on the worker thread
 * with no sub-resource fetch in flight (owofs allows one open stream at a time),
 * so the transport defers to it after a navigation completes. */
void cache_flush(void);

#endif /* OWOS_BROWSER_CACHE_H */
