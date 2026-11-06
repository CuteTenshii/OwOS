/* jar.c — the browser's persistent cookie + HSTS store.
 *
 * Keeps the cookie/HSTS data model AND its on-disk persistence out of the HTTP
 * transport (fetch.c), which only ever calls cookie_store/cookie_header/
 * hsts_add/hsts_has. Both in-memory lists are uncapped heap linked lists
 * (B->cookies, B->hsts) — there is no fixed jar size and no fixed field width.
 *
 * On disk both live in ONE real SQLite3 database (user/lib/sqlite), the file
 * JAR_DB, as two tables: `cookies(host, name, value, expiry, secure, domain,
 * path)` and `hsts(host)`. Old 3-column cookie rows still load (the extra
 * attributes default). It is a genuine SQLite file — `sqlite3 cookies.db` on
 * the host opens and queries it.
 * The OwOS file API is whole-file, so every change rebuilds and rewrites the
 * whole db; the jar is small, so that is cheap. All persistence is best-effort:
 * a read/write/allocation failure just leaves the in-memory list as-is, so the
 * browser still runs without a writable home directory.
 */
#include "jar.h"
#include "browser.h"
#include "httpcache.h" /* http_date_parse: Expires -> Unix epoch seconds */
#include "sqlite.h"

/* Mirrors the browser's existing /home/root convention (see DL_DIR for
 * downloads). The file name stays cookies.db even though it now also holds the
 * HSTS table — it is the browser's jar. */
#define JAR_DB "/home/root/cookies.db"
#define COOKIE_SQL                                                                                                     \
  "CREATE TABLE cookies(host TEXT, name TEXT, value TEXT, "                                                            \
  "expiry INTEGER, secure INTEGER, domain TEXT, path TEXT)"
#define COOKIE_COLS 7
#define HSTS_SQL "CREATE TABLE hsts(host TEXT)"

/* Heap copy of a byte range as a NUL-terminated string (no length cap). */
static char *dupn(const char *s, int n)
{
  char *d = (char *)malloc((unsigned)(n + 1));
  if (!d)
    return 0;
  for (int i = 0; i < n; i++)
    d[i] = s[i];
  d[n] = '\0';
  return d;
}
static char *dupz(const char *s) { return dupn(s, strlen(s)); }

/* Set when the in-memory jar changes; jar_flush() writes the db once, later.
 * This keeps the expensive whole-file rewrite OUT of the per-header parse loop
 * — a response with several Set-Cookie lines now costs one write, not one each.
 */
static bool jar_dirty;

/* Per-cookie attributes that struct cookie (browser.h: host/name/val only)
 * can't carry, kept in a side list keyed by the cookie node. jar.c owns every
 * cookie node's whole lifecycle, so the two lists stay in lockstep; a cookie
 * with no meta node is a plain session, host-only, non-Secure, any-path cookie
 * (the defaults), so meta is allocated only when an attribute needs it. */
struct cookie_meta {
  struct cookie *c; /* the B->cookies node this describes         */
  long expires;     /* Unix epoch secs; 0 = session (no expiry)   */
  bool secure;      /* send only over TLS                         */
  char *domain;     /* NULL = host-only; else host suffix-match   */
  char *path;       /* NULL = any path; else request-path prefix  */
  struct cookie_meta *next;
};
static struct cookie_meta *cookie_metas;

static struct cookie_meta *meta_find(struct cookie *c)
{
  for (struct cookie_meta *m = cookie_metas; m; m = m->next)
    if (m->c == c)
      return m;
  return 0;
}

/* Drop c's meta node (freeing its owned domain/path). No-op if it has none. */
static void meta_detach(struct cookie *c)
{
  struct cookie_meta **pp = &cookie_metas;
  while (*pp) {
    if ((*pp)->c == c) {
      struct cookie_meta *m = *pp;
      *pp = m->next;
      free(m->domain);
      free(m->path);
      free(m);
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Attach attributes to c, taking ownership of `domain`/`path`. Replaces any
 * previous meta. When everything is default it stores none (and frees the
 * passed-in strings). On OOM the cookie simply keeps the defaults. */
static void meta_set(struct cookie *c, long expires, bool secure, char *domain, char *path)
{
  meta_detach(c);
  if (!expires && !secure && !domain && !path) {
    free(domain);
    free(path);
    return;
  }
  struct cookie_meta *m = (struct cookie_meta *)malloc(sizeof *m);
  if (!m) {
    free(domain);
    free(path);
    return;
  }
  m->c = c;
  m->expires = expires;
  m->secure = secure;
  m->domain = domain;
  m->path = path;
  m->next = cookie_metas;
  cookie_metas = m;
}

static void meta_clear(void)
{
  struct cookie_meta *m = cookie_metas;
  while (m) {
    struct cookie_meta *n = m->next;
    free(m->domain);
    free(m->path);
    free(m);
    m = n;
  }
  cookie_metas = 0;
}

/* --------------------------------------------------------------- cookie jar */

/* Case-insensitive compare of the [s, s+n) slice against the NUL-terminated
 * literal `lit` (attribute names aren't NUL-terminated in the header). */
static bool attr_is(const char *s, int n, const char *lit)
{
  int i = 0;
  for (; i < n && lit[i]; i++)
    if (lower(s[i]) != lower(lit[i]))
      return false;
  return i == n && lit[i] == '\0';
}

/* Parse a (possibly signed) decimal from the [s, s+n) slice; *ok=false if it
 * isn't a well-formed integer. */
static long parse_long(const char *s, int n, bool *ok)
{
  int i = 0;
  bool neg = false;
  long r = 0;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    neg = (s[i] == '-');
    i++;
  }
  if (i >= n) {
    *ok = false;
    return 0;
  }
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      *ok = false;
      return 0;
    }
    r = r * 10 + (s[i] - '0');
  }
  *ok = true;
  return neg ? -r : r;
}

