/* csp.c — Content-Security-Policy engine (see csp.h). Parses one or more
 * policies and answers per-resource load/run decisions with the full source
 * grammar. */
#include "csp.h"
#include "base64.h" /* shared base64 for the digest encoding */
#include "crypto.h" /* sha256 for 'sha256-...' inline hashes */
#include "dom.h"    /* lower / starts_ci / streq_ci */
#include "owos.h"
#include "url.h"

/* One policy = a value string per directive (NULL = unset) + the two flag
 * directives. The whole CSP is the set of policies; a load must pass them all.
 */
struct policy {
  char *dir[CSP_NTYPES]; /* raw value (space-separated sources), or NULL */
  char *form;            /* form-action value, or NULL                  */
  int upgrade;           /* upgrade-insecure-requests                    */
  int block_mixed;       /* block-all-mixed-content                      */
};
struct csp {
  struct policy *p;
  int n, cap;
};

static char *dupn(const char *s, int n)
{
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return 0;
  for (int i = 0; i < n; i++)
    d[i] = s[i];
  d[n] = '\0';
  return d;
}

csp *csp_new(void)
{
  csp *c = malloc(sizeof *c);
  if (!c)
    return 0;
  c->p = 0;
  c->n = 0;
  c->cap = 0;
  return c;
}

void csp_free(csp *c)
{
  if (!c)
    return;
  for (int i = 0; i < c->n; i++) {
    for (int d = 0; d < CSP_NTYPES; d++)
      free(c->p[i].dir[d]);
    free(c->p[i].form);
  }
  free(c->p);
  free(c);
}

/* Map a directive name to a CSP_* slot, or -1 (flag/unknown handled by caller).
 */
static int dir_index(const char *name, int len)
{
  struct {
    const char *n;
    int i;
  } map[] = {{"default-src", CSP_DEFAULT},
             {"script-src", CSP_SCRIPT},
             {"script-src-elem", CSP_SCRIPT},
             {"style-src", CSP_STYLE},
             {"style-src-elem", CSP_STYLE},
             {"img-src", CSP_IMG},
             {"font-src", CSP_FONT},
             {"connect-src", CSP_CONNECT},
             {"frame-src", CSP_FRAME},
             {"child-src", CSP_CHILD},
             {"media-src", CSP_MEDIA},
             {"object-src", CSP_OBJECT},
             {0, 0}};
  for (int i = 0; map[i].n; i++) {
    const char *nm = map[i].n;
    int k = 0;
    while (k < len && nm[k] && lower(name[k]) == lower(nm[k]))
      k++;
    if (k == len && !nm[k])
      return map[i].i;
  }
  return -1;
}

void csp_add(csp *c, const char *policy)
{
  if (!c || !policy)
    return;
  if (c->n >= c->cap) {
    int nc = c->cap ? c->cap * 2 : 2;
    struct policy *np = realloc(c->p, (unsigned)nc * sizeof *np);
    if (!np)
      return;
    c->p = np;
    c->cap = nc;
  }
  struct policy *pol = &c->p[c->n];
  for (int d = 0; d < CSP_NTYPES; d++)
    pol->dir[d] = 0;
  pol->form = 0;
  pol->upgrade = 0;
  pol->block_mixed = 0;

  /* Split on ';' into directives; each is "name value...". */
  const char *p = policy;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ';')
      p++;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != ';')
      p++;
    const char *segend = p; /* [seg,segend) is one directive */

    const char *ns = seg; /* directive name */
    while (ns < segend && (*ns == ' ' || *ns == '\t'))
      ns++;
    const char *ne = ns;
    while (ne < segend && *ne != ' ' && *ne != '\t')
      ne++;
    int nlen = (int)(ne - ns);
    if (nlen <= 0)
      continue;

    const char *vs = ne; /* value (trimmed) */
    while (vs < segend && (*vs == ' ' || *vs == '\t'))
      vs++;
    const char *ve = segend;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t'))
      ve--;

    /* Flag directives carry no value. */
    int nl = nlen;
    if (nl == 25 && starts_ci(ns, "upgrade-insecure-requests")) {
      pol->upgrade = 1;
      continue;
    }
    if (nl == 23 && starts_ci(ns, "block-all-mixed-content")) {
      pol->block_mixed = 1;
      continue;
    }
    if (nl == 12 && starts_ci(ns, "form-action")) {
      free(pol->form);
      pol->form = dupn(vs, (int)(ve - vs));
      continue;
    }

    int di = dir_index(ns, nlen);
    if (di < 0)
      continue; /* frame-ancestors/base-uri/report/etc: ignored here */
    free(pol->dir[di]);
    pol->dir[di] = dupn(vs, (int)(ve - vs));
  }
  c->n++;
}

