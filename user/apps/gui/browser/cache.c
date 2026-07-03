/* cache.c — the browser's persistent HTTP response cache.
 *
 * A store of GET responses keyed by URL, honouring the response cache headers
 * via user/lib/curl/httpcache (the RFC 7234 freshness/age policy). It survives
 * across sessions: each entry's body lives in its own file under CACHE_DIR on
 * owofs, so a body is NOT bound by the SQLite jar's 4 KiB record limit (jar.c) —
 * the reason cookies use SQLite but response bodies can't. A small metadata
 * index (URL, freshness facts, validators, on-disk filename) is held in memory.
 *
 * The store is bounded: total body bytes are capped (LRU eviction), and one
 * oversized body is simply not cached. All of it is best-effort — a read/write
 * failure just means a miss or a not-cached response, never a fetch failure. A
 * read-only home directory transparently degrades to a memory-only cache.
 *
 * Concurrency: cache_lookup/cache_store run on the parallel sub-resource pool,
 * so the in-memory index is guarded by a spinlock. But owofs allows only one
 * open write stream at a time, so disk writes/deletes are NOT done inline — they
 * are deferred to cache_flush(), which the transport calls from the single
 * worker thread after a navigation, when no fetch is in flight.
 *
 * The transport (fetch.c, resource.c) sees only cache_lookup / cache_store /
 * cache_revalidated / cache_load / cache_flush. */
#include "browser.h"
#include "headers.h"
#include "httpcache.h"
#include "cache.h"

#define CACHE_DIR       "/home/root/.cache/browser"
#define CACHE_MAX_BYTES (16 * 1024 * 1024)
#define CACHE_MAX_ONE   ( 4 * 1024 * 1024)
#define CACHE_MAGIC     "OWCACHE1"
#define PEND_UNLINK_MAX 128            /* deferred deletes between flushes       */
#define FM_MAX          3072           /* front-matter (magic+url+meta+ctype) cap */

/* One stored response. body/hdr/ctype/etag/last_mod are owned heap copies. The
 * freshness facts are frozen at store time so a lookup needs no re-parse; only
 * a 304 revalidation recomputes them. */
struct cache_entry {
	char          *url;
	int            status;
	char          *ctype;
	char          *hdr;              /* raw response header block            */
	unsigned char *body;
	int            len;

	long           stored_at;        /* epoch seconds when received          */
	long           freshness;        /* lifetime seconds (from headers)      */
	long           date, age;        /* for current-age recomputation        */
	int            no_cache, must_revalidate;
	char          *etag;             /* validator, or NULL                   */
	char          *last_mod;         /* validator, or NULL                   */

	char           disk[24];         /* on-disk filename (hash of url), or "" */
	int            dirty;            /* body/headers not yet written to disk */
	unsigned       lru;              /* bumped on every access; oldest evicts */
	struct cache_entry *next;
};

static struct cache_entry *g_cache;
static long                g_bytes;
static unsigned            g_tick;
static int                 g_disk_ok;              /* CACHE_DIR is writable      */

/* Files whose entries were evicted; deleted by the next cache_flush (owofs
 * one-stream rule keeps unlink off the fetch threads too). */
static char g_unlink[PEND_UNLINK_MAX][24];
static int  g_nunlink;

/* Spinlock (xchg + yield), matching resource.c's pool locks. Guards the list,
 * g_bytes, g_tick and the pending-unlink array against the fetch threads. */
