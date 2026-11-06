/* csp.c: Content-Security-Policy parsing and per-resource load/run decisions. */
#include "features/csp/csp.h"
#include "base64.h" /* shared base64 for the digest encoding */
#include "crypto.h" /* sha256 for 'sha256-...' inline hashes */
#include "dom.h"    /* lower / starts_ci / streq_ci */
#include "owos.h"
#include "url.h"
#include <stdbool.h>

/* One policy: a value string per directive (NULL = unset) plus flag directives. */
struct policy {
  char *dir[CSP_NTYPES]; /* raw value (space-separated sources), or NULL */
  char *form;            /* form-action value, or NULL                  */
  bool upgrade;          /* upgrade-insecure-requests                    */
  bool block_mixed;      /* block-all-mixed-content                      */
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

/* Map a directive name to a CSP_* slot, or -1. */
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
  pol->upgrade = false;
  pol->block_mixed = false;

  const char *p = policy;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ';')
      p++;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != ';')
      p++;
    const char *segend = p;

    const char *ns = seg;
    while (ns < segend && (*ns == ' ' || *ns == '\t'))
      ns++;
    const char *ne = ns;
    while (ne < segend && *ne != ' ' && *ne != '\t')
      ne++;
    int nlen = (int)(ne - ns);
    if (nlen <= 0)
      continue;

    const char *vs = ne;
    while (vs < segend && (*vs == ' ' || *vs == '\t'))
      vs++;
    const char *ve = segend;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t'))
      ve--;

    int nl = nlen;
    if (nl == 25 && starts_ci(ns, "upgrade-insecure-requests")) {
      pol->upgrade = true;
      continue;
    }
    if (nl == 23 && starts_ci(ns, "block-all-mixed-content")) {
      pol->block_mixed = true;
      continue;
    }
    if (nl == 12 && starts_ci(ns, "form-action")) {
      free(pol->form);
      pol->form = dupn(vs, (int)(ve - vs));
      continue;
    }

    int di = dir_index(ns, nlen);
    if (di < 0)
      continue;
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

/* Source value governing `type` under one policy after fallbacks; NULL = unrestricted. */
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
  return pol->dir[CSP_DEFAULT];
}

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
static bool tok_is(const char *ts, const char *te, const char *lit)
{
  int n = (int)(te - ts);
  for (int i = 0; i < n; i++) {
    if (!lit[i] || lower(ts[i]) != lower(lit[i]))
      return false;
  }
  return lit[n] == 0;
}
static bool tok_starts(const char *ts, const char *te, const char *pre)
{
  int n = (int)(te - ts), i = 0;
  for (; pre[i]; i++) {
    if (i >= n || lower(ts[i]) != lower(pre[i]))
      return false;
  }
  return true;
}

static bool origin_eq(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return false;
  return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

/* Match a host-source token (scheme/port/path optional) against an absolute URL. */
static bool host_source_match(const char *s, const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return false;

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
      return false;
    h = sep + 3;
  }
  if (want_tls >= 0 && u.tls != want_tls)
    return false;

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

  const char *path = (*h == '/') ? h : 0;

  if (host[0] == '*' && host[1] == '\0') {
    /* any host */
  } else if (host[0] == '*' && host[1] == '.') {
    const char *suf = host + 1;
    int ul = (int)strlen(u.host), sl = (int)strlen(suf);
    if (ul <= sl || !streq_ci(u.host + (ul - sl), suf))
      return false;
  } else if (!streq_ci(u.host, host)) {
    return false;
  }

  if (port[0]) {
    if (!(port[0] == '*' && port[1] == '\0')) {
      int pn = 0;
      for (const char *q = port; *q >= '0' && *q <= '9'; q++)
        pn = pn * 10 + (*q - '0');
      if (pn != u.port)
        return false;
    }
  } else {
    if (u.port != (u.tls ? 443 : 80))
      return false;
  }

  /* trailing '/' = prefix, else exact or segment match */
  if (path && !(path[0] == '/' && path[1] == '\0')) {
    char pp[512];
    int k = 0;
    for (const char *q = path; *q && *q != '?' && *q != '#' && k < (int)sizeof pp - 1; q++)
      pp[k++] = *q;
    pp[k] = '\0';
    int pl = k, ulen = (int)strlen(u.path);
    if (pl > 0 && pp[pl - 1] == '/') {
      if (ulen < pl)
        return false;
      for (int i = 0; i < pl; i++)
        if (u.path[i] != pp[i])
          return false;
    } else {
      if (!streq(u.path, pp)) {
        if (ulen <= pl)
          return false;
        for (int i = 0; i < pl; i++)
          if (u.path[i] != pp[i])
            return false;
        if (u.path[pl] != '/')
          return false;
      }
    }
  }
  return true;
}

static bool scheme_source_match(const char *s, const char *url)
{
  int n = (int)strlen(s);
  if (n < 2 || s[n - 1] != ':')
    return false;
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
  char pre[26];
  int j = 0;
  for (int i = 0; sc[i] && j < (int)sizeof pre - 2; i++)
    pre[j++] = sc[i];
  pre[j++] = ':';
  pre[j] = '\0';
  return starts_ci(url, pre);
}

