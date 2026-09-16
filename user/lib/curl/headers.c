/* headers.c — the HTTP response header object declared in headers.h.
 *
 * The raw block is parsed exactly once, here, into {name, value} pairs; every
 * client then queries by name. That keeps all the fiddly, error-prone string
 * scanning (line splitting, CR/LF handling, whitespace trimming, repeated
 * headers) in one tested place instead of copy-pasted into each caller. */
#include "headers.h"
#include "owos.h"

/* Heap copy of [s, s+n) as a NUL-terminated string. */
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

/* Append a {name(lowercased), value} field, both copied from spans. 0/-1. */
static int add_field(struct http_headers *h, const char *name, int nlen, const char *val, int vlen)
{
  if (h->n == h->cap) {
    int nc = h->cap ? h->cap * 2 : 16;
    struct http_field *nf = (struct http_field *)realloc(h->fields, (unsigned)(nc * (int)sizeof *nf));
    if (!nf)
      return -1;
    h->fields = nf;
    h->cap = nc;
  }
  char *nm = dupn(name, nlen);
  char *vl = dupn(val, vlen);
  if (!nm || !vl) {
    free(nm);
    free(vl);
    return -1;
  }
  for (char *p = nm; *p; p++)
    *p = lower(*p);
  h->fields[h->n].name = nm;
  h->fields[h->n].value = vl;
  h->n++;
  return 0;
}

int http_headers_parse(struct http_headers *h, const char *block)
{
  h->status = 0;
  h->fields = 0;
  h->n = 0;
  h->cap = 0;

  /* Status line: "HTTP/x.y NNN ...". */
  {
    const char *p = block;
    while (*p && *p != ' ')
      p++;
    while (*p == ' ')
      p++;
    int code = 0, k = 0;
    while (p[k] >= '0' && p[k] <= '9' && k < 3) {
      code = code * 10 + (p[k] - '0');
      k++;
    }
    if (k)
      h->status = code;
  }

  /* Advance to the line after the status line. */
  const char *line = block;
  while (*line && *line != '\n')
    line++;
  if (*line)
    line++;

  int rc = 0;
  while (*line && *line != '\r' && *line != '\n') {
    const char *eol = line;
    while (*eol && *eol != '\n')
      eol++; /* '\n' or NUL              */
    const char *lend = eol;
    if (lend > line && lend[-1] == '\r')
      lend--;

    /* name : value */
    const char *colon = line;
    while (colon < lend && *colon != ':')
      colon++;
    if (colon < lend) {
      const char *v = colon + 1;
      while (v < lend && (*v == ' ' || *v == '\t'))
        v++; /* ltrim     */
      const char *vend = lend;
      while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t'))
        vend--; /* rtrim */
      if (add_field(h, line, (int)(colon - line), v, (int)(vend - v)) != 0)
        rc = -1;
    }
    line = *eol ? eol + 1 : eol;
  }
  return rc;
}

const char *http_headers_at(const struct http_headers *h, const char *name, int idx)
{
  for (int i = 0; i < h->n; i++)
    if (streq_ci(h->fields[i].name, name) && idx-- == 0)
      return h->fields[i].value;
  return 0;
}

const char *http_headers_get(const struct http_headers *h, const char *name) { return http_headers_at(h, name, 0); }

int http_headers_count(const struct http_headers *h, const char *name)
{
  int c = 0;
  for (int i = 0; i < h->n; i++)
    if (streq_ci(h->fields[i].name, name))
      c++;
  return c;
}

void http_headers_free(struct http_headers *h)
{
  for (int i = 0; i < h->n; i++) {
    free(h->fields[i].name);
    free(h->fields[i].value);
  }
  free(h->fields);
  h->fields = 0;
  h->n = h->cap = 0;
}

bool http_header_has_token(const char *value, const char *token)
{
  if (!value)
    return false;
  for (const char *s = value; *s;) {
    while (*s == ' ' || *s == '\t' || *s == ',')
      s++; /* skip separators */
    if (!*s)
      break;
    const char *a = s, *b = token;
    while (*a && lower(*a) == lower(*b)) {
      a++;
      b++;
    }
    if (*b == '\0' && (*a == '\0' || *a == ',' || *a == ' ' || *a == '\t' || *a == ';'))
      return true;
    while (*s && *s != ',')
      s++; /* advance to the next token */
  }
  return false;
}

unsigned http_parse_uint(const char *value, bool *ok)
{
  bool any = false;
  unsigned v = 0;
  if (value) {
    while (*value == ' ' || *value == '\t')
      value++;
    while (*value >= '0' && *value <= '9') {
      v = v * 10 + (unsigned)(*value++ - '0');
      any = true;
    }
  }
  if (ok)
    *ok = any;
  return v;
}