int csp_empty(const csp *c)
{
  if (!c)
    return 1;
  for (int i = 0; i < c->n; i++) {
    struct policy *pol = &c->p[i];
    if (pol->upgrade || pol->block_mixed || pol->form)
      return 0;
    for (int d = 0; d < CSP_NTYPES; d++)
      if (pol->dir[d])
        return 0;
  }
  return 1;
}

/* The source value that governs `type` under one policy, applying the fallback
 * chain. form-action has no fallback; fetch directives fall back to
 * default-src; frame falls back through child-src to default-src. NULL =
 * unrestricted. */
static const char *effective(const struct policy *pol, int type)
{
  if (type == CSP_FORM)
    return pol->form;
  if (pol->dir[type])
    return pol->dir[type];
  if (type == CSP_FRAME) {
    if (pol->dir[CSP_CHILD])
      return pol->dir[CSP_CHILD];
    return pol->dir[CSP_DEFAULT];
  }
  if (type == CSP_DEFAULT)
    return 0;
  return pol->dir[CSP_DEFAULT]; /* script/style/img/font/connect/media/object */
}

/* --- token helpers ---------------------------------------------------------
 */
static const char *tok_next(const char *p, const char **ts, const char **te)
{
  while (*p == ' ' || *p == '\t')
    p++;
  if (!*p)
    return 0;
  *ts = p;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  *te = p;
  return p;
}
static int tok_is(const char *ts, const char *te, const char *lit)
{
  int n = (int)(te - ts);
  for (int i = 0; i < n; i++) {
    if (!lit[i] || lower(ts[i]) != lower(lit[i]))
      return 0;
  }
  return lit[n] == 0;
}
static int tok_starts(const char *ts, const char *te, const char *pre)
{
  int n = (int)(te - ts), i = 0;
  for (; pre[i]; i++) {
    if (i >= n || lower(ts[i]) != lower(pre[i]))
      return 0;
  }
  return 1;
}

/* --- origin / host-source matching -----------------------------------------
 */
