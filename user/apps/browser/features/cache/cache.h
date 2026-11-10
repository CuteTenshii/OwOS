/* cache.h: persistent HTTP response cache (bodies one file per entry on owofs, index in memory). */
#ifndef OWOS_BROWSER_CACHE_H
#define OWOS_BROWSER_CACHE_H

/* How cache_lookup treats a matching entry */
enum cache_lookup_mode {
  CACHE_LOOKUP_NORMAL = 0,
  CACHE_LOOKUP_REVALIDATE, /* never fresh: always validate (fetch cache: no-cache) */
  CACHE_LOOKUP_STALE_OK,   /* any entry is a hit (fetch cache: force-cache, only-if-cached) */
};

enum cache_verdict {
  CACHE_MISS = 0,   /* nothing usable, fetch normally                      */
  CACHE_FRESH,      /* a fresh copy was returned; skip the network         */
  CACHE_REVALIDATE, /* stale copy exists; send `cond`, then cache_revalidated */
};

/* What cache_lookup fills in; every buffer is optional (NULL or cap 0 skips it). */
struct cache_out {
  unsigned char *body; /* FRESH: malloc'd copy of the stored body */
  int len;
  char *ctype;
  int ctype_cap;
  char *hdr; /* the stored response header block */
  int hdr_cap;
  char *cond; /* REVALIDATE: conditional request header lines */
  int cond_cap;
};

/* Look up a GET. `req_hdrs` is the request header block ("Name: value\r\n" lines) that is about to be sent; it
 * selects the Vary variant. `out` is zeroed of body/len first. */
int cache_lookup(const char *method, const char *url, const char *req_hdrs, long now, int mode, struct cache_out *out);

/* Offer a GET response for storage under the variant `req_hdrs` selects; no-op unless cacheable. Disk write
 * deferred to cache_flush. */
void cache_store(const char *method, const char *url, const char *req_hdrs, int status, long now, const char *hdr_block,
                 const char *ctype, const unsigned char *body, int len);

/* After a 304: refresh the variant `req_hdrs` selects and return its body; 0 if it vanished. */
int cache_revalidated(const char *url, const char *req_hdrs, long now, const char *hdr_block, unsigned char **body,
                      int *len, char *ctype, int ctype_cap, char *hdr, int hdr_cap);

/* Restore the cache index and bodies from disk at startup. */
void cache_load(void);

/* Persist dirty entries and delete evicted files. Worker thread, no sub-resource fetch in flight. */
void cache_flush(void);

#endif /* OWOS_BROWSER_CACHE_H */
