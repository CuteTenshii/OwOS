/* jar.c: persistent cookie and HSTS store (SQLite file JAR_DB), best-effort, whole-file rewrite. */
#include "features/jar/jar.h"
#include "browser.h"
#include "httpcache.h" /* http_date_parse: Expires -> Unix epoch seconds */
#include "sqlite.h"

/* Browser home-dir convention; cookies.db also holds the HSTS table. */
#define JAR_DB "/home/root/cookies.db"
#define COOKIE_SQL                                                                                                     \
  "CREATE TABLE cookies(host TEXT, name TEXT, value TEXT, "                                                            \
  "expiry INTEGER, secure INTEGER, domain TEXT, path TEXT)"
#define COOKIE_COLS 7
#define HSTS_SQL "CREATE TABLE hsts(host TEXT)"

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

/* Set on change; jar_flush() writes the db once, not per Set-Cookie header. */
static bool jar_dirty;

/* Per-cookie attributes beyond struct cookie, in a side list; absent meta = session/host-only/non-Secure/any-path. */
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

/* Attach attributes to c, taking ownership of `domain`/`path`; all-default stores none. */
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

/* Case-insensitive compare of the [s, s+n) slice against literal `lit`. */
static bool attr_is(const char *s, int n, const char *lit)
{
  int i = 0;
  for (; i < n && lit[i]; i++)
    if (lower(s[i]) != lower(lit[i]))
      return false;
  return i == n && lit[i] == '\0';
}

/* Parse a signed decimal from the slice; *ok=false if malformed. */
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

/* RFC 6265 domain-match: `host` equals or is a sub-domain of `domain`. */
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

/* RFC 6265 path-match; query/fragment end `rpath`; NULL/empty cpath matches all. */
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

/* Remove any stored (host, name) cookie and its meta. */
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

  /* Max-Age/Expires/Secure/Domain/Path are honoured; other attributes are ignored */
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

  /* reject a Domain that isn't a suffix of the origin host (public-suffix list not consulted) */
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

  if (expired) {
    cookie_remove(host, name);
    free(name);
    free(val);
    free(domain);
    free(path);
    return;
  }

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

/* Build the Cookie: header value; `path` NULL skips Path filtering. */
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

/* Legacy variant (browser.h API): assumes a secure request. */
int cookie_header(const char *host, char *out, int cap) { return cookie_header_tls(host, 0, 1, out, cap); }

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

/* Load the persistent jar at startup; a missing or corrupt db leaves the lists empty. */
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
      /* columns 3-6 are absent in the old 3-column schema */
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

/* Persist the jar only if it changed since the last flush. */
void jar_flush(void)
{
  if (!jar_dirty)
    return;
  jar_dirty = false;
  jar_save();
}