/* True if `host` equals `domain` or is a sub-domain of it (label-boundary
 * suffix match), case-insensitively — RFC 6265 domain-match. */
static bool domain_match(const char *host, const char *domain)
{
  int hl = (int)strlen(host), dl = (int)strlen(domain);
  if (dl == 0 || dl > hl)
    return false;
  if (dl == hl)
    return streq_ci(host, domain);
  if (host[hl - dl - 1] != '.')
    return false;
  return streq_ci(host + hl - dl, domain);
}

/* RFC 6265 path-match: `rpath` is at/under `cpath`. Query/fragment end the
 * request path. Case-sensitive (paths are). NULL/empty cpath matches all. */
static bool path_match(const char *cpath, const char *rpath)
{
  if (!cpath || !*cpath)
    return true;
  int i = 0;
  for (; cpath[i]; i++)
    if (rpath[i] != cpath[i])
      return false;
  char after = rpath[i];
  if (after == '\0' || after == '/' || after == '?' || after == '#')
    return true;
  return cpath[i - 1] == '/';
}

/* Remove any stored (host, name) cookie (and its meta). Used to honour a
 * Set-Cookie that is already expired (a deletion request). */
static void cookie_remove(const char *host, const char *name)
{
  struct cookie **pp = &B->cookies;
  while (*pp) {
    struct cookie *c = *pp;
    if (streq_ci(c->host, host) && streq(c->name, name)) {
      *pp = c->next;
      meta_detach(c);
      free(c->host);
      free(c->name);
      free(c->val);
      free(c);
      jar_dirty = true;
      return;
    }
    pp = &c->next;
  }
}