static int origin_eq(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return 0;
  return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

/* Match a host-source token `s` (NUL-terminated, may carry scheme/port/path)
 * against absolute http(s) URL `url`. */
static int host_source_match(const char *s, const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return 0;

  /* Optional "scheme://" prefix. */
  int want_tls = -1; /* -1 = no scheme given */
  const char *h = s;
  const char *sep = 0;
  for (const char *q = s; q[0] && q[1] && q[2]; q++)
    if (q[0] == ':' && q[1] == '/' && q[2] == '/') {
      sep = q;
      break;
    }
  if (sep) {
    int slen = (int)(sep - s);
    if (slen == 5 && starts_ci(s, "https"))
      want_tls = 1;
    else if (slen == 4 && starts_ci(s, "http"))
      want_tls = 0;
    else if (slen == 3 && starts_ci(s, "wss"))
      want_tls = 1;
    else if (slen == 2 && starts_ci(s, "ws"))
      want_tls = 0;
    else
      return 0; /* a scheme we never speak */
    h = sep + 3;
  }
  if (want_tls >= 0 && u.tls != want_tls)
    return 0;

  /* host (up to ':' or '/'), then optional :port, then optional /path. */
  char host[256];
  int hi = 0;
  while (*h && *h != ':' && *h != '/' && hi < (int)sizeof host - 1)
    host[hi++] = *h++;
  host[hi] = '\0';

  char port[16];
  int pi = 0;
  if (*h == ':') {
    h++;
    while (*h && *h != '/' && pi < (int)sizeof port - 1)
      port[pi++] = *h++;
  }
  port[pi] = '\0';

  const char *path = (*h == '/') ? h : 0; /* remaining = path */

  /* host */
  if (host[0] == '*' && host[1] == '\0') {
    /* any host */
  } else if (host[0] == '*' && host[1] == '.') {
    const char *suf = host + 1; /* ".rest" */
    int ul = (int)strlen(u.host), sl = (int)strlen(suf);
    if (ul <= sl || !streq_ci(u.host + (ul - sl), suf))
      return 0;
  } else if (!streq_ci(u.host, host)) {
    return 0;
  }

  /* port */
  if (port[0]) {
    if (!(port[0] == '*' && port[1] == '\0')) {
      int pn = 0;
      for (const char *q = port; *q >= '0' && *q <= '9'; q++)
        pn = pn * 10 + (*q - '0');
      if (pn != u.port)
        return 0;
    }
  } else {
    if (u.port != (u.tls ? 443 : 80))
      return 0; /* omitted port = default only */
  }

  /* path (query stripped); trailing '/' = prefix, else exact-or-segment. */
  if (path && !(path[0] == '/' && path[1] == '\0')) {
    char pp[512];
    int k = 0;
    for (const char *q = path; *q && *q != '?' && *q != '#' && k < (int)sizeof pp - 1; q++)
      pp[k++] = *q;
    pp[k] = '\0';
    int pl = k, ulen = (int)strlen(u.path);
    if (pl > 0 && pp[pl - 1] == '/') {
      if (ulen < pl)
        return 0;
      for (int i = 0; i < pl; i++)
        if (u.path[i] != pp[i])
          return 0;
    } else {
      if (!streq(u.path, pp)) {
        if (ulen <= pl)
          return 0;
        for (int i = 0; i < pl; i++)
          if (u.path[i] != pp[i])
            return 0;
        if (u.path[pl] != '/')
          return 0;
      }
    }
  }
  return 1;
}

/* A scheme-source token like "https:" / "data:" — matches by URL scheme. */
static int scheme_source_match(const char *s, const char *url)
{
  int n = (int)strlen(s);
  if (n < 2 || s[n - 1] != ':')
    return 0; /* must be "<scheme>:" */
  char sc[24];
  int k = 0;
  for (int i = 0; i < n - 1 && k < (int)sizeof sc - 1; i++)
    sc[k++] = lower(s[i]);
  sc[k] = '\0';
  if (streq(sc, "https"))
    return starts_ci(url, "https://");
  if (streq(sc, "http"))
    return starts_ci(url, "http://");
  if (streq(sc, "wss"))
    return starts_ci(url, "wss://");
  if (streq(sc, "ws"))
    return starts_ci(url, "ws://");
  /* data:/blob:/filesystem:/mediastream: etc — literal scheme prefix. */
  char pre[26];
  int j = 0;
  for (int i = 0; sc[i] && j < (int)sizeof pre - 2; i++)
    pre[j++] = sc[i];
  pre[j++] = ':';
  pre[j] = '\0';
  return starts_ci(url, pre);
}

/* Does the source token [ts,te) admit URL `url` (with `page_url` for 'self')?
 */
static int url_source_admits(const char *ts, const char *te, const char *url, const char *page_url)
{
  if (tok_is(ts, te, "'self'"))
    return origin_eq(url, page_url);
  if (ts[0] == '\'')
    return 0; /* unsafe-inline/eval/nonce/hash: not for URLs */
  if (te - ts == 1 && ts[0] == '*')
    return starts_ci(url, "http://") || starts_ci(url, "https://") || starts_ci(url, "ws://") ||
           starts_ci(url, "wss://");
  char s[512];
  int n = (int)(te - ts);
  if (n > (int)sizeof s - 1)
    n = (int)sizeof s - 1;
  for (int i = 0; i < n; i++)
    s[i] = ts[i];
  s[n] = '\0';

  /* "<scheme>:" with no host is a scheme-source; anything with "://" or a bare
   * host is a host-source. */
  int has_sep = 0;
  for (const char *q = s; q[0] && q[1] && q[2]; q++)
    if (q[0] == ':' && q[1] == '/' && q[2] == '/') {
      has_sep = 1;
      break;
    }
  if (!has_sep && s[n - 1] == ':')
    return scheme_source_match(s, url);
  return host_source_match(s, url);
}

/* Match a URL against a directive value's source list (no inline keywords). */
static int sources_admit_url(const char *val, const char *url, const char *page_url)
{
  const char *p = val, *ts, *te;
  int found = 0;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'none'"))
      return 0;
    if (url_source_admits(ts, te, url, page_url))
      found = 1;
    p = te;
  }
  return found;
}

