/* httpcache.c - the RFC 7234 caching policy declared in httpcache.h.
 *
 * Pure computation over a parsed response: no I/O, no clock (the caller passes
 * `now`); only the Vary key builders allocate. Every fiddly bit of the spec - the three date grammars,
 * the freshness-lifetime precedence, the current-age formula - lives here once
 * so a cache just stores bytes and asks these functions fresh-or-stale. */
#include "httpcache.h"
#include "owos.h"

static int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_dig(int c) { return c >= '0' && c <= '9'; }

static long rd_num(const char **p)
{
  long v = 0;
  while (is_dig((unsigned char)**p)) {
    v = v * 10 + (**p - '0');
    (*p)++;
  }
  return v;
}

/* First three letters (case-insensitive) name a month -> 1..12, else 0. */
static int month_num(const char *s)
{
  static const char *const m[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  if (!s[0] || !s[1] || !s[2])
    return 0; /* don't read past a truncated header */
  for (int i = 0; i < 12; i++)
    if (lower(s[0]) == m[i][0] && lower(s[1]) == m[i][1] && lower(s[2]) == m[i][2])
      return i + 1;
  return 0;
}

/* Days from 1970-01-01 to civil y-m-d (Howard Hinnant's algorithm). */
static long days_from_civil(long y, int m, int d)
{
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

long http_date_parse(const char *s)
{
  if (!s)
    return 0;
  const char *p = s;
  while (*p == ' ')
    p++;
  while (is_alpha((unsigned char)*p))
    p++; /* weekday name */
  while (*p == ',' || *p == ' ')
    p++;

  int day = 0, mon = 0, hh = 0, mm = 0, ss = 0;
  long year = 0;

  if (is_alpha((unsigned char)*p)) { /* asctime: Nov  6 08:49:37 1994 */
    mon = month_num(p);
    while (is_alpha((unsigned char)*p))
      p++;
    while (*p == ' ')
      p++;
    day = (int)rd_num(&p);
    while (*p == ' ')
      p++;
    hh = (int)rd_num(&p);
    if (*p == ':') {
      p++;
      mm = (int)rd_num(&p);
    }
    if (*p == ':') {
      p++;
      ss = (int)rd_num(&p);
    }
    while (*p == ' ')
      p++;
    year = rd_num(&p);
  } else { /* 06 Nov 1994 / 06-Nov-94 */
    day = (int)rd_num(&p);
    while (*p == '-' || *p == ' ')
      p++;
    mon = month_num(p);
    while (is_alpha((unsigned char)*p))
      p++;
    while (*p == '-' || *p == ' ')
      p++;
    year = rd_num(&p);
    while (*p == ' ')
      p++;
    hh = (int)rd_num(&p);
    if (*p == ':') {
      p++;
      mm = (int)rd_num(&p);
    }
    if (*p == ':') {
      p++;
      ss = (int)rd_num(&p);
    }
  }

  if (mon < 1 || mon > 12 || day < 1 || day > 31)
    return 0;
  if (hh > 23 || mm > 59 || ss > 60)
    return 0; /* 60 = leap second */
  if (year < 100)
    year += (year < 70) ? 2000 : 1900; /* RFC 850 two-digit year */

  return days_from_civil(year, mon, day) * 86400L + hh * 3600 + mm * 60 + ss;
}

/* Value of a Cache-Control directive that carries "=<digits>" (max-age,
 * s-maxage). Returns 1 + *out on success, 0 if the directive is absent. Matched
 * only at a comma/space/start boundary so "max-age" doesn't match inside
 * another token. */
static int cc_num_directive(const char *cc, const char *name, long *out)
{
  int nl = strlen(name);
  for (const char *p = cc; *p; p++) {
    if (p != cc && p[-1] != ',' && p[-1] != ' ' && p[-1] != '\t')
      continue;
    if (!starts_ci(p, name))
      continue;
    const char *q = p + nl;
    while (*q == ' ' || *q == '\t')
      q++;
    if (*q != '=')
      continue;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '"')
      q++;
    if (!is_dig((unsigned char)*q))
      continue;
    *out = rd_num(&q);
    return 1;
  }
  return 0;
}

void http_cc_parse(const struct http_headers *H, struct http_cc *cc)
{
  memset(cc, 0, sizeof *cc);

  const char *ccv = http_headers_get(H, "cache-control");
  if (ccv) {
    cc->no_store = http_header_has_token(ccv, "no-store");
    cc->no_cache = http_header_has_token(ccv, "no-cache");
    cc->must_revalidate = http_header_has_token(ccv, "must-revalidate");
    cc->is_private = http_header_has_token(ccv, "private");
    long ma;
    if (cc_num_directive(ccv, "max-age", &ma)) {
      cc->has_max_age = 1;
      cc->max_age = ma;
    }
  }
  /* HTTP/1.0 "Pragma: no-cache" is equivalent to Cache-Control: no-cache. */
  const char *pragma = http_headers_get(H, "pragma");
  if (pragma && http_header_has_token(pragma, "no-cache"))
    cc->no_cache = 1;

  cc->date = http_date_parse(http_headers_get(H, "date"));

  const char *exp = http_headers_get(H, "expires");
  if (exp) {
    cc->has_expires = 1;
    cc->expires = http_date_parse(exp);
  }

  bool ok;
  cc->age = (long)http_parse_uint(http_headers_get(H, "age"), &ok);
  if (!ok)
    cc->age = 0;

  cc->last_mod_str = http_headers_get(H, "last-modified");
  cc->last_modified = http_date_parse(cc->last_mod_str);
  cc->etag = http_headers_get(H, "etag");
}

int http_cc_storable(const struct http_cc *cc, int status)
{
  if (cc->no_store)
    return 0;
  return status == 200;
}

long http_cc_freshness(const struct http_cc *cc, long now)
{
  if (cc->has_max_age)
    return cc->max_age > 0 ? cc->max_age : 0;
  if (cc->has_expires) {
    if (!cc->expires)
      return 0; /* present but invalid = expired */
    long base = cc->date ? cc->date : now;
    long f = cc->expires - base;
    return f > 0 ? f : 0;
  }
  /* Heuristic (RFC 7234 §4.2.2): 10% of the time since Last-Modified. */
  if (cc->last_modified && cc->date) {
    long f = (cc->date - cc->last_modified) / 10;
    if (f < 0)
      f = 0;
    if (f > 86400)
      f = 86400; /* cap the guess at one day */
    return f;
  }
  return 0;
}

long http_cc_current_age(const struct http_cc *cc, long stored_at, long now)
{
  long apparent = stored_at - (cc->date ? cc->date : stored_at);
  if (apparent < 0)
    apparent = 0;
  long corrected = apparent > cc->age ? apparent : cc->age;
  long resident = now - stored_at;
  if (resident < 0)
    resident = 0;
  return corrected + resident;
}

static bool is_ws(char c) { return c == ' ' || c == '\t'; }

static int vary_add(struct http_vary *v, const char *s, int len)
{
  char *nm = malloc((unsigned)len + 1);
  if (!nm)
    return -1;
  for (int i = 0; i < len; i++)
    nm[i] = lower(s[i]);
  nm[len] = '\0';
  int at = 0;
  while (at < v->n && strcmp(v->names[at], nm) < 0)
    at++;
  if (at < v->n && streq(v->names[at], nm)) {
    free(nm);
    return 0;
  }
  char **nn = realloc(v->names, (unsigned)(v->n + 1) * sizeof *nn);
  if (!nn) {
    free(nm);
    return -1;
  }
  v->names = nn;
  memmove(nn + at + 1, nn + at, (unsigned)(v->n - at) * sizeof *nn);
  nn[at] = nm;
  v->n++;
  return 0;
}

int http_vary_parse(struct http_vary *v, const struct http_headers *H)
{
  memset(v, 0, sizeof *v);
  int rc = 0;
  for (int i = 0, lines = http_headers_count(H, "vary"); i < lines; i++) {
    const char *p = http_headers_at(H, "vary", i);
    while (*p) {
      while (is_ws(*p) || *p == ',')
        p++;
      const char *s = p;
      while (*p && *p != ',')
        p++;
      const char *e = p;
      while (e > s && is_ws(e[-1]))
        e--;
      if (e == s)
        continue;
      if (e - s == 1 && *s == '*')
        v->star = true;
      else if (vary_add(v, s, (int)(e - s)) < 0)
        rc = -1;
    }
  }
  return rc;
}

void http_vary_free(struct http_vary *v)
{
  for (int i = 0; i < v->n; i++)
    free(v->names[i]);
  free(v->names);
  memset(v, 0, sizeof *v);
}

bool http_vary_same(const struct http_vary *a, const struct http_vary *b)
{
  if (a->star != b->star || a->n != b->n)
    return false;
  for (int i = 0; i < a->n; i++)
    if (!streq(a->names[i], b->names[i]))
      return false;
  return true;
}

struct kbuf {
  char *p;
  int n, cap;
};

static bool kput(struct kbuf *k, char c)
{
  if (k->n + 1 >= k->cap) {
    int nc = k->cap ? k->cap * 2 : 64;
    char *np = realloc(k->p, (unsigned)nc);
    if (!np)
      return false;
    k->p = np;
    k->cap = nc;
  }
  k->p[k->n++] = c;
  return true;
}

/* Fields whose values are case-insensitive comma lists. */
static bool list_field(const char *name)
{
  return streq(name, "accept") || streq(name, "accept-charset") || streq(name, "accept-encoding") ||
         streq(name, "accept-language");
}

/* Append one field value: trimmed, whitespace runs collapsed; lists also lowercased and tight around commas. */
static bool kput_value(struct kbuf *k, const char *s, const char *e, bool list)
{
  bool gap = false, any = false;
  for (; s < e; s++) {
    if (is_ws(*s)) {
      gap = true;
      continue;
    }
    if (gap && any && !(list && (*s == ',' || k->p[k->n - 1] == ',')) && !kput(k, ' '))
      return false;
    if (!kput(k, list ? (char)lower(*s) : *s))
      return false;
    gap = false;
    any = true;
  }
  return true;
}

static bool name_is(const char *s, int len, const char *lname)
{
  if ((int)strlen(lname) != len)
    return false;
  for (int i = 0; i < len; i++)
    if (lower(s[i]) != lname[i])
      return false;
  return true;
}

/* Append "name:" then '-' (absent) or '+' and the comma-joined values of every line named `name`. */
static bool kput_field(struct kbuf *k, const char *name, const char *req_hdrs)
{
  for (const char *p = name; *p; p++)
    if (!kput(k, *p))
      return false;
  if (!kput(k, ':') || !kput(k, '-'))
    return false;
  int mark = k->n - 1;
  bool list = list_field(name);
  for (const char *l = req_hdrs; l && *l && *l != '\r' && *l != '\n';) {
    const char *eol = l;
    while (*eol && *eol != '\n')
      eol++;
    const char *le = eol;
    if (le > l && le[-1] == '\r')
      le--;
    const char *colon = l;
    while (colon < le && *colon != ':')
      colon++;
    const char *ne = colon;
    while (ne > l && is_ws(ne[-1]))
      ne--;
    if (colon < le && name_is(l, (int)(ne - l), name)) {
      if (k->p[mark] == '-')
        k->p[mark] = '+';
      else if (!kput(k, ','))
        return false;
      if (!kput_value(k, colon + 1, le, list))
        return false;
    }
    l = *eol ? eol + 1 : eol;
  }
  return kput(k, '\n');
}

char *http_vary_key(const struct http_vary *v, const char *req_hdrs)
{
  if (v->star)
    return 0;
  struct kbuf k = {0, 0, 0};
  bool ok = true;
  for (int i = 0; ok && i < v->n; i++)
    ok = kput_field(&k, v->names[i], req_hdrs);
  if (!ok || !kput(&k, '\0')) {
    free(k.p);
    return 0;
  }
  return k.p;
}

bool http_vary_key_match(const char *a, const char *b) { return a && b && streq(a, b); }
