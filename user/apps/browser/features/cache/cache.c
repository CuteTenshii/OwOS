/* cache.c: persistent HTTP response cache; one body file per entry on owofs, index in memory.
 * Disk writes are deferred to cache_flush (owofs allows one open stream). */
#include "features/cache/cache.h"
#include "browser.h"
#include "headers.h"
#include "httpcache.h"
#include "mutex.h"

#ifndef CACHE_DIR
#define CACHE_DIR "/home/root/.cache/browser"
#endif
#define CACHE_MAX_BYTES (16 * 1024 * 1024)
#define CACHE_MAX_ONE (4 * 1024 * 1024)
#define CACHE_MAGIC "OWCACHE2"
#define PEND_UNLINK_MAX 128 /* deferred deletes between flushes       */
#define FM_MAX 3072         /* front-matter (magic+url+meta+ctype) cap */

/* One stored response; freshness facts are frozen at store time and recomputed on 304. */
struct cache_entry {
  char *url;
  int status;
  char *ctype;
  char *hdr; /* raw response header block            */
  unsigned char *body;
  int len;

  long stored_at; /* epoch seconds when received          */
  long freshness; /* lifetime seconds (from headers)      */
  long date, age; /* for current-age recomputation        */
  bool no_cache, must_revalidate;
  char *etag;     /* validator, or NULL                   */
  char *last_mod; /* validator, or NULL                   */

  struct http_vary vary; /* response Vary names                  */
  char *vkey;            /* secondary key of the request that fetched it ("" without Vary) */

  char disk[24];                   /* on-disk filename (hash of url + vkey), or "" */
  bool dirty;                      /* body/headers not yet written to disk */
  unsigned long long hash;         /* url_hash(url): hash-index + bucket key */
  struct cache_entry *next, *prev; /* LRU list (head = most-recently-used)   */
  struct cache_entry *hnext;       /* next entry sharing this url's bucket   */
};

/* LRU list plus a URL-hash bucket index over the same entries. */
static struct cache_entry *g_head, *g_tail;
#define CACHE_BUCKETS 256 /* power of two: url-hash index */
static struct cache_entry *g_bucket[CACHE_BUCKETS];
static long g_bytes;
static bool g_disk_ok; /* CACHE_DIR is writable      */

/* Files of evicted entries, deleted by the next cache_flush. */
static char g_unlink[PEND_UNLINK_MAX][24];
static int g_nunlink;

/* Guards the list, bucket index, g_bytes and pending unlinks against fetch threads. */
static mutex_t g_lock;

static char *sdup(const char *s)
{
  if (!s)
    return 0;
  int n = strlen(s);
  char *d = malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}

static bool is_get(const char *method) { return !method || !method[0] || streq_ci(method, "GET"); }

/* FNV-1a of the URL: keys the disk filename and the bucket. */
static unsigned long long url_hash(const char *url)
{
  unsigned long long h = 1469598103934665603ULL;
  for (const char *p = url; *p; p++) {
    h ^= (unsigned char)*p;
    h *= 1099511628211ULL;
  }
  return h;
}

/* Hash of url and variant key as 16 hex chars: the entry's stable on-disk filename. */
static void disk_name(const char *url, const char *vkey, char *out)
{
  unsigned long long h = url_hash(url);
  for (const char *p = vkey; *p; p++) {
    h ^= (unsigned char)*p;
    h *= 1099511628211ULL;
  }
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++)
    out[i] = hex[(h >> ((15 - i) * 4)) & 0xf];
  out[16] = '\0';
}

static unsigned bucket_of(unsigned long long h) { return (unsigned)(h & (CACHE_BUCKETS - 1)); }

static void lru_push_front(struct cache_entry *e)
{
  e->prev = 0;
  e->next = g_head;
  if (g_head)
    g_head->prev = e;
  else
    g_tail = e;
  g_head = e;
}
static void lru_remove(struct cache_entry *e)
{
  if (e->prev)
    e->prev->next = e->next;
  else
    g_head = e->next;
  if (e->next)
    e->next->prev = e->prev;
  else
    g_tail = e->prev;
  e->prev = e->next = 0;
}
static void lru_touch(struct cache_entry *e)
{
  if (g_head == e)
    return;
  lru_remove(e);
  lru_push_front(e);
}

static void idx_insert(struct cache_entry *e)
{
  unsigned b = bucket_of(e->hash);
  e->hnext = g_bucket[b];
  g_bucket[b] = e;
}
static void idx_remove(struct cache_entry *e)
{
  struct cache_entry **pp = &g_bucket[bucket_of(e->hash)];
  while (*pp && *pp != e)
    pp = &(*pp)->hnext;
  if (*pp)
    *pp = e->hnext;
  e->hnext = 0;
}