void cookie_store(const char *host, const char *v)
{
  while (*v == ' ')
    v++;
  const char *ns = v;
  while (*v && *v != '=' && *v != ';')
    v++;
  int nlen = (int)(v - ns);
  if (*v != '=' || nlen == 0)
    return;
  char *name = dupn(ns, nlen);
  if (!name)
    return;
  v++;
  const char *vs = v;
  while (*v && *v != ';' && *v != '\r' && *v != '\n')
    v++;
  char *val = dupn(vs, (int)(v - vs));
  if (!val) {
    free(name);
    return;
  }

  /* Attributes after the first ';' (Max-Age/Expires/Secure/Domain/Path; other
   * attributes such as HttpOnly/SameSite are accepted and ignored). */
  bool have_maxage = false;
  long maxage = 0;
  bool have_expires = false;
  long expires_at = 0;
  bool secure = false;
  char *domain = 0, *path = 0;

  for (const char *a = v; *a == ';';) {
    a++;
    while (*a == ' ' || *a == '\t')
      a++;
    const char *an = a;
    while (*a && *a != '=' && *a != ';')
      a++;
    int anlen = (int)(a - an);
    const char *av = 0;
    int avlen = 0;
    if (*a == '=') {
      a++;
      av = a;
      while (*a && *a != ';')
        a++;
      avlen = (int)(a - av);
      while (avlen > 0 && (*av == ' ' || *av == '\t')) {
        av++;
        avlen--;
      }
      while (avlen > 0 && (av[avlen - 1] == ' ' || av[avlen - 1] == '\t'))
        avlen--;
    }

    if (attr_is(an, anlen, "max-age") && av) {
      bool ok;
      long m = parse_long(av, avlen, &ok);
      if (ok) {
        have_maxage = true;
        maxage = m;
      } /* takes precedence over Expires */
    } else if (attr_is(an, anlen, "expires") && av) {
      char *ds = dupn(av, avlen);
      if (ds) {
        long e = http_date_parse(ds);
        free(ds);
        if (e > 0) {
          have_expires = true;
          expires_at = e;
        }
      }
    } else if (attr_is(an, anlen, "secure")) {
      secure = true;
    } else if (attr_is(an, anlen, "domain") && av && avlen > 0) {
      if (*av == '.') {
        av++;
        avlen--;
      } /* leading dot is ignored */
      free(domain);
      domain = avlen > 0 ? dupn(av, avlen) : 0;
    } else if (attr_is(an, anlen, "path") && av && avlen > 0) {
      free(path);
      path = dupn(av, avlen);
    }
  }

  /* A Domain that isn't a suffix of the origin host is rejected (a site can't
   * set cookies for an unrelated domain). The public-suffix list is not
   * consulted, so Domain=<eTLD> isn't blocked — same approximation the CORS
   * code documents. */
  if (domain && !domain_match(host, domain)) {
    free(name);
    free(val);
    free(domain);
    free(path);
    return;
  }

  long now = (long)rtc_epoch();
  long expiry = 0; /* 0 = session cookie (no persisted expiry) */
  bool expired = false;
  if (have_maxage) {
    if (maxage <= 0)
      expired = true;
    else
      expiry = now + maxage;
  } else if (have_expires) {
    expiry = expires_at;
    if (expires_at <= now)
      expired = true;
  }

  /* An already-expired Set-Cookie is a deletion request: drop the match. */
  if (expired) {
    cookie_remove(host, name);
    free(name);
    free(val);
    free(domain);
    free(path);
    return;
  }

  /* Update in place if this (host, name) is already stored. */
  for (struct cookie *c = B->cookies; c; c = c->next) {
    if (streq_ci(c->host, host) && streq(c->name, name)) {
      free(c->val);
      c->val = val;
      free(name);
      meta_set(c, expiry, secure, domain, path);
      jar_dirty = true;
      return;
    }
  }
  /* Otherwise prepend a fresh node. */
  struct cookie *c = (struct cookie *)malloc(sizeof *c);
  if (!c) {
    free(name);
    free(val);
    free(domain);
    free(path);
    return;
  }
  c->host = dupz(host);
  c->name = name;
  c->val = val;
  if (!c->host) {
    free(c->name);
    free(c->val);
    free(domain);
    free(path);
    free(c);
    return;
  }
  c->next = B->cookies;
  B->cookies = c;
  meta_set(c, expiry, secure, domain, path);
  jar_dirty = true;
}

/* Build the Cookie: header value for a request. Skips cookies that have
 * expired, that a Domain/Path scope excludes, or (when !tls) that are Secure.
 * `path` may be NULL to skip Path filtering (callers without the request path).
 */
int cookie_header_tls(const char *host, const char *path, int tls, char *out, int cap)
{
  long now = (long)rtc_epoch();
  int n = 0;
  for (struct cookie *c = B->cookies; c; c = c->next) {
    struct cookie_meta *m = meta_find(c);
    if (m && m->expires > 0 && m->expires <= now)
      continue; /* expired    */
    if (m && m->domain) {
      if (!domain_match(host, m->domain))
        continue;
    } else {
      if (!streq_ci(c->host, host))
        continue;
    }
    if (m && m->secure && !tls)
      continue; /* Secure     */
    if (m && m->path && path && !path_match(m->path, path))
      continue; /* Path scope */
    if (n)
      sapp(out, &n, cap, "; ");
    sapp(out, &n, cap, c->name);
    sapp(out, &n, cap, "=");
    sapp(out, &n, cap, c->val);
  }
  return n;
}

/* Legacy scheme/path-agnostic variant (browser.h API). Assumes the request is
 * secure so it stays a no-op change for existing callers (resource.c). */
int cookie_header(const char *host, char *out, int cap) { return cookie_header_tls(host, 0, 1, out, cap); }

/* Free the in-memory cookie jar (used before reloading it from disk). */
static void cookie_clear(void)
{
  struct cookie *c = B->cookies;
  while (c) {
    struct cookie *n = c->next;
    free(c->host);
    free(c->name);
    free(c->val);
    free(c);
    c = n;
  }
  B->cookies = 0;
  meta_clear();
}

/* ----------------------------------------------- HSTS (force-HTTPS hosts) */

int hsts_has(const char *host)
{
  for (struct hsts_host *h = B->hsts; h; h = h->next)
    if (streq_ci(h->host, host))
      return 1;
  return 0;
}

void hsts_add(const char *host)
{
  if (hsts_has(host))
    return;
  struct hsts_host *h = (struct hsts_host *)malloc(sizeof *h);
  if (!h)
    return;
  h->host = dupz(host);
  if (!h->host) {
    free(h);
    return;
  }
  h->next = B->hsts;
  B->hsts = h;
  jar_dirty = true;
}