static volatile int g_lock;
static int lk_xchg(volatile int *p, int v)
{ __asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory"); return v; }
static void lk_lock(void)   { while (lk_xchg(&g_lock, 1)) msleep(0); }
static void lk_unlock(void) { __asm__ volatile ("" ::: "memory"); g_lock = 0; }

/* Heap copy of a NUL-terminated string (NULL-safe: NULL -> NULL). */
static char *sdup(const char *s)
{
	if (!s) return 0;
	int n = strlen(s);
	char *d = malloc((unsigned)n + 1);
	if (d) memcpy(d, s, (unsigned)n + 1);
	return d;
}

static int is_get(const char *method)
{
	return !method || !method[0] || streq_ci(method, "GET");
}

/* FNV-1a of the URL -> 16 hex chars: the entry's stable on-disk filename, so a
 * re-store overwrites the same file and a reload maps back to it. */
static void disk_name(const char *url, char *out)
{
	unsigned long long h = 1469598103934665603ULL;
	for (const char *p = url; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) out[i] = hex[(h >> ((15 - i) * 4)) & 0xf];
	out[16] = '\0';
}

static void disk_path(const char *name, char *out, int cap)
{
	int n = 0; out[0] = '\0';
	sapp(out, &n, cap, CACHE_DIR "/");
	sapp(out, &n, cap, name);
}

static struct cache_entry *find(const char *url)
{
	for (struct cache_entry *e = g_cache; e; e = e->next)
		if (streq(e->url, url)) return e;
	return 0;
}

static void entry_free(struct cache_entry *e)
{
	free(e->url); free(e->ctype); free(e->hdr); free(e->body);
	free(e->etag); free(e->last_mod);
	free(e);
}

/* Queue an entry's disk file for deletion by the next flush (caller holds lock). */
static void queue_unlink(const char *name)
{
	if (!name || !name[0] || !g_disk_ok) return;
	if (g_nunlink >= PEND_UNLINK_MAX) return;          /* overflow: leave the file */
	int k = 0; g_unlink[g_nunlink][0] = '\0';
	sapp(g_unlink[g_nunlink], &k, (int)sizeof g_unlink[0], name);
	g_nunlink++;
}

/* Unlink `e` from the list, queue its file for deletion, and free it. */
static void unlink_free(struct cache_entry *e)
{
	struct cache_entry **pp = &g_cache;
	while (*pp && *pp != e) pp = &(*pp)->next;
	if (!*pp) return;
	*pp = e->next; g_bytes -= e->len;
	queue_unlink(e->disk);
	entry_free(e);
}

/* Evict least-recently-used entries until the new body would fit the budget. */
static void evict_for(int incoming)
{
	while (g_cache && g_bytes + incoming > CACHE_MAX_BYTES) {
		struct cache_entry *oldest = g_cache;
		for (struct cache_entry *e = g_cache->next; e; e = e->next)
			if (e->lru < oldest->lru) oldest = e;
		unlink_free(oldest);
	}
}

/* Current age of a stored entry now (RFC 7234 §4.2.3), from its frozen Age/Date
 * facts plus how long it has resided in the cache. */
static long entry_current_age(const struct cache_entry *e, long now)
{
	long apparent = e->stored_at - (e->date ? e->date : e->stored_at);
	if (apparent < 0) apparent = 0;
	long corrected = apparent > e->age ? apparent : e->age;
	long resident = now - e->stored_at;
	if (resident < 0) resident = 0;
	return corrected + resident;
}

/* Freeze the freshness facts from a parsed response into `e`. */
static void set_freshness(struct cache_entry *e, const struct http_cc *cc, long now)
{
	e->stored_at       = now;
	e->freshness       = http_cc_freshness(cc, now);
	e->date            = cc->date;
	e->age             = cc->age;
	e->no_cache        = cc->no_cache;
	e->must_revalidate = cc->must_revalidate;
	free(e->etag);     e->etag     = sdup(cc->etag);
	free(e->last_mod); e->last_mod = sdup(cc->last_mod_str);
}

/* Build the conditional-request header lines for a stale entry with a validator.
 * Lowercased names, so the one string is valid for HTTP/1.1 and HTTP/2 alike. */
static void build_cond(const struct cache_entry *e, char *cond, int cap)
{
	int n = 0;
	cond[0] = '\0';
	if (e->etag) {
		sapp(cond, &n, cap, "if-none-match: ");
		sapp(cond, &n, cap, e->etag);
		sapp(cond, &n, cap, "\r\n");
	}
	if (e->last_mod) {
		sapp(cond, &n, cap, "if-modified-since: ");
		sapp(cond, &n, cap, e->last_mod);
		sapp(cond, &n, cap, "\r\n");
	}
}

static void fill_out(const struct cache_entry *e,
                     unsigned char **body, int *len,
                     char *ctype, int ctype_cap, char *hdr, int hdr_cap)
{
	*body = malloc((unsigned)(e->len ? e->len : 1));
	if (*body && e->len) memcpy(*body, e->body, (unsigned)e->len);
	*len = *body ? e->len : 0;
	if (ctype && ctype_cap) { int k = 0; ctype[0] = '\0'; sapp(ctype, &k, ctype_cap, e->ctype ? e->ctype : ""); }
	if (hdr && hdr_cap)     { int k = 0; hdr[0]   = '\0'; sapp(hdr,   &k, hdr_cap,   e->hdr   ? e->hdr   : ""); }
}

int cache_lookup(const char *method, const char *url, long now,
                 unsigned char **body, int *len,
                 char *ctype, int ctype_cap, char *hdr, int hdr_cap,
                 char *cond, int cond_cap)
{
	*body = 0; *len = 0;
	if (cond && cond_cap) cond[0] = '\0';
	if (!is_get(method)) return CACHE_MISS;

	lk_lock();
	struct cache_entry *e = find(url);
	if (!e) { lk_unlock(); return CACHE_MISS; }
	e->lru = ++g_tick;

	int has_validator = e->etag || e->last_mod;
	int fresh = !e->no_cache && entry_current_age(e, now) < e->freshness;

	if (fresh) {
		fill_out(e, body, len, ctype, ctype_cap, hdr, hdr_cap);
		int ok = *body != 0;
		lk_unlock();
		return ok ? CACHE_FRESH : CACHE_MISS;
	}
	if (has_validator) {
		/* Stale but revalidatable. If the caller can carry conditional headers,
		 * ask it to revalidate; otherwise report a miss but KEEP the entry (a
		 * revalidation-capable caller may reuse it later). */
		if (cond && cond_cap) { build_cond(e, cond, cond_cap); lk_unlock(); return CACHE_REVALIDATE; }
		lk_unlock();
		return CACHE_MISS;
	}
	/* Stale with no way to revalidate: drop it and fetch fresh. */
	unlink_free(e);
	lk_unlock();
	return CACHE_MISS;
}

void cache_store(const char *method, const char *url, int status, long now,
                 const char *hdr_block, const char *ctype,
                 const unsigned char *body, int len)
{
	if (!is_get(method) || !url || len < 0 || len > CACHE_MAX_ONE) return;

	struct http_headers H;
	http_headers_parse(&H, hdr_block ? hdr_block : "");
	struct http_cc cc;
	http_cc_parse(&H, &cc);

	if (!http_cc_storable(&cc, status)) { http_headers_free(&H); return; }
	/* Vary means the response depends on request headers this URL-keyed cache
	 * doesn't match on — refuse it rather than risk serving the wrong variant. */
	const char *vary = http_headers_get(&H, "vary");
	if (vary && vary[0]) { http_headers_free(&H); return; }
	/* Worth keeping only if it can be served without a round trip (has some
	 * freshness) or revalidated cheaply (has a validator). */
	int worth = http_cc_freshness(&cc, now) > 0 || cc.etag || cc.last_mod_str;
	if (!worth) { http_headers_free(&H); return; }

	lk_lock();
	struct cache_entry *e = find(url);
	if (e) {                                    /* replace the stored body */
		unsigned char *nb = malloc((unsigned)(len ? len : 1));
		if (!nb) { lk_unlock(); http_headers_free(&H); return; }
		if (len) memcpy(nb, body, (unsigned)len);
		g_bytes -= e->len;
		free(e->body); e->body = nb; e->len = len;
		free(e->ctype); e->ctype = sdup(ctype);
		free(e->hdr);   e->hdr   = sdup(hdr_block);
		e->status = status;
		set_freshness(e, &cc, now);
		e->dirty = 1;
		e->lru = ++g_tick;
		g_bytes += len;
		lk_unlock();
		http_headers_free(&H);
		return;
	}

	evict_for(len);
	e = malloc(sizeof *e);
	if (!e) { lk_unlock(); http_headers_free(&H); return; }
	memset(e, 0, sizeof *e);
	e->url    = sdup(url);
	e->ctype  = sdup(ctype);
	e->hdr    = sdup(hdr_block);
	e->body   = malloc((unsigned)(len ? len : 1));
	if (!e->url || !e->body) { entry_free(e); lk_unlock(); http_headers_free(&H); return; }
	if (len) memcpy(e->body, body, (unsigned)len);
	e->len    = len;
	e->status = status;
	set_freshness(e, &cc, now);
	disk_name(url, e->disk);
	e->dirty  = 1;
	e->lru    = ++g_tick;
	e->next   = g_cache;
	g_cache   = e;
	g_bytes  += len;
	lk_unlock();
	http_headers_free(&H);
}

int cache_revalidated(const char *url, long now, const char *hdr_block,
                      unsigned char **body, int *len,
                      char *ctype, int ctype_cap, char *hdr, int hdr_cap)
{
	*body = 0; *len = 0;

	struct http_headers H;
	http_headers_parse(&H, hdr_block ? hdr_block : "");
	struct http_cc cc;
	http_cc_parse(&H, &cc);

	lk_lock();
	struct cache_entry *e = find(url);
	if (!e) { lk_unlock(); http_headers_free(&H); return 0; }
	set_freshness(e, &cc, now);          /* refresh freshness from the 304 */
	e->dirty = 1;                        /* freshness changed: rewrite front matter */
	e->lru = ++g_tick;
	fill_out(e, body, len, ctype, ctype_cap, hdr, hdr_cap);
	int ok = *body != 0;
	lk_unlock();
	http_headers_free(&H);
	return ok;
}

/* ------------------------------------------------------------- persistence */

/* Write one entry to its file: front matter (magic / url / status stored_at
 * hdr_len body_len / ctype), then the raw header block, then the body. Worker
 * thread only (owofs single-stream). Returns 0, or -1 on any write failure. */
static int write_entry(const struct cache_entry *e)
{
	char path[128];
	disk_path(e->disk, path, sizeof path);

	char fm[FM_MAX]; int n = 0;
	int hlen = e->hdr ? strlen(e->hdr) : 0;
	sapp(fm, &n, sizeof fm, CACHE_MAGIC "\n");
	sapp(fm, &n, sizeof fm, e->url); sapp(fm, &n, sizeof fm, "\n");
	uapp(fm, &n, sizeof fm, (unsigned)e->status);     sapp(fm, &n, sizeof fm, " ");
	uapp(fm, &n, sizeof fm, (unsigned)e->stored_at);  sapp(fm, &n, sizeof fm, " ");
	uapp(fm, &n, sizeof fm, (unsigned)hlen);          sapp(fm, &n, sizeof fm, " ");
	uapp(fm, &n, sizeof fm, (unsigned)e->len);        sapp(fm, &n, sizeof fm, "\n");
	sapp(fm, &n, sizeof fm, e->ctype ? e->ctype : ""); sapp(fm, &n, sizeof fm, "\n");

	if (stream_begin(path) != 0) return -1;
	if (stream_write(fm, n) < 0) { stream_end(); return -1; }
	if (hlen && stream_write(e->hdr, hlen) < 0) { stream_end(); return -1; }
	if (e->len && stream_write(e->body, e->len) < 0) { stream_end(); return -1; }
	return stream_end() < 0 ? -1 : 0;
}

void cache_flush(void)
{
	if (!g_disk_ok) return;
	lk_lock();
	for (struct cache_entry *e = g_cache; e; e = e->next)
		if (e->dirty && e->disk[0]) { if (write_entry(e) == 0) e->dirty = 0; }
	for (int i = 0; i < g_nunlink; i++) {
		char path[128]; disk_path(g_unlink[i], path, sizeof path);
		unlink(path);
	}
	g_nunlink = 0;
	lk_unlock();
}

/* Locate the four front-matter lines in `fm` (read from the file head) and parse
 * them. Fills url and ctype (into caller spans), status, stored_at, hdr_len,
 * body_len, and text_len = byte offset where the header block begins. Returns
 * 0, or -1 if the magic or shape is wrong. */
static int parse_front(const char *fm, int fmlen, char *url, int url_cap,
                       char *ctype, int ctype_cap, int *status, long *stored_at,
                       int *hdr_len, int *body_len, int *text_len)
{
	if (fmlen < 9 || !starts_ci(fm, CACHE_MAGIC "\n")) return -1;
	const char *nl[5]; int nn = 0;
	for (int i = 0; i < fmlen && nn < 5; i++) if (fm[i] == '\n') nl[nn++] = fm + i;
	if (nn < 4) return -1;                         /* need 4 lines of front matter */

	const char *u = nl[0] + 1; int ul = (int)(nl[1] - u);
	int k = 0; for (int i = 0; i < ul && k < url_cap - 1; i++) url[k++] = u[i]; url[k] = '\0';

	/* Line 3: four space-separated decimals. skip_field steps past one. */
	const char *m = nl[1] + 1;
	bool ok;
	*status = (int)http_parse_uint(m, &ok); if (!ok) return -1;
	for (int f = 0; f < 3; f++) {
		while (*m && *m != ' ') m++;
		while (*m == ' ') m++;
		long v = (long)http_parse_uint(m, &ok);
		if (f == 0) *stored_at = v; else if (f == 1) *hdr_len = (int)v; else *body_len = (int)v;
	}

	const char *c = nl[2] + 1; int cl = (int)(nl[3] - c);
	k = 0; for (int i = 0; i < cl && k < ctype_cap - 1; i++) ctype[k++] = c[i]; ctype[k] = '\0';

	*text_len = (int)(nl[3] + 1 - fm);
	if (*hdr_len < 0 || *body_len < 0 || *body_len > CACHE_MAX_ONE) return -1;
	return 0;
}

/* Load one cache file into an in-memory entry. Returns the entry (inserted into
 * g_cache, g_bytes updated) or NULL on any problem (the caller then unlinks the
 * bad/oversized file). Single-threaded startup, so no lock. */
static struct cache_entry *load_file(const char *name)
{
	char path[128]; disk_path(name, path, sizeof path);

	char fm[FM_MAX];
	int got = readfile_at(path, 0, fm, sizeof fm - 1);
	if (got <= 0) return 0;
	fm[got] = '\0';

	char url[URL_MAX]; char ctype[80];
	int status, hdr_len, body_len, text_len; long stored_at;
	if (parse_front(fm, got, url, sizeof url, ctype, sizeof ctype,
	                &status, &stored_at, &hdr_len, &body_len, &text_len) != 0)
		return 0;
	if (g_bytes + body_len > CACHE_MAX_BYTES) return 0;   /* over budget: drop it */

	char *hdr = malloc((unsigned)hdr_len + 1);
	unsigned char *body = malloc((unsigned)(body_len ? body_len : 1));
	if (!hdr || !body) { free(hdr); free(body); return 0; }
	if (hdr_len && readfile_at(path, (unsigned)text_len, hdr, (unsigned)hdr_len) != hdr_len)
		{ free(hdr); free(body); return 0; }
	hdr[hdr_len] = '\0';
	if (body_len && readfile_at(path, (unsigned)(text_len + hdr_len), body, (unsigned)body_len) != body_len)
		{ free(hdr); free(body); return 0; }

	struct cache_entry *e = malloc(sizeof *e);
	if (!e) { free(hdr); free(body); return 0; }
	memset(e, 0, sizeof *e);
	e->url = sdup(url); e->ctype = sdup(ctype); e->hdr = hdr;
	e->body = body; e->len = body_len; e->status = status;
	if (!e->url) { entry_free(e); return 0; }
	{ int k = 0; sapp(e->disk, &k, (int)sizeof e->disk, name); }

	/* Recompute the freshness facts from the stored headers, exactly as the
	 * original store did (now == stored_at when it was received). */
	struct http_headers H; http_headers_parse(&H, hdr);
	struct http_cc cc; http_cc_parse(&H, &cc);
	set_freshness(e, &cc, stored_at);
	http_headers_free(&H);

	e->lru = ++g_tick;
	e->next = g_cache; g_cache = e; g_bytes += body_len;
	return e;
}

void cache_load(void)
{
	/* Create the cache dir (parents first); if it can't be made, stay memory-only. */
	mkdir("/home/root/.cache");
	if (mkdir(CACHE_DIR) != 0) {
		struct statinfo si;
		if (stat(CACHE_DIR, &si) != 0) return;    /* no dir and can't create: no disk */
	}
	g_disk_ok = 1;

	struct dirent_u de;
	for (int i = 0; readdir(CACHE_DIR, i, &de) == 1; i++) {
		if (de.is_dir || !de.name[0]) continue;
		if (!load_file(de.name)) {                /* corrupt / oversized: remove it */
			char path[128]; disk_path(de.name, path, sizeof path);
			unlink(path);
		}
	}
}