static void entry_link(struct cache_entry *e)
{
  e->hash = url_hash(e->url);
  idx_insert(e);
  lru_push_front(e);
}

static void disk_path(const char *name, char *out, int cap)
{
  int n = 0;
  out[0] = '\0';
  sapp(out, &n, cap, CACHE_DIR "/");
  sapp(out, &n, cap, name);
}

/* Does the request's header block select this variant (RFC 9111 4.1)? */
static bool variant_matches(const struct cache_entry *e, const char *req_hdrs)
{
  if (e->vary.star)
    return false;
  if (!e->vary.n)
    return true;
  char *k = http_vary_key(&e->vary, req_hdrs ? req_hdrs : "");
  bool ok = http_vary_key_match(k, e->vkey);
  free(k);
  return ok;
}

static struct cache_entry *find_match(const char *url, const char *req_hdrs)
{
  unsigned long long h = url_hash(url);
  for (struct cache_entry *e = g_bucket[bucket_of(h)]; e; e = e->hnext)
    if (e->hash == h && streq(e->url, url) && variant_matches(e, req_hdrs))
      return e;
  return 0;
}

static struct cache_entry *find_variant(const char *url, const char *vkey)
{
  unsigned long long h = url_hash(url);
  for (struct cache_entry *e = g_bucket[bucket_of(h)]; e; e = e->hnext)
    if (e->hash == h && streq(e->url, url) && streq(e->vkey, vkey))
      return e;
  return 0;
}

static void entry_free(struct cache_entry *e)
{
  free(e->url);
  free(e->ctype);
  free(e->hdr);
  free(e->body);
  free(e->etag);
  free(e->last_mod);
  free(e->vkey);
  http_vary_free(&e->vary);
  free(e);
}

/* Queue a file for deletion by the next flush; caller holds the lock. */
static void queue_unlink(const char *name)
{
  if (!name || !name[0] || !g_disk_ok)
    return;
  if (g_nunlink >= PEND_UNLINK_MAX)
    return;
  int k = 0;
  g_unlink[g_nunlink][0] = '\0';
  sapp(g_unlink[g_nunlink], &k, (int)sizeof g_unlink[0], name);
  g_nunlink++;
}

/* Unlink `e` from the list and index, queue its file for deletion, free it. */
static void unlink_free(struct cache_entry *e)
{
  lru_remove(e);
  idx_remove(e);
  g_bytes -= e->len;
  queue_unlink(e->disk);
  entry_free(e);
}

/* The server changed which fields it varies on: older variants of `url` no longer apply. */
static void drop_other_variants(const char *url, const struct http_vary *vary)
{
  unsigned long long h = url_hash(url);
  struct cache_entry *next;
  for (struct cache_entry *e = g_bucket[bucket_of(h)]; e; e = next) {
    next = e->hnext;
    if (e->hash == h && streq(e->url, url) && !http_vary_same(&e->vary, vary))
      unlink_free(e);
  }
}

/* Evict from the LRU tail until `incoming` fits the budget. */
static void evict_for(int incoming)
{
  while (g_tail && g_bytes + incoming > CACHE_MAX_BYTES)
    unlink_free(g_tail);
}

/* Current age per RFC 7234 section 4.2.3. */
static long entry_current_age(const struct cache_entry *e, long now)
{
  long apparent = e->stored_at - (e->date ? e->date : e->stored_at);
  if (apparent < 0)
    apparent = 0;
  long corrected = apparent > e->age ? apparent : e->age;
  long resident = now - e->stored_at;
  if (resident < 0)
    resident = 0;
  return corrected + resident;
}

static void set_freshness(struct cache_entry *e, const struct http_cc *cc, long now)
{
  e->stored_at = now;
  e->freshness = http_cc_freshness(cc, now);
  e->date = cc->date;
  e->age = cc->age;
  e->no_cache = cc->no_cache;
  e->must_revalidate = cc->must_revalidate;
  free(e->etag);
  e->etag = sdup(cc->etag);
  free(e->last_mod);
  e->last_mod = sdup(cc->last_mod_str);
}

/* Conditional-request lines, lowercased so valid for HTTP/1.1 and HTTP/2. */
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