/* Does the source list `val` carry a 'nonce-<nonce>' matching `nonce`? A nonce
 * on a <script src>/<link> element admits it regardless of the host-source
 * list. */
static int sources_admit_nonce(const char *val, const char *nonce)
{
  if (!nonce || !*nonce)
    return 0;
  int nl = (int)strlen(nonce);
  const char *p = val, *ts, *te;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_starts(ts, te, "'nonce-")) {
      const char *vs = ts + 7; /* after "'nonce-" */
      const char *ve = te;
      if (ve > vs && ve[-1] == '\'')
        ve--;
      if (ve - vs == nl) {
        int eq = 1;
        for (int i = 0; i < nl; i++)
          if (vs[i] != nonce[i]) {
            eq = 0;
            break;
          }
        if (eq)
          return 1;
      }
    }
    p = te;
  }
  return 0;
}

/* Does the source list `val` carry 'strict-dynamic'? When present, host-source
 * and scheme-source expressions (and 'self') are ignored for scripts: only
 * nonces, hashes, and scripts injected by an already-trusted script are
 * admitted. */
static int sources_strict_dynamic(const char *val)
{
  const char *p = val, *ts, *te;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'strict-dynamic'"))
      return 1;
    p = te;
  }
  return 0;
}

/* --- base64 (for sha256 hashes) --------------------------------------------
 */

/* Canonical (padless, +/ alphabet) base64 of a digest, into dst — the form CSP
 * 'sha256-...' tokens are compared in after b64_canon_token normalizes them. */
static void b64_canon_digest(const unsigned char *d, int n, char *dst)
{
  int l = base64_encode(d, n, dst);
  while (l > 0 && dst[l - 1] == '=')
    l--; /* CSP hashes carry no padding */
  dst[l] = '\0';
}

/* Canonicalise a CSP hash payload: map base64url to base64 and drop '='. */
static void b64_canon_token(const char *ts, const char *te, char *dst, int cap)
{
  int o = 0;
  for (const char *q = ts; q < te && o < cap - 1; q++) {
    char c = *q;
    if (c == '=')
      continue;
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
    dst[o++] = c;
  }
  dst[o] = '\0';
}

