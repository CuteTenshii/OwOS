/* cors.c — CORS + Cross-Origin-Resource-Policy policy logic (see cors.h). Pure
 * decisions over parsed headers and URLs; the transport and the fetch/XHR
 * orchestration live in resource.c. */
#include "features/cors/cors.h"
#include "browser.h" /* parse_url, sapp/uapp, streq_ci, starts_ci, lower */
#include "headers.h"

void cors_origin(const char *url, char *out, int cap)
{
  struct purl u;
  int n = 0;
  out[0] = '\0';
  if (!url || !parse_url(url, &u) || !u.host[0]) {
    sapp(out, &n, cap, "null");
    return;
  }
  sapp(out, &n, cap, u.tls ? "https://" : "http://");
  sapp(out, &n, cap, u.host);
  if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) {
    sapp(out, &n, cap, ":");
    uapp(out, &n, cap, (unsigned)u.port);
  }
}

int cors_same_origin(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return 0;
  return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

/* The registrable domain of `host`, approximated as its last two dotted labels
 * ("a.b.example.com" -> "example.com"); a host with one label is returned
 * whole. */
static const char *registrable_domain(const char *host)
{
  const char *last = host, *prev = host;
  for (const char *p = host; *p; p++)
    if (*p == '.') {
      prev = last;
      last = p + 1;
    }
  return (last == host) ? host : prev; /* prev points at the second-to-last label */
}

int cors_same_site(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return 0;
  if (ua.tls != ub.tls)
    return 0; /* schemeful same-site */
  return streq_ci(registrable_domain(ua.host), registrable_domain(ub.host));
}

/* The media type of a Content-Type, lowercased into `out` (params after ';'
 * dropped, surrounding spaces trimmed). */
static void media_type(const char *ct, char *out, int cap)
{
  int k = 0;
  out[0] = '\0';
  if (!ct)
    return;
  while (*ct == ' ')
    ct++;
  for (; *ct && *ct != ';' && *ct != ' ' && k < cap - 1; ct++)
    out[k++] = lower(*ct);
  out[k] = '\0';
}

/* A CORS-safelisted *request* header name (always allowed without a preflight).
 */
static bool header_is_safelisted(const char *name, int len)
{
  static const char *const safe[] = {"accept", "accept-language", "content-language", "content-type", 0};
  for (int i = 0; safe[i]; i++) {
    int j = 0;
    for (; j < len && safe[i][j] && lower(name[j]) == safe[i][j]; j++)
      ;
    if (j == len && !safe[i][j])
      return true;
  }
  return false;
}

/* Call `fn(name, namelen, ctx)` for each "Name: value\r\n" line of an author
 * header blob; stop early if `fn` returns non-zero (which this then returns).
 */
static int for_each_header_name(const char *blob, int (*fn)(const char *, int, void *), void *ctx)
{
  const char *p = blob;
  while (p && *p) {
    while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
      p++;
    const char *name = p;
    while (*p && *p != ':' && *p != '\r' && *p != '\n')
      p++;
    int len = (int)(p - name);
    if (len > 0) {
      int r = fn(name, len, ctx);
      if (r)
        return r;
    }
    while (*p && *p != '\n')
      p++; /* to end of line */
    if (*p)
      p++;
  }
  return 0;
}

static int any_unsafe_cb(const char *name, int len, void *ctx)
{
  (void)ctx;
  return !header_is_safelisted(name, len); /* a non-safelisted header forces a preflight */
}

int cors_needs_preflight(const char *method, const char *req_ctype, const char *author_hdrs)
{
  if (!(streq_ci(method, "GET") || streq_ci(method, "HEAD") || streq_ci(method, "POST")))
    return 1;
  if (author_hdrs && for_each_header_name(author_hdrs, any_unsafe_cb, 0))
    return 1;
  if (req_ctype && req_ctype[0]) {
    char mt[64];
    media_type(req_ctype, mt, sizeof mt);
    if (!(streq(mt, "application/x-www-form-urlencoded") || streq(mt, "multipart/form-data") ||
          streq(mt, "text/plain")))
      return 1;
  }
  return 0;
}

struct collect_names {
  char *out;
  int cap, n;
};
static int collect_name_cb(const char *name, int len, void *ctx)
{
  struct collect_names *c = ctx;
  if (header_is_safelisted(name, len))
    return 0;
  if (c->n)
    sapp(c->out, &c->n, c->cap, ",");
  for (int i = 0; i < len && c->n < c->cap - 1; i++)
    c->out[c->n++] = lower(name[i]);
  c->out[c->n] = '\0';
  return 0;
}

void cors_request_header_names(const char *author_hdrs, char *out, int cap)
{
  struct collect_names c = {out, cap, 0};
  out[0] = '\0';
  if (author_hdrs)
    for_each_header_name(author_hdrs, collect_name_cb, &c);
}

/* Is `tok` (case-insensitive) a member of the comma-separated list `list`?
 * `wildcard_ok` lets a literal "*" entry match anything. */
static bool list_has(const char *list, const char *tok, bool wildcard_ok)
{
  if (!list)
    return false;
  const char *p = list;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == '\t')
      p++;
    const char *s = p;
    while (*p && *p != ',')
      p++;
    const char *e = p;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
      e--;
    int n = (int)(e - s);
    if (n == 1 && s[0] == '*' && wildcard_ok)
      return true;
    if (n > 0) {
      int j = 0;
      for (; j < n && tok[j] && lower(s[j]) == lower(tok[j]); j++)
        ;
      if (j == n && !tok[j])
        return true;
    }
    if (*p == ',')
      p++;
  }
  return false;
}