static void fill_out(const struct cache_entry *e, unsigned char **body, int *len, char *ctype, int ctype_cap, char *hdr,
                     int hdr_cap)
{
  *body = malloc((unsigned)(e->len ? e->len : 1));
  if (*body && e->len)
    memcpy(*body, e->body, (unsigned)e->len);
  *len = *body ? e->len : 0;
  if (ctype && ctype_cap) {
    int k = 0;
    ctype[0] = '\0';
    sapp(ctype, &k, ctype_cap, e->ctype ? e->ctype : "");
  }
  if (hdr && hdr_cap) {
    int k = 0;
    hdr[0] = '\0';
    sapp(hdr, &k, hdr_cap, e->hdr ? e->hdr : "");
  }
}

int cache_lookup(const char *method, const char *url, const char *req_hdrs, long now, unsigned char **body, int *len,
                 char *ctype, int ctype_cap, char *hdr, int hdr_cap, char *cond, int cond_cap)
{
  *body = 0;
  *len = 0;
  if (cond && cond_cap)
    cond[0] = '\0';
  if (!is_get(method))
    return CACHE_MISS;

  mutex_lock(&g_lock);
  struct cache_entry *e = find_match(url, req_hdrs);
  if (!e) {
    mutex_unlock(&g_lock);
    return CACHE_MISS;
  }
  lru_touch(e);

  int has_validator = e->etag || e->last_mod;
  bool fresh = !e->no_cache && entry_current_age(e, now) < e->freshness;

  if (fresh) {
    fill_out(e, body, len, ctype, ctype_cap, hdr, hdr_cap);
    int ok = *body != 0;
    mutex_unlock(&g_lock);
    return ok ? CACHE_FRESH : CACHE_MISS;
  }
  if (has_validator) {
    /* without a cond buffer, report a miss but keep the entry */
    if (cond && cond_cap) {
      build_cond(e, cond, cond_cap);
      mutex_unlock(&g_lock);
      return CACHE_REVALIDATE;
    }
    mutex_unlock(&g_lock);
    return CACHE_MISS;
  }
  /* stale with no validator: drop */
  unlink_free(e);
  mutex_unlock(&g_lock);
  return CACHE_MISS;
}

/* Insert or replace the variant keyed by `vkey`; a new entry takes over *vary. Caller holds the lock. */
static void store_locked(const char *url, const char *vkey, struct http_vary *vary, int status, long now,
                         const char *hdr_block, const char *ctype, const unsigned char *body, int len,
                         const struct http_cc *cc)
{
  drop_other_variants(url, vary);
  unsigned char *nb = malloc((unsigned)(len ? len : 1));
  if (!nb)
    return;
  if (len)
    memcpy(nb, body, (unsigned)len);

  struct cache_entry *e = find_variant(url, vkey);
  if (e) {
    /* touch e first so it cannot evict itself */
    g_bytes -= e->len;
    lru_touch(e);
    evict_for(len);
    free(e->body);
    e->body = nb;
    e->len = len;
    free(e->ctype);
    e->ctype = sdup(ctype);
    free(e->hdr);
    e->hdr = sdup(hdr_block);
    e->status = status;
    set_freshness(e, cc, now);
    e->dirty = true;
    g_bytes += len;
    return;
  }

  evict_for(len);
  e = malloc(sizeof *e);
  if (!e) {
    free(nb);
    return;
  }
  memset(e, 0, sizeof *e);
  e->url = sdup(url);
  e->vkey = sdup(vkey);
  e->ctype = sdup(ctype);
  e->hdr = sdup(hdr_block);
  e->body = nb;
  if (!e->url || !e->vkey) {
    entry_free(e);
    return;
  }
  e->vary = *vary;
  memset(vary, 0, sizeof *vary);
  e->len = len;
  e->status = status;
  set_freshness(e, cc, now);
  disk_name(url, vkey, e->disk);
  e->dirty = true;
  entry_link(e);
  g_bytes += len;
}

void cache_store(const char *method, const char *url, const char *req_hdrs, int status, long now, const char *hdr_block,
                 const char *ctype, const unsigned char *body, int len)
{
  if (!is_get(method) || !url || len < 0 || len > CACHE_MAX_ONE)
    return;

  struct http_headers H;
  http_headers_parse(&H, hdr_block ? hdr_block : "");
  struct http_cc cc;
  http_cc_parse(&H, &cc);
  struct http_vary vary;
  char *vkey = 0;
  if (http_vary_parse(&vary, &H) == 0 && !vary.star)
    vkey = http_vary_key(&vary, req_hdrs ? req_hdrs : "");

  /* worth keeping only with some freshness or a validator */
  bool worth = http_cc_freshness(&cc, now) > 0 || cc.etag || cc.last_mod_str;
  if (vkey && http_cc_storable(&cc, status) && worth) {
    mutex_lock(&g_lock);
    store_locked(url, vkey, &vary, status, now, hdr_block, ctype, body, len, &cc);
    mutex_unlock(&g_lock);
  }
  free(vkey);
  http_vary_free(&vary);
  http_headers_free(&H);
}