/* Decide inline allowance for one policy's directive value. */
static int inline_allowed_by(const char *val, const char *nonce, const unsigned char *body, int len)
{
  const char *p = val, *ts, *te;
  int unsafe_inline = 0, has_nh = 0, ok = 0;
  char myhash[64];
  int have_hash = 0;

  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'none'"))
      return 0;
    else if (tok_is(ts, te, "'unsafe-inline'"))
      unsafe_inline = 1;
    else if (tok_starts(ts, te, "'nonce-")) {
      has_nh = 1;
      if (nonce && *nonce) {
        const char *vs = ts + 7; /* after "'nonce-" */
        const char *ve = te;
        if (ve > vs && ve[-1] == '\'')
          ve--;
        int nl = (int)strlen(nonce);
        if (ve - vs == nl) {
          int eq = 1;
          for (int i = 0; i < nl; i++)
            if (vs[i] != nonce[i]) {
              eq = 0;
              break;
            }
          if (eq)
            ok = 1;
        }
      }
    } else if (tok_starts(ts, te, "'sha256-")) {
      has_nh = 1;
      if (body) {
        if (!have_hash) {
          unsigned char dg[SHA256_DIGEST];
          sha256(body, (unsigned)len, dg);
          b64_canon_digest(dg, SHA256_DIGEST, myhash);
          have_hash = 1;
        }
        const char *vs = ts + 8; /* after "'sha256-" */
        const char *ve = te;
        if (ve > vs && ve[-1] == '\'')
          ve--;
        char tk[64];
        b64_canon_token(vs, ve, tk, sizeof tk);
        if (streq(tk, myhash))
          ok = 1;
      }
    } else if (tok_starts(ts, te, "'sha384-") || tok_starts(ts, te, "'sha512-")) {
      has_nh = 1; /* present but unverifiable here */
    }
    p = te;
  }
  if (ok)
    return 1;
  if (unsafe_inline && !has_nh)
    return 1; /* nonce/hash present => unsafe-inline ignored */
  return 0;
}

/* --- public checks ---------------------------------------------------------
 */
static const char *dir_name(int type)
{
  switch (type) {
  case CSP_SCRIPT:
    return "script-src";
  case CSP_STYLE:
    return "style-src";
  case CSP_IMG:
    return "img-src";
  case CSP_FONT:
    return "font-src";
  case CSP_CONNECT:
    return "connect-src";
  case CSP_FRAME:
    return "frame-src";
  case CSP_FORM:
    return "form-action";
  case CSP_MEDIA:
    return "media-src";
  case CSP_OBJECT:
    return "object-src";
  default:
    return "default-src";
  }
}

static int is_local_host(const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return 0;
  return streq_ci(u.host, "localhost") || streq_ci(u.host, "127.0.0.1") || streq_ci(u.host, "::1");
}

int csp_check_url(const csp *c, int type, const char *abs_url, const char *page_url, char *out, int out_cap,
                  char *reason, int rcap)
{
  {
    int k = 0;
    out[0] = '\0';
    while (abs_url[k] && k < out_cap - 1) {
      out[k] = abs_url[k];
      k++;
    }
    out[k] = '\0';
  }
  if (reason && rcap)
    reason[0] = '\0';
  if (!c || csp_empty(c))
    return 1;

  /* upgrade-insecure-requests: any policy can request the rewrite. */
  int upgrade = 0;
  for (int i = 0; i < c->n; i++)
    if (c->p[i].upgrade)
      upgrade = 1;
  if (upgrade && starts_ci(out, "http://") && !is_local_host(out)) {
    /* Insert the 's' in place ("http://" -> "https://"): one char longer, so
     * only rewrite when it fits — never truncate a long URL into a broken one.
     */
    int len = 0;
    while (out[len])
      len++;
    if (len + 1 < out_cap) {
      for (int j = len; j >= 4; j--)
        out[j + 1] = out[j];
      out[4] = 's';
    }
  }

  struct purl pu;
  int page_https = parse_url(page_url, &pu) && pu.tls;
  int out_http = starts_ci(out, "http://");

  for (int i = 0; i < c->n; i++) {
    struct policy *pol = &c->p[i];
    if (pol->block_mixed && page_https && out_http) {
      if (reason && rcap) {
        int k = 0;
        const char *m = "blocked: mixed content";
        while (m[k] && k < rcap - 1) {
          reason[k] = m[k];
          k++;
        }
        reason[k] = '\0';
      }
      return 0;
    }
    const char *val = effective(pol, type);
    if (!val)
      continue;
    if (!sources_admit_url(val, out, page_url)) {
      if (reason && rcap) {
        int k = 0;
        const char *m = dir_name(type);
        while (m[k] && k < rcap - 1) {
          reason[k] = m[k];
          k++;
        }
        reason[k] = '\0';
      }
      return 0;
    }
  }
  return 1;
}

