/* url.c — shared URL parsing and resolution (see url.h). */
#include "url.h"
#include "owos.h" /* lower/starts_ci case-insensitive helpers */

static void sapp(char *d, int *n, int cap, const char *s)
{
  while (*s && *n < cap - 1)
    d[(*n)++] = *s++;
  d[*n] = '\0';
}

static void uapp(char *d, int *n, int cap, unsigned v)
{
  char t[12];
  int k = 0;
  if (v == 0) {
    if (*n < cap - 1)
      d[(*n)++] = '0';
    d[*n] = '\0';
    return;
  }
  while (v) {
    t[k++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (k && *n < cap - 1)
    d[(*n)++] = t[--k];
  d[*n] = '\0';
}

static int has_colon(const char *s)
{
  while (*s)
    if (*s++ == ':')
      return 1;
  return 0;
}

const char *url_bracket_host(const char *host, char *buf, int cap)
{
  if (!has_colon(host))
    return host;
  int n = 0;
  sapp(buf, &n, cap, "[");
  sapp(buf, &n, cap, host);
  sapp(buf, &n, cap, "]");
  return buf;
}

int parse_url(const char *url, struct purl *u)
{
  const char *p = url;
  u->tls = 0;
  u->port = 0;
  if (starts_ci(p, "http://"))
    p += 7;
  else if (starts_ci(p, "https://")) {
    p += 8;
    u->tls = 1;
  }

  int i = 0;
  if (*p == '[') { /* IPv6 literal: host is kept without the brackets */
    p++;
    while (*p && *p != ']' && i < (int)sizeof(u->host) - 1)
      u->host[i++] = *p++;
    if (*p == ']')
      p++;
  } else {
    while (*p && *p != '/' && *p != ':' && i < (int)sizeof(u->host) - 1)
      u->host[i++] = *p++;
  }
  u->host[i] = '\0';
  if (i == 0)
    return 0;

  if (*p == ':') {
    p++;
    int port = 0;
    while (*p >= '0' && *p <= '9')
      port = port * 10 + (*p++ - '0');
    u->port = port;
  }
  if (u->port == 0)
    u->port = u->tls ? 443 : 80;

  int j = 0;
  if (*p != '/')
    u->path[j++] = '/';
  while (*p && j < (int)sizeof(u->path) - 1)
    u->path[j++] = *p++;
  u->path[j] = '\0';
  if (u->path[0] == '\0') {
    u->path[0] = '/';
    u->path[1] = '\0';
  }
  return 1;
}

/* Collapse "." and ".." segments in the absolute path `in` (RFC 3986 §5.2.4).
 * `in` must begin with '/'; a query/fragment (from the first '?' or '#') is
 * copied through untouched. Each kept segment carries its trailing '/', so ".."
 * pops back to — but never above — the leading slash. */
static void normalize_path(const char *in, char *out, int cap)
{
  int plen = 0;
  while (in[plen] && in[plen] != '?' && in[plen] != '#')
    plen++;

  int n = 0;
  if (cap > 1)
    out[n++] = '/';
  const char *p = in;
  if (*p == '/')
    p++;
  while (p < in + plen) {
    const char *segend = p;
    while (segend < in + plen && *segend != '/')
      segend++;
    int seglen = (int)(segend - p);
    if (seglen == 1 && p[0] == '.') {
      /* current dir — drop */
    } else if (seglen == 2 && p[0] == '.' && p[1] == '.') {
      if (n > 1) {
        n--;
        while (n > 0 && out[n - 1] != '/')
          n--;
      }
    } else {
      for (int i = 0; i < seglen && n < cap - 1; i++)
        out[n++] = p[i];
      if (segend < in + plen && n < cap - 1)
        out[n++] = '/';
    }
    p = (segend < in + plen) ? segend + 1 : segend;
  }
  out[n] = '\0';
  for (const char *t = in + plen; *t && n < cap - 1;)
    out[n++] = *t++;
  out[n] = '\0';
}

void resolve_url(const char *base, const char *loc, char *out, int cap)
{
  int n = 0;
  if (starts_ci(loc, "http://") || starts_ci(loc, "https://") || starts_ci(loc, "blob:") ||
      starts_ci(loc, "data:") || starts_ci(loc, "file:") || starts_ci(loc, "mailto:")) {
    sapp(out, &n, cap, loc);
    return;
  }
  struct purl b;
  if (!parse_url(base, &b)) {
    sapp(out, &n, cap, loc);
    return;
  }
  const char *scheme = b.tls ? "https://" : "http://";

  /* "host[:port]" of the base — the port must ride onto relative/absolute-path
   * references, or a :8000-style origin would resolve back to :80. */
  char hostport[URL_HOST_MAX + 8];
  int hp = 0;
  char hb[URL_HOST_MAX + 3];
  sapp(hostport, &hp, sizeof hostport, url_bracket_host(b.host, hb, sizeof hb));
  if (b.port != (b.tls ? 443 : 80)) {
    sapp(hostport, &hp, sizeof hostport, ":");
    uapp(hostport, &hp, sizeof hostport, (unsigned)b.port);
  }

  if (loc[0] == '/' && loc[1] == '/') { /* scheme-relative "//host" */
    sapp(out, &n, cap, scheme);
    sapp(out, &n, cap, loc + 2);
  } else { /* abs path or base-relative */
    char raw[URL_PATH_MAX];
    int r = 0;
    if (loc[0] == '/') { /* absolute path */
      sapp(raw, &r, sizeof raw, loc);
    } else { /* relative to base's dir */
      int slash = 0;
      for (int i = 0; b.path[i]; i++)
        if (b.path[i] == '/')
          slash = i;
      for (int i = 0; i <= slash && r < (int)sizeof raw - 1; i++)
        raw[r++] = b.path[i];
      raw[r] = '\0';
      sapp(raw, &r, sizeof raw, loc);
    }
    char norm[URL_PATH_MAX];
    normalize_path(raw, norm, sizeof norm);
    sapp(out, &n, cap, scheme);
    sapp(out, &n, cap, hostport);
    sapp(out, &n, cap, norm);
  }
}