static void put_reason(char *reason, int rcap, const char *s)
{
  int k = 0;
  if (reason && rcap) {
    reason[0] = '\0';
    sapp(reason, &k, rcap, s);
  }
}

int cors_check_response(const struct http_headers *H, const char *origin, int with_credentials, char *reason, int rcap)
{
  const char *acao = http_headers_get(H, "access-control-allow-origin");
  if (!acao) {
    put_reason(reason, rcap, "no Access-Control-Allow-Origin");
    return 0;
  }

  if (with_credentials) {
    if (streq(acao, "*")) {
      put_reason(reason, rcap, "Access-Control-Allow-Origin '*' with credentials");
      return 0;
    }
    if (!streq_ci(acao, origin)) {
      put_reason(reason, rcap, "Access-Control-Allow-Origin mismatch");
      return 0;
    }
    const char *acac = http_headers_get(H, "access-control-allow-credentials");
    if (!acac || !streq_ci(acac, "true")) {
      put_reason(reason, rcap, "missing Access-Control-Allow-Credentials");
      return 0;
    }
    return 1;
  }
  if (streq(acao, "*") || streq_ci(acao, origin))
    return 1;
  put_reason(reason, rcap, "Access-Control-Allow-Origin mismatch");
  return 0;
}

struct unmatched_hdr {
  const struct http_headers *H;
  bool with_creds;
  const char *bad;
  int badlen;
};
static int header_not_allowed_cb(const char *name, int len, void *ctx)
{
  struct unmatched_hdr *u = ctx;
  if (header_is_safelisted(name, len))
    return 0;
  char nm[128];
  int k = 0;
  for (; k < len && k < (int)sizeof nm - 1; k++)
    nm[k] = name[k];
  nm[k] = '\0';
  const char *acah = http_headers_get(u->H, "access-control-allow-headers");
  if (list_has(acah, nm, !u->with_creds))
    return 0;
  u->bad = name;
  u->badlen = len; /* remember which header was refused */
  return 1;
}

int cors_check_preflight(const struct http_headers *H, const char *origin, const char *method, const char *author_hdrs,
                         int with_credentials, char *reason, int rcap)
{
  if (!cors_check_response(H, origin, with_credentials, reason, rcap))
    return 0;

  const char *acm = http_headers_get(H, "access-control-allow-methods");
  if (!(streq_ci(method, "GET") || streq_ci(method, "HEAD") || streq_ci(method, "POST")) &&
      !list_has(acm, method, !with_credentials)) {
    put_reason(reason, rcap, "method not allowed by preflight");
    return 0;
  }

  if (author_hdrs) {
    struct unmatched_hdr u = {H, with_credentials, 0, 0};
    if (for_each_header_name(author_hdrs, header_not_allowed_cb, &u)) {
      put_reason(reason, rcap, "header not allowed by preflight");
      return 0;
    }
  }
  return 1;
}

int corp_allows(const struct http_headers *H, const char *embedder_url, const char *resource_url, char *reason,
                int rcap)
{
  const char *corp = http_headers_get(H, "cross-origin-resource-policy");
  if (!corp)
    return 1;
  if (streq_ci(corp, "same-origin")) {
    if (cors_same_origin(embedder_url, resource_url))
      return 1;
    put_reason(reason, rcap, "Cross-Origin-Resource-Policy: same-origin");
    return 0;
  }
  if (streq_ci(corp, "same-site")) {
    if (cors_same_site(embedder_url, resource_url))
      return 1;
    put_reason(reason, rcap, "Cross-Origin-Resource-Policy: same-site");
    return 0;
  }
  return 1; /* "cross-origin" or unrecognised: allow */
}