/* Does the source token admit `url` ('self' is relative to `page_url`)? */
static bool url_source_admits(const char *ts, const char *te, const char *url, const char *page_url)
{
  if (tok_is(ts, te, "'self'"))
    return origin_eq(url, page_url);
  if (ts[0] == '\'')
    return false;
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

  bool has_sep = false;
  for (const char *q = s; q[0] && q[1] && q[2]; q++)
    if (q[0] == ':' && q[1] == '/' && q[2] == '/') {
      has_sep = true;
      break;
    }
  if (!has_sep && s[n - 1] == ':')
    return scheme_source_match(s, url);
  return host_source_match(s, url);
}

static bool sources_admit_url(const char *val, const char *url, const char *page_url)
{
  const char *p = val, *ts, *te;
  bool found = false;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'none'"))
      return false;
    if (url_source_admits(ts, te, url, page_url))
      found = true;
    p = te;
  }
  return found;
}

/* Does `val` carry a 'nonce-<nonce>' matching `nonce`? */
static bool sources_admit_nonce(const char *val, const char *nonce)
{
  if (!nonce || !*nonce)
    return false;
  int nl = (int)strlen(nonce);
  const char *p = val, *ts, *te;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_starts(ts, te, "'nonce-")) {
      const char *vs = ts + 7;
      const char *ve = te;
      if (ve > vs && ve[-1] == '\'')
        ve--;
      if (ve - vs == nl) {
        bool eq = true;
        for (int i = 0; i < nl; i++)
          if (vs[i] != nonce[i]) {
            eq = false;
            break;
          }
        if (eq)
          return true;
      }
    }
    p = te;
  }
  return false;
}

/* 'strict-dynamic' ignores host/scheme sources: only nonces, hashes and injected scripts pass. */
static bool sources_strict_dynamic(const char *val)
{
  const char *p = val, *ts, *te;
  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'strict-dynamic'"))
      return true;
    p = te;
  }
  return false;
}

/* Padless base64 of a digest, the form b64_canon_token normalizes tokens to. */
static void b64_canon_digest(const unsigned char *d, int n, char *dst)
{
  int l = base64_encode(d, n, dst);
  while (l > 0 && dst[l - 1] == '=')
    l--;
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

static bool inline_allowed_by(const char *val, const char *nonce, const unsigned char *body, int len)
{
  const char *p = val, *ts, *te;
  int unsafe_inline = 0, has_nh = 0, ok = 0;
  char myhash[64];
  bool have_hash = false;

  while ((p = tok_next(p, &ts, &te)) != 0) {
    if (tok_is(ts, te, "'none'"))
      return false;
    else if (tok_is(ts, te, "'unsafe-inline'"))
      unsafe_inline = 1;
    else if (tok_starts(ts, te, "'nonce-")) {
      has_nh = 1;
      if (nonce && *nonce) {
        const char *vs = ts + 7;
        const char *ve = te;
        if (ve > vs && ve[-1] == '\'')
          ve--;
        int nl = (int)strlen(nonce);
        if (ve - vs == nl) {
          bool eq = true;
          for (int i = 0; i < nl; i++)
            if (vs[i] != nonce[i]) {
              eq = false;
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
          have_hash = true;
        }
        const char *vs = ts + 8;
        const char *ve = te;
        if (ve > vs && ve[-1] == '\'')
          ve--;
        char tk[64];
        b64_canon_token(vs, ve, tk, sizeof tk);
        if (streq(tk, myhash))
          ok = 1;
      }
    } else if (tok_starts(ts, te, "'sha384-") || tok_starts(ts, te, "'sha512-")) {
      has_nh = 1;
    }
    p = te;
  }
  if (ok)
    return true;
  if (unsafe_inline && !has_nh)
    return true;
  return false;
}

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

static bool is_local_host(const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return false;
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

  bool upgrade = false;
  for (int i = 0; i < c->n; i++)
    if (c->p[i].upgrade)
      upgrade = true;
  if (upgrade && starts_ci(out, "http://") && !is_local_host(out)) {
    /* http:// -> https:// is one char longer; only rewrite when it fits */
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
  /* csp_check_url fills `out` even when it blocks */
  if (csp_check_url(c, CSP_SCRIPT, abs_url, page_url, out, out_cap, reason, rcap))
    return 1;
  if (!c || csp_empty(c))
    return 1;

  /* re-decide with the element's nonce and 'strict-dynamic'; every policy must still pass */
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
      continue;

    int admit = sources_admit_nonce(val, nonce);
    if (!admit && sources_strict_dynamic(val)) {
      /* parser-inserted scripts need a nonce/hash (already tried); injected ones are trusted */
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
      continue;
    const char *p = val, *ts, *te;
    bool seen = false;
    while ((p = tok_next(p, &ts, &te)) != 0) {
      if (tok_is(ts, te, "'unsafe-eval'"))
        seen = true;
      p = te;
    }
    if (!seen)
      return 0;
  }
  return 1;
}