int cache_revalidated(const char *url, const char *req_hdrs, long now, const char *hdr_block, unsigned char **body,
                      int *len, char *ctype, int ctype_cap, char *hdr, int hdr_cap)
{
  *body = 0;
  *len = 0;

  struct http_headers H;
  http_headers_parse(&H, hdr_block ? hdr_block : "");
  struct http_cc cc;
  http_cc_parse(&H, &cc);

  mutex_lock(&g_lock);
  struct cache_entry *e = find_match(url, req_hdrs);
  if (!e) {
    mutex_unlock(&g_lock);
    http_headers_free(&H);
    return 0;
  }
  set_freshness(e, &cc, now);
  e->dirty = true;
  lru_touch(e);
  fill_out(e, body, len, ctype, ctype_cap, hdr, hdr_cap);
  int ok = *body != 0;
  mutex_unlock(&g_lock);
  http_headers_free(&H);
  return ok;
}

/* Write front matter (magic, url, numbers, ctype), variant key, header block, then body. Worker thread only. */
static int write_entry(const struct cache_entry *e)
{
  char path[128];
  disk_path(e->disk, path, sizeof path);

  char fm[FM_MAX];
  int n = 0;
  int hlen = e->hdr ? strlen(e->hdr) : 0;
  int klen = strlen(e->vkey);
  sapp(fm, &n, sizeof fm, CACHE_MAGIC "\n");
  sapp(fm, &n, sizeof fm, e->url);
  sapp(fm, &n, sizeof fm, "\n");
  uapp(fm, &n, sizeof fm, (unsigned)e->status);
  sapp(fm, &n, sizeof fm, " ");
  uapp(fm, &n, sizeof fm, (unsigned)e->stored_at);
  sapp(fm, &n, sizeof fm, " ");
  uapp(fm, &n, sizeof fm, (unsigned)hlen);
  sapp(fm, &n, sizeof fm, " ");
  uapp(fm, &n, sizeof fm, (unsigned)e->len);
  sapp(fm, &n, sizeof fm, " ");
  uapp(fm, &n, sizeof fm, (unsigned)klen);
  sapp(fm, &n, sizeof fm, "\n");
  sapp(fm, &n, sizeof fm, e->ctype ? e->ctype : "");
  sapp(fm, &n, sizeof fm, "\n");

  if (stream_begin(path) != 0)
    return -1;
  if (stream_write(fm, n) < 0) {
    stream_end();
    return -1;
  }
  if (klen && stream_write(e->vkey, klen) < 0) {
    stream_end();
    return -1;
  }
  if (hlen && stream_write(e->hdr, hlen) < 0) {
    stream_end();
    return -1;
  }
  if (e->len && stream_write(e->body, e->len) < 0) {
    stream_end();
    return -1;
  }
  return stream_end() < 0 ? -1 : 0;
}

void cache_flush(void)
{
  if (!g_disk_ok)
    return;
  mutex_lock(&g_lock);
  for (struct cache_entry *e = g_head; e; e = e->next)
    if (e->dirty && e->disk[0]) {
      if (write_entry(e) == 0)
        e->dirty = false;
    }
  for (int i = 0; i < g_nunlink; i++) {
    char path[128];
    disk_path(g_unlink[i], path, sizeof path);
    unlink(path);
  }
  g_nunlink = 0;
  mutex_unlock(&g_lock);
}