int csp_check_script_el(const csp *c, const char *abs_url, const char *page_url, const char *nonce, int parser_inserted,
                        char *out, int out_cap, char *reason, int rcap)
{
  /* csp_check_url fills `out` (upgrade rewrite applied) even when it blocks,
   * and gives the host-source-only verdict. If that already admits, we're done.
   */
  if (csp_check_url(c, CSP_SCRIPT, abs_url, page_url, out, out_cap, reason, rcap))
    return 1;
  if (!c || csp_empty(c))
    return 1;

  /* Host-source failed under some policy. Re-decide with the two things
   * csp_check_url can't express for a <script> element: a matching nonce, and
   * 'strict-dynamic' (which trusts scripts injected by an already-run script).
   * The element must still satisfy EVERY policy. */
  struct purl pu;
  int page_https = parse_url(page_url, &pu) && pu.tls;
  int out_http = starts_ci(out, "http://");

  for (int i = 0; i < c->n; i++) {
    struct policy *pol = &c->p[i];
    if (pol->block_mixed && page_https && out_http) {
      if (reason && rcap) {
        int k = 0;
        const char *m = "blocked: mixed content";
        while (m[k] && k < rcap - 1) {
          reason[k] = m[k];
          k++;
        }
        reason[k] = '\0';
      }
      return 0;
    }
    const char *val = effective(pol, CSP_SCRIPT);
    if (!val)
      continue; /* unrestricted by this policy */

    int admit = sources_admit_nonce(val, nonce);
    if (!admit && sources_strict_dynamic(val)) {
      /* strict-dynamic ignores host/scheme sources: a parser-inserted script
       * needs a nonce/hash (nonce already tried); a script-injected one is
       * trusted by its injector. */
      if (!parser_inserted)
        admit = 1;
    } else if (!admit) {
      admit = sources_admit_url(val, out, page_url);
    }
    if (!admit) {
      if (reason && rcap) {
        int k = 0;
        const char *m = "script-src";
        while (m[k] && k < rcap - 1) {
          reason[k] = m[k];
          k++;
        }
        reason[k] = '\0';
      }
      return 0;
    }
  }
  return 1;
}

int csp_check_inline(const csp *c, int type, const char *nonce, const unsigned char *body, int len, char *reason,
                     int rcap)
{
  if (reason && rcap)
    reason[0] = '\0';
  if (!c || csp_empty(c))
    return 1;
  int dt = (type == CSP_STYLE) ? CSP_STYLE : CSP_SCRIPT;
  for (int i = 0; i < c->n; i++) {
    const char *val = effective(&c->p[i], dt);
    if (!val)
      continue;
    if (!inline_allowed_by(val, nonce, body, len)) {
      if (reason && rcap) {
        int k = 0;
        const char *m = dir_name(dt);
        while (m[k] && k < rcap - 1) {
          reason[k] = m[k];
          k++;
        }
        reason[k] = '\0';
      }
      return 0;
    }
  }
  return 1;
}

int csp_allows_eval(const csp *c)
{
  if (!c || csp_empty(c))
    return 1;
  for (int i = 0; i < c->n; i++) {
    const char *val = effective(&c->p[i], CSP_SCRIPT);
    if (!val)
      continue; /* unrestricted by this policy */
    const char *p = val, *ts, *te;
    int seen = 0;
    while ((p = tok_next(p, &ts, &te)) != 0) {
      if (tok_is(ts, te, "'unsafe-eval'"))
        seen = 1;
      p = te;
    }
    if (!seen)
      return 0; /* this policy forbids it */
  }
  return 1;
}