/* Free the in-memory HSTS list. */
static void hsts_clear(void)
{
  struct hsts_host *h = B->hsts;
  while (h) {
    struct hsts_host *n = h->next;
    free(h->host);
    free(h);
    h = n;
  }
  B->hsts = 0;
}

/* ----------------------------------------------------- SQLite persistence */

/* Load the persistent jar (cookies + HSTS) from the SQLite db at startup. A
 * missing or corrupt db simply leaves both lists empty. No caps on either. */
void jar_load(void)
{
  struct sql_db *db = sql_open(JAR_DB);
  if (!db)
    return;
  cookie_clear();
  hsts_clear();

  long now = (long)rtc_epoch();
  struct sql_table *t = sql_table_find(db, "cookies");
  if (t) {
    for (struct sql_row *r = t->rows; r; r = r->next) {
      if (r->ncol < 3)
        continue;
      if (r->col[0].type != SQL_TEXT || r->col[1].type != SQL_TEXT || r->col[2].type != SQL_TEXT)
        continue;
      /* Columns 3-6 are absent in the old 3-column schema — read what a
       * row carries and default the rest. */
      long expiry = (r->ncol >= 4 && r->col[3].type == SQL_INT) ? (long)r->col[3].i : 0;
      if (expiry > 0 && expiry <= now)
        continue; /* drop expired on load */
      bool secure = (r->ncol >= 5 && r->col[4].type == SQL_INT && r->col[4].i != 0);
      char *domain = (r->ncol >= 6 && r->col[5].type == SQL_TEXT) ? dupz(r->col[5].text) : 0;
      char *path = (r->ncol >= 7 && r->col[6].type == SQL_TEXT) ? dupz(r->col[6].text) : 0;
      struct cookie *c = (struct cookie *)malloc(sizeof *c);
      if (!c) {
        free(domain);
        free(path);
        break;
      }
      c->host = dupz(r->col[0].text);
      c->name = dupz(r->col[1].text);
      c->val = dupz(r->col[2].text);
      if (!c->host || !c->name || !c->val) {
        free(c->host);
        free(c->name);
        free(c->val);
        free(c);
        free(domain);
        free(path);
        continue;
      }
      c->next = B->cookies;
      B->cookies = c;
      meta_set(c, expiry, secure, domain, path);
    }
  }

  struct sql_table *ht = sql_table_find(db, "hsts");
  if (ht) {
    for (struct sql_row *r = ht->rows; r; r = r->next) {
      if (r->ncol < 1 || r->col[0].type != SQL_TEXT)
        continue;
      struct hsts_host *h = (struct hsts_host *)malloc(sizeof *h);
      if (!h)
        break;
      h->host = dupz(r->col[0].text);
      if (!h->host) {
        free(h);
        continue;
      }
      h->next = B->hsts;
      B->hsts = h;
    }
  }
  sql_close(db);
}

/* Rewrite the jar db (cookies + HSTS tables) from the in-memory lists. */
void jar_save(void)
{
  struct sql_db *db = sql_new();
  if (!db)
    return;
  long now = (long)rtc_epoch();
  struct sql_table *t = sql_create_table(db, "cookies", COOKIE_SQL, COOKIE_COLS);
  if (t) {
    for (struct cookie *c = B->cookies; c; c = c->next) {
      struct cookie_meta *m = meta_find(c);
      if (m && m->expires > 0 && m->expires <= now)
        continue; /* self-clean expired */
      struct sql_value v[COOKIE_COLS];
      v[0] = sqlv_text(c->host);
      v[1] = sqlv_text(c->name);
      v[2] = sqlv_text(c->val);
      v[3] = sqlv_int(m ? m->expires : 0);
      v[4] = sqlv_int(m && m->secure ? 1 : 0);
      v[5] = (m && m->domain) ? sqlv_text(m->domain) : sqlv_null();
      v[6] = (m && m->path) ? sqlv_text(m->path) : sqlv_null();
      sql_insert(t, v, COOKIE_COLS);
    }
  }
  struct sql_table *ht = sql_create_table(db, "hsts", HSTS_SQL, 1);
  if (ht) {
    for (struct hsts_host *h = B->hsts; h; h = h->next) {
      struct sql_value v[1];
      v[0] = sqlv_text(h->host);
      sql_insert(ht, v, 1);
    }
  }
  sql_save(db, JAR_DB);
  sql_close(db);
}

/* Persist the jar to disk only if it changed since the last flush. Call once
 * after handling a response, not once per cookie. */
void jar_flush(void)
{
  if (!jar_dirty)
    return;
  jar_dirty = false;
  jar_save();
}