/* Parse the front-matter lines; text_len is where the header block begins. 0, or -1 if malformed. */
static int parse_front(const char *fm, int fmlen, char *url, int url_cap, char *ctype, int ctype_cap, int *status,
                       long *stored_at, int *hdr_len, int *body_len, int *key_len, int *text_len)
{
  if (fmlen < 9 || !starts_ci(fm, CACHE_MAGIC "\n"))
    return -1;
  const char *nl[5];
  int nn = 0;
  for (int i = 0; i < fmlen && nn < 5; i++)
    if (fm[i] == '\n')
      nl[nn++] = fm + i;
  if (nn < 4)
    return -1;

  const char *u = nl[0] + 1;
  int ul = (int)(nl[1] - u);
  int k = 0;
  for (int i = 0; i < ul && k < url_cap - 1; i++)
    url[k++] = u[i];
  url[k] = '\0';

  const char *m = nl[1] + 1;
  bool ok;
  *status = (int)http_parse_uint(m, &ok);
  if (!ok)
    return -1;
  for (int f = 0; f < 4; f++) {
    while (*m && *m != ' ')
      m++;
    while (*m == ' ')
      m++;
    long v = (long)http_parse_uint(m, &ok);
    if (f == 0)
      *stored_at = v;
    else if (f == 1)
      *hdr_len = (int)v;
    else if (f == 2)
      *body_len = (int)v;
    else
      *key_len = (int)v;
  }

  const char *c = nl[2] + 1;
  int cl = (int)(nl[3] - c);
  k = 0;
  for (int i = 0; i < cl && k < ctype_cap - 1; i++)
    ctype[k++] = c[i];
  ctype[k] = '\0';

  *text_len = (int)(nl[3] + 1 - fm);
  if (*hdr_len < 0 || *hdr_len > RESP_HDR_MAX || *body_len < 0 || *body_len > CACHE_MAX_ONE || *key_len < 0 ||
      *key_len > CACHE_MAX_ONE)
    return -1;
  return 0;
}

/* Load one cache file into a linked entry, or NULL (caller then deletes the file). Startup only. */
static struct cache_entry *load_file(const char *name)
{
  char path[128];
  disk_path(name, path, sizeof path);

  char fm[FM_MAX];
  int got = readfile_at(path, 0, fm, sizeof fm - 1);
  if (got <= 0)
    return 0;
  fm[got] = '\0';

  char url[URL_MAX];
  char ctype[80];
  int status, hdr_len, body_len = 0, key_len, text_len;
  long stored_at = 0;
  if (parse_front(fm, got, url, sizeof url, ctype, sizeof ctype, &status, &stored_at, &hdr_len, &body_len, &key_len,
                  &text_len) != 0)
    return 0;
  if (g_bytes + body_len > CACHE_MAX_BYTES)
    return 0;

  char *vkey = malloc((unsigned)key_len + 1);
  char *hdr = malloc((unsigned)hdr_len + 1);
  unsigned char *body = malloc((unsigned)(body_len ? body_len : 1));
  struct cache_entry *e = 0;
  struct http_headers H = {0};
  if (!vkey || !hdr || !body)
    goto out;
  if (key_len && readfile_at(path, (unsigned)text_len, vkey, (unsigned)key_len) != key_len)
    goto out;
  vkey[key_len] = '\0';
  if (hdr_len && readfile_at(path, (unsigned)(text_len + key_len), hdr, (unsigned)hdr_len) != hdr_len)
    goto out;
  hdr[hdr_len] = '\0';
  if (body_len && readfile_at(path, (unsigned)(text_len + key_len + hdr_len), body, (unsigned)body_len) != body_len)
    goto out;

  e = malloc(sizeof *e);
  if (!e)
    goto out;
  memset(e, 0, sizeof *e);
  e->url = sdup(url);
  e->ctype = sdup(ctype);
  e->hdr = hdr;
  e->body = body;
  e->vkey = vkey;
  hdr = 0;
  body = 0;
  vkey = 0;
  e->len = body_len;
  e->status = status;

  http_headers_parse(&H, e->hdr);
  if (!e->url || http_vary_parse(&e->vary, &H) < 0 || e->vary.star || find_variant(e->url, e->vkey)) {
    entry_free(e);
    e = 0;
    goto out;
  }
  {
    int k = 0;
    sapp(e->disk, &k, (int)sizeof e->disk, name);
  }
  struct http_cc cc;
  http_cc_parse(&H, &cc);
  set_freshness(e, &cc, stored_at);
  entry_link(e);
  g_bytes += body_len;

out:
  http_headers_free(&H);
  free(vkey);
  free(hdr);
  free(body);
  return e;
}

void cache_load(void)
{
  mkdir("/home/root/.cache");
  if (mkdir(CACHE_DIR) != 0) {
    struct statinfo si;
    if (stat(CACHE_DIR, &si) != 0)
      return;
  }
  g_disk_ok = true;

  struct dirent_u de;
  for (int i = 0; readdir(CACHE_DIR, i, &de) == 1; i++) {
    if (de.is_dir || !de.name[0])
      continue;
    if (!load_file(de.name)) {
      char path[128];
      disk_path(de.name, path, sizeof path);
      unlink(path);
    }
  }
}
