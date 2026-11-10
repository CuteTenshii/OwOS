/* plist.c: Apple property lists, XML and binary. See plist.h. */
#include "plist.h"
#include "base64.h"
#include "fmt.h"
#include "owos.h"

#define MAX_DEPTH 200

/* ---- tree ---- */

static plist *node_new(plist_type t)
{
  plist *v = (plist *)malloc(sizeof *v);
  if (v) {
    memset(v, 0, sizeof *v);
    v->type = t;
  }
  return v;
}

void plist_free(plist *v)
{
  if (!v)
    return;
  switch (v->type) {
  case PLIST_STRING:
  case PLIST_DATA:
    free(v->u.str.data);
    break;
  case PLIST_ARRAY:
    for (int i = 0; i < v->u.array.count; i++)
      plist_free(v->u.array.items[i]);
    free(v->u.array.items);
    break;
  case PLIST_DICT:
    for (int i = 0; i < v->u.dict.count; i++) {
      free(v->u.dict.keys[i]);
      plist_free(v->u.dict.vals[i]);
    }
    free(v->u.dict.keys);
    free(v->u.dict.vals);
    break;
  default:
    break;
  }
  free(v);
}

plist *plist_new_bool(int b)
{
  plist *v = node_new(PLIST_BOOL);
  if (v)
    v->u.boolean = b != 0;
  return v;
}
plist *plist_new_int(long long n)
{
  plist *v = node_new(PLIST_INT);
  if (v)
    v->u.integer = n;
  return v;
}
plist *plist_new_uid(long long n)
{
  plist *v = node_new(PLIST_UID);
  if (v)
    v->u.integer = n;
  return v;
}
plist *plist_new_real(double d)
{
  plist *v = node_new(PLIST_REAL);
  if (v)
    v->u.real = d;
  return v;
}
plist *plist_new_date(double s)
{
  plist *v = node_new(PLIST_DATE);
  if (v)
    v->u.real = s;
  return v;
}

static plist *new_bytes(plist_type t, const void *bytes, int len)
{
  plist *v;
  if (len < 0)
    return 0;
  v = node_new(t);
  if (!v)
    return 0;
  v->u.str.data = (char *)malloc((unsigned)len + 1);
  if (!v->u.str.data) {
    free(v);
    return 0;
  }
  if (len)
    memcpy(v->u.str.data, bytes, (unsigned)len);
  v->u.str.data[len] = '\0';
  v->u.str.len = len;
  return v;
}

plist *plist_new_string(const char *s) { return new_bytes(PLIST_STRING, s ? s : "", s ? (int)strlen(s) : 0); }
plist *plist_new_data(const void *bytes, int len) { return new_bytes(PLIST_DATA, bytes, len); }
plist *plist_new_array(void) { return node_new(PLIST_ARRAY); }
plist *plist_new_dict(void) { return node_new(PLIST_DICT); }

int plist_array_append(plist *arr, plist *item)
{
  if (!item)
    return -1;
  if (!arr || arr->type != PLIST_ARRAY)
    goto fail;
  if (arr->u.array.count == arr->u.array.cap) {
    int nc = arr->u.array.cap ? arr->u.array.cap * 2 : 8;
    plist **ni = (plist **)realloc(arr->u.array.items, (unsigned)nc * sizeof *ni);
    if (!ni)
      goto fail;
    arr->u.array.items = ni;
    arr->u.array.cap = nc;
  }
  arr->u.array.items[arr->u.array.count++] = item;
  return 0;
fail:
  plist_free(item);
  return -1;
}

int plist_dict_set(plist *d, const char *key, plist *val)
{
  char *k;
  if (!val)
    return -1;
  if (!d || d->type != PLIST_DICT || !key) {
    plist_free(val);
    return -1;
  }
  for (int i = 0; i < d->u.dict.count; i++)
    if (!strcmp(d->u.dict.keys[i], key)) {
      plist_free(d->u.dict.vals[i]);
      d->u.dict.vals[i] = val;
      return 0;
    }
  k = strdup(key);
  if (k && d->u.dict.count == d->u.dict.cap) { /* keys and vals grow together */
    int nc = d->u.dict.cap ? d->u.dict.cap * 2 : 8;
    char **nk = (char **)realloc(d->u.dict.keys, (unsigned)nc * sizeof *nk);
    plist **nv;
    if (nk)
      d->u.dict.keys = nk;
    nv = nk ? (plist **)realloc(d->u.dict.vals, (unsigned)nc * sizeof *nv) : 0;
    if (nv) {
      d->u.dict.vals = nv;
      d->u.dict.cap = nc;
    } else {
      free(k);
      k = 0;
    }
  }
  if (!k) {
    plist_free(val);
    return -1;
  }
  d->u.dict.keys[d->u.dict.count] = k;
  d->u.dict.vals[d->u.dict.count++] = val;
  return 0;
}

int plist_count(const plist *v)
{
  if (v && v->type == PLIST_ARRAY)
    return v->u.array.count;
  if (v && v->type == PLIST_DICT)
    return v->u.dict.count;
  return 0;
}

plist *plist_array_get(const plist *a, int i)
{
  return a && a->type == PLIST_ARRAY && i >= 0 && i < a->u.array.count ? a->u.array.items[i] : 0;
}

plist *plist_dict_get(const plist *d, const char *key)
{
  if (!d || d->type != PLIST_DICT || !key)
    return 0;
  for (int i = 0; i < d->u.dict.count; i++)
    if (!strcmp(d->u.dict.keys[i], key))
      return d->u.dict.vals[i];
  return 0;
}

const char *plist_get_string(const plist *v) { return v && v->type == PLIST_STRING ? v->u.str.data : 0; }

int plist_get_number(const plist *v, double *out)
{
  if (!v)
    return 0;
  switch (v->type) {
  case PLIST_INT:
  case PLIST_UID:
    *out = (double)v->u.integer;
    return 1;
  case PLIST_REAL:
  case PLIST_DATE:
    *out = v->u.real;
    return 1;
  case PLIST_BOOL:
    *out = v->u.boolean;
    return 1;
  default:
    return 0;
  }
}

int plist_get_bool(const plist *v, int dflt) { return v && v->type == PLIST_BOOL ? v->u.boolean : dflt; }

/* ---- growable buffer ---- */

typedef struct {
  unsigned char *b;
  int n, cap, oom;
} sbuf;

static void sb_put(sbuf *s, const void *p, int len)
{
  if (s->oom || len < 0)
    return;
  if (s->n + len + 1 > s->cap) {
    int nc = s->cap ? s->cap : 256;
    unsigned char *nb;
    while (nc < s->n + len + 1)
      nc *= 2;
    nb = (unsigned char *)realloc(s->b, (unsigned)nc);
    if (!nb) {
      s->oom = 1;
      return;
    }
    s->b = nb;
    s->cap = nc;
  }
  if (len)
    memcpy(s->b + s->n, p, (unsigned)len);
  s->n += len;
  s->b[s->n] = '\0';
}
static void sb_str(sbuf *s, const char *str) { sb_put(s, str, (int)strlen(str)); }
static void sb_byte(sbuf *s, int c)
{
  unsigned char b = (unsigned char)c;
  sb_put(s, &b, 1);
}

/* ---- dates ---- */

static long long days_from_civil(long long y, unsigned m, unsigned d)
{
  long long era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d)
{
  long long era;
  unsigned doe, yoe, doy, mp;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  *y = (long long)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y += *m <= 2;
}

#define EPOCH_2001 978307200LL /* unix time of 2001-01-01T00:00:00Z */

static int dig(const char *s, int n, int *out)
{
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return 0;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return 1;
}

/* "YYYY-MM-DDTHH:MM:SSZ" to seconds since 2001 */
static int parse_date(const char *s, int len, double *out)
{
  int Y, M, D, h, mi, se;
  if (len < 20 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
    return 0;
  if (!dig(s, 4, &Y) || !dig(s + 5, 2, &M) || !dig(s + 8, 2, &D) || !dig(s + 11, 2, &h) || !dig(s + 14, 2, &mi) ||
      !dig(s + 17, 2, &se) || M < 1 || M > 12 || D < 1 || D > 31)
    return 0;
  *out = (double)(days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400LL + h * 3600 + mi * 60 + se - EPOCH_2001);
  return 1;
}

static void format_date(double secs, char *out, int cap)
{
  long long t = (long long)secs + EPOCH_2001, days = t / 86400, rem = t % 86400, y;
  unsigned m, d;
  if (rem < 0) {
    rem += 86400;
    days--;
  }
  civil_from_days(days, &y, &m, &d);
  snprintf(out, (unsigned)cap, "%04d-%02u-%02uT%02d:%02d:%02dZ", (int)y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60),
           (int)(rem % 60));
}

/* ---- XML parser ---- */

typedef struct {
  const char *p, *end, *base;
  plist_error *err;
  int failed;
} xp;

static void xfail(xp *x, const char *msg, const char *at)
{
  if (!x->failed && x->err) {
    x->err->pos = (int)(at - x->base);
    snprintf(x->err->msg, sizeof x->err->msg, "%s", msg);
  }
  x->failed = 1;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void x_skip_ws(xp *x)
{
  while (x->p < x->end && is_ws(*x->p))
    x->p++;
}

static int x_starts(xp *x, const char *lit)
{
  int n = (int)strlen(lit);
  return x->end - x->p >= n && !memcmp(x->p, lit, (unsigned)n);
}

/* Skip the prolog and comments between elements: <?...?>, <!-- -->, <!DOCTYPE ...> */
static void x_skip_misc(xp *x)
{
  for (;;) {
    x_skip_ws(x);
    if (x_starts(x, "<?")) {
      while (x->p < x->end && !x_starts(x, "?>"))
        x->p++;
      x->p = x->p + 2 > x->end ? x->end : x->p + 2;
    } else if (x_starts(x, "<!--")) {
      while (x->p < x->end && !x_starts(x, "-->"))
        x->p++;
      x->p = x->p + 3 > x->end ? x->end : x->p + 3;
    } else if (x_starts(x, "<!DOCTYPE")) {
      int depth = 0;
      while (x->p < x->end) {
        if (*x->p == '[')
          depth++;
        else if (*x->p == ']')
          depth--;
        else if (*x->p == '>' && depth <= 0) {
          x->p++;
          break;
        }
        x->p++;
      }
    } else
      return;
  }
}

/* Read an opening tag at '<': name, and whether it is self-closing; leaves p after '>'. 0 on a closing tag/error. */
static int x_open(xp *x, char *name, int cap, int *selfclose)
{
  int n = 0;
  *selfclose = 0;
  if (x->p >= x->end || *x->p != '<' || x->p + 1 >= x->end || x->p[1] == '/')
    return 0;
  x->p++;
  while (x->p < x->end && !is_ws(*x->p) && *x->p != '>' && *x->p != '/') {
    if (n < cap - 1)
      name[n++] = *x->p;
    x->p++;
  }
  name[n] = '\0';
  while (x->p < x->end && *x->p != '>') { /* attributes are ignored */
    if (*x->p == '/' && x->p + 1 < x->end && x->p[1] == '>')
      *selfclose = 1;
    x->p++;
  }
  if (x->p >= x->end)
    return 0;
  x->p++;
  return 1;
}

static int x_close(xp *x, const char *name)
{
  int n = (int)strlen(name);
  x_skip_ws(x);
  if (!x_starts(x, "</") || x->end - x->p < n + 3 || memcmp(x->p + 2, name, (unsigned)n))
    return 0;
  x->p += 2 + n;
  x_skip_ws(x);
  if (x->p >= x->end || *x->p != '>')
    return 0;
  x->p++;
  return 1;
}

static void utf8_put(sbuf *s, unsigned cp)
{
  if (cp < 0x80)
    sb_byte(s, (int)cp);
  else if (cp < 0x800) {
    sb_byte(s, 0xC0 | (int)(cp >> 6));
    sb_byte(s, 0x80 | (int)(cp & 0x3F));
  } else if (cp < 0x10000) {
    sb_byte(s, 0xE0 | (int)(cp >> 12));
    sb_byte(s, 0x80 | (int)((cp >> 6) & 0x3F));
    sb_byte(s, 0x80 | (int)(cp & 0x3F));
  } else {
    sb_byte(s, 0xF0 | (int)(cp >> 18));
    sb_byte(s, 0x80 | (int)((cp >> 12) & 0x3F));
    sb_byte(s, 0x80 | (int)((cp >> 6) & 0x3F));
    sb_byte(s, 0x80 | (int)(cp & 0x3F));
  }
}

/* Text up to the next '<' with entities and CDATA decoded into a malloc'd, NUL-terminated buffer */
static char *x_text(xp *x, int *len)
{
  sbuf s = {0, 0, 0, 0};
  while (x->p < x->end) {
    if (x_starts(x, "<![CDATA[")) {
      x->p += 9;
      while (x->p < x->end && !x_starts(x, "]]>"))
        sb_byte(&s, *x->p++);
      x->p = x->p + 3 > x->end ? x->end : x->p + 3;
    } else if (*x->p == '<') {
      break;
    } else if (*x->p == '&') {
      const char *e = x->p + 1;
      unsigned cp = 0;
      while (e < x->end && *e != ';' && e - x->p < 12)
        e++;
      if (e >= x->end || *e != ';') {
        xfail(x, "unterminated entity", x->p);
        break;
      }
      if (e - x->p - 1 == 2 && !memcmp(x->p + 1, "lt", 2))
        cp = '<';
      else if (e - x->p - 1 == 2 && !memcmp(x->p + 1, "gt", 2))
        cp = '>';
      else if (e - x->p - 1 == 3 && !memcmp(x->p + 1, "amp", 3))
        cp = '&';
      else if (e - x->p - 1 == 4 && !memcmp(x->p + 1, "quot", 4))
        cp = '"';
      else if (e - x->p - 1 == 4 && !memcmp(x->p + 1, "apos", 4))
        cp = '\'';
      else if (x->p[1] == '#') {
        const char *d = x->p + 2;
        int hex = *d == 'x' || *d == 'X';
        if (hex)
          d++;
        for (; d < e; d++) {
          int c = *d >= '0' && *d <= '9' ? *d - '0' : hex && *d >= 'a' && *d <= 'f' ? *d - 'a' + 10 : hex && *d >= 'A' && *d <= 'F' ? *d - 'A' + 10 : -1;
          if (c < 0) {
            cp = 0xFFFD;
            break;
          }
          cp = cp * (hex ? 16 : 10) + (unsigned)c;
          if (cp > 0x10FFFF) {
            cp = 0xFFFD;
            break;
          }
        }
      } else {
        xfail(x, "unknown entity", x->p);
        break;
      }
      utf8_put(&s, cp);
      x->p = e + 1;
    } else {
      sb_byte(&s, *x->p++);
    }
  }
  if (s.oom || x->failed) {
    free(s.b);
    return 0;
  }
  if (!s.b) {
    s.b = (unsigned char *)malloc(1);
    if (!s.b)
      return 0;
    s.b[0] = '\0';
  }
  *len = s.n;
  return (char *)s.b;
}

static plist *x_value(xp *x, const char *name, int selfclose, int depth);

static int x_scalar_text(xp *x, const char *name, int selfclose, char **text, int *tlen)
{
  *text = 0;
  *tlen = 0;
  if (selfclose) {
    *text = strdup("");
    return *text != 0;
  }
  *text = x_text(x, tlen);
  if (!*text)
    return 0;
  if (!x_close(x, name)) {
    xfail(x, "missing closing tag", x->p);
    free(*text);
    *text = 0;
    return 0;
  }
  return 1;
}

static plist *x_value(xp *x, const char *name, int selfclose, int depth)
{
  plist *v = 0;
  char *text;
  int tlen;
  if (depth > MAX_DEPTH) {
    xfail(x, "nesting too deep", x->p);
    return 0;
  }
  if (!strcmp(name, "true") || !strcmp(name, "false")) {
    if (!selfclose && !x_close(x, name)) {
      xfail(x, "bad boolean", x->p);
      return 0;
    }
    return plist_new_bool(name[0] == 't');
  }
  if (!strcmp(name, "array") || !strcmp(name, "dict")) {
    int isdict = name[0] == 'd';
    char sub[24];
    int sc;
    v = isdict ? plist_new_dict() : plist_new_array();
    if (!v)
      return 0;
    if (selfclose)
      return v;
    for (;;) {
      char *key = 0;
      plist *child;
      x_skip_misc(x);
      if (x_starts(x, "</")) {
        if (!x_close(x, name)) {
          xfail(x, "mismatched closing tag", x->p);
          goto fail;
        }
        return v;
      }
      if (!x_open(x, sub, sizeof sub, &sc)) {
        xfail(x, "bad element", x->p);
        goto fail;
      }
      if (isdict) {
        int klen;
        if (strcmp(sub, "key")) {
          xfail(x, "expected <key>", x->p);
          goto fail;
        }
        if (!x_scalar_text(x, "key", sc, &key, &klen))
          goto fail;
        x_skip_misc(x);
        if (!x_open(x, sub, sizeof sub, &sc)) {
          free(key);
          xfail(x, "key without value", x->p);
          goto fail;
        }
      }
      child = x_value(x, sub, sc, depth + 1);
      if (!child) {
        free(key);
        goto fail;
      }
      if (isdict ? plist_dict_set(v, key, child) : plist_array_append(v, child)) {
        free(key);
        xfail(x, "out of memory", x->p);
        goto fail;
      }
      free(key);
    }
  }
  if (!x_scalar_text(x, name, selfclose, &text, &tlen))
    return 0;
  if (!strcmp(name, "string")) {
    v = new_bytes(PLIST_STRING, text, tlen);
  } else if (!strcmp(name, "integer")) {
    const char *p = text;
    int neg = 0;
    unsigned long long n = 0;
    while (is_ws(*p))
      p++;
    if (*p == '-' || *p == '+')
      neg = *p++ == '-';
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
      for (p += 2; *p; p++)
        n = n * 16 + (unsigned long long)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
    } else {
      for (; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (unsigned long long)(*p - '0');
    }
    v = plist_new_int(neg ? -(long long)n : (long long)n);
  } else if (!strcmp(name, "real")) {
    const char *p = text;
    while (is_ws(*p))
      p++;
    if (starts_ci(p, "nan"))
      v = plist_new_real(__builtin_nan(""));
    else if (starts_ci(p, "inf"))
      v = plist_new_real(__builtin_inf());
    else if (starts_ci(p, "-inf"))
      v = plist_new_real(-__builtin_inf());
    else
      v = plist_new_real(strtod(p, 0));
  } else if (!strcmp(name, "date")) {
    double d;
    if (!parse_date(text, tlen, &d)) {
      xfail(x, "bad date", x->p);
    } else
      v = plist_new_date(d);
  } else if (!strcmp(name, "data")) {
    unsigned char *buf = (unsigned char *)malloc((unsigned)base64_decoded_cap(tlen) + 1);
    if (buf) {
      int n = base64_decode(text, tlen, buf);
      if (n < 0)
        xfail(x, "bad base64", x->p);
      else
        v = plist_new_data(buf, n);
      free(buf);
    }
  } else {
    xfail(x, "unknown element", x->p);
  }
  free(text);
  return v;
fail:
  plist_free(v);
  return 0;
}

static plist *parse_xml(const char *buf, int len, plist_error *err)
{
  xp x = {buf, buf + len, buf, err, 0};
  char name[24];
  int sc;
  plist *v = 0;
  x_skip_misc(&x);
  if (!x_open(&x, name, sizeof name, &sc) || strcmp(name, "plist")) {
    xfail(&x, "not a plist", x.p);
    return 0;
  }
  if (sc) {
    xfail(&x, "empty plist", x.p);
    return 0;
  }
  x_skip_misc(&x);
  if (!x_open(&x, name, sizeof name, &sc)) {
    xfail(&x, "empty plist", x.p);
    return 0;
  }
  v = x_value(&x, name, sc, 0);
  if (v && !x_close(&x, "plist") && (x_skip_misc(&x), !x_close(&x, "plist"))) {
    xfail(&x, "missing </plist>", x.p);
    plist_free(v);
    return 0;
  }
  return v;
}

/* ---- binary parser ---- */

typedef struct {
  const unsigned char *b;
  int len, nobj, refsz, offsz;
  long long table;
  plist_error *err;
  int failed;
} bp;

static void bfail(bp *p, const char *msg, int pos)
{
  if (!p->failed && p->err) {
    p->err->pos = pos;
    snprintf(p->err->msg, sizeof p->err->msg, "%s", msg);
  }
  p->failed = 1;
}

static unsigned long long be(const unsigned char *p, int n)
{
  unsigned long long v = 0;
  for (int i = 0; i < n; i++)
    v = v << 8 | p[i];
  return v;
}

/* Count at the marker's low nibble, or the int object that follows when it is 0xF; advances *off */
static int b_count(bp *p, int *off, int nib, long long *count)
{
  if (nib != 0xF) {
    *count = nib;
    return 1;
  }
  if (*off >= p->len || (p->b[*off] & 0xF0) != 0x10)
    return 0;
  {
    int sz = 1 << (p->b[*off] & 0xF);
    if (sz > 8 || *off + 1 + sz > p->len)
      return 0;
    *count = (long long)be(p->b + *off + 1, sz);
    *off += 1 + sz;
  }
  return *count >= 0;
}

static plist *b_obj(bp *p, long long idx, int depth);

static plist *b_obj(bp *p, long long idx, int depth)
{
  int off, marker, type, nib;
  long long cnt, offv;
  plist *v = 0;
  if (depth > MAX_DEPTH) {
    bfail(p, "nesting too deep", 0);
    return 0;
  }
  if (idx < 0 || idx >= p->nobj) {
    bfail(p, "object reference out of range", 0);
    return 0;
  }
  offv = (long long)be(p->b + p->table + idx * p->offsz, p->offsz);
  if (offv < 8 || offv >= p->len) {
    bfail(p, "object offset out of range", (int)offv);
    return 0;
  }
  off = (int)offv;
  marker = p->b[off++];
  type = marker >> 4;
  nib = marker & 0xF;
  switch (type) {
  case 0x0:
    if (marker == 0x08 || marker == 0x09)
      return plist_new_bool(marker == 0x09);
    bfail(p, "unsupported null/fill object", off - 1);
    return 0;
  case 0x1: {
    int sz = 1 << nib;
    long long n;
    if (sz > 16 || off + sz > p->len) {
      bfail(p, "bad integer", off - 1);
      return 0;
    }
    n = sz > 8 ? (long long)be(p->b + off + sz - 8, 8) : (long long)be(p->b + off, sz);
    return plist_new_int(n);
  }
  case 0x2: {
    int sz = 1 << nib;
    if ((sz != 4 && sz != 8) || off + sz > p->len) {
      bfail(p, "bad real", off - 1);
      return 0;
    }
    if (sz == 4) {
      unsigned u = (unsigned)be(p->b + off, 4);
      float f;
      memcpy(&f, &u, 4);
      return plist_new_real((double)f);
    } else {
      unsigned long long u = be(p->b + off, 8);
      double d;
      memcpy(&d, &u, 8);
      return plist_new_real(d);
    }
  }
  case 0x3: {
    unsigned long long u;
    double d;
    if (marker != 0x33 || off + 8 > p->len) {
      bfail(p, "bad date", off - 1);
      return 0;
    }
    u = be(p->b + off, 8);
    memcpy(&d, &u, 8);
    return plist_new_date(d);
  }
  case 0x4:
  case 0x5:
  case 0x6: {
    if (!b_count(p, &off, nib, &cnt)) {
      bfail(p, "bad length", off);
      return 0;
    }
    if (type == 0x6)
      cnt *= 2;
    if (cnt > p->len - off) {
      bfail(p, "object runs past the end", off);
      return 0;
    }
    if (type == 0x4)
      return new_bytes(PLIST_DATA, p->b + off, (int)cnt);
    if (type == 0x5)
      return new_bytes(PLIST_STRING, p->b + off, (int)cnt);
    {
      sbuf s = {0, 0, 0, 0};
      for (long long i = 0; i + 1 < cnt; i += 2) {
        unsigned cp = (unsigned)be(p->b + off + i, 2);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < cnt) {
          unsigned lo = (unsigned)be(p->b + off + i + 2, 2);
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
          }
        }
        utf8_put(&s, cp);
      }
      if (s.oom)
        return 0;
      v = new_bytes(PLIST_STRING, s.b ? (char *)s.b : "", s.n);
      free(s.b);
      return v;
    }
  }
  case 0x8: {
    int sz = nib + 1;
    if (sz > 8 || off + sz > p->len) {
      bfail(p, "bad uid", off - 1);
      return 0;
    }
    return plist_new_uid((long long)be(p->b + off, sz));
  }
  case 0xA:
  case 0xD: {
    int isdict = type == 0xD;
    long long refs;
    if (!b_count(p, &off, nib, &cnt)) {
      bfail(p, "bad length", off);
      return 0;
    }
    refs = isdict ? cnt * 2 : cnt;
    if (cnt > p->nobj || refs * p->refsz > p->len - off) {
      bfail(p, "container runs past the end", off);
      return 0;
    }
    v = isdict ? plist_new_dict() : plist_new_array();
    if (!v)
      return 0;
    for (long long i = 0; i < cnt; i++) {
      plist *child = b_obj(p, (long long)be(p->b + off + (isdict ? cnt + i : i) * p->refsz, p->refsz), depth + 1);
      if (!child) {
        plist_free(v);
        return 0;
      }
      if (isdict) {
        plist *key = b_obj(p, (long long)be(p->b + off + i * p->refsz, p->refsz), depth + 1);
        if (!key || key->type != PLIST_STRING) {
          bfail(p, "dict key is not a string", off);
          plist_free(key);
          plist_free(child);
          plist_free(v);
          return 0;
        }
        if (plist_dict_set(v, key->u.str.data, child)) {
          plist_free(key);
          plist_free(v);
          return 0;
        }
        plist_free(key);
      } else if (plist_array_append(v, child)) {
        plist_free(v);
        return 0;
      }
    }
    return v;
  }
  default:
    bfail(p, "unknown object type", off - 1);
    return 0;
  }
}

static plist *parse_bin(const unsigned char *b, int len, plist_error *err)
{
  bp p = {b, len, 0, 0, 0, 0, err, 0};
  const unsigned char *t;
  long long top;
  if (len < 8 + 32 || memcmp(b, "bplist0", 7)) {
    bfail(&p, "not a binary plist", 0);
    return 0;
  }
  t = b + len - 32;
  p.offsz = t[6];
  p.refsz = t[7];
  if (p.offsz < 1 || p.offsz > 8 || p.refsz < 1 || p.refsz > 8) {
    bfail(&p, "bad trailer", len - 32);
    return 0;
  }
  {
    unsigned long long n = be(t + 8, 8), tp = be(t + 16, 8), ot = be(t + 24, 8);
    if (n == 0 || n > 0x7fffffffULL || ot < 8 || ot > (unsigned long long)len - 32 ||
        n * (unsigned)p.offsz > (unsigned long long)len - 32 - ot || tp >= n) {
      bfail(&p, "bad trailer", len - 32);
      return 0;
    }
    p.nobj = (int)n;
    p.table = (long long)ot;
    top = (long long)tp;
  }
  return b_obj(&p, top, 0);
}

plist *plist_parse(const void *buf, int len, plist_error *err)
{
  if (err) {
    err->pos = 0;
    err->msg[0] = '\0';
  }
  if (!buf || len < 0) {
    if (err)
      snprintf(err->msg, sizeof err->msg, "no input");
    return 0;
  }
  if (len >= 6 && !memcmp(buf, "bplist", 6))
    return parse_bin((const unsigned char *)buf, len, err);
  return parse_xml((const char *)buf, len, err);
}

/* ---- XML writer ---- */

static void x_esc(sbuf *s, const char *p, int len)
{
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '<')
      sb_str(s, "&lt;");
    else if (c == '>')
      sb_str(s, "&gt;");
    else if (c == '&')
      sb_str(s, "&amp;");
    else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
      ; /* not representable in XML 1.0 */
    else
      sb_byte(s, c);
  }
}

static void x_indent(sbuf *s, int depth)
{
  for (int i = 0; i < depth; i++)
    sb_byte(s, '\t');
}

static void x_write(sbuf *s, const plist *v, int depth)
{
  char tmp[64];
  x_indent(s, depth);
  switch (v->type) {
  case PLIST_BOOL:
    sb_str(s, v->u.boolean ? "<true/>\n" : "<false/>\n");
    return;
  case PLIST_INT:
  case PLIST_UID:
    snprintf(tmp, sizeof tmp, "<integer>%lld</integer>\n", v->u.integer);
    sb_str(s, tmp);
    return;
  case PLIST_REAL:
    sb_str(s, "<real>");
    if (v->u.real != v->u.real)
      sb_str(s, "nan");
    else if (v->u.real == __builtin_inf())
      sb_str(s, "inf");
    else if (v->u.real == -__builtin_inf())
      sb_str(s, "-inf");
    else {
      dtoa(v->u.real, tmp);
      sb_str(s, tmp);
    }
    sb_str(s, "</real>\n");
    return;
  case PLIST_DATE:
    format_date(v->u.real, tmp, sizeof tmp);
    sb_str(s, "<date>");
    sb_str(s, tmp);
    sb_str(s, "</date>\n");
    return;
  case PLIST_STRING:
    sb_str(s, "<string>");
    x_esc(s, v->u.str.data, v->u.str.len);
    sb_str(s, "</string>\n");
    return;
  case PLIST_DATA: {
    char *enc = (char *)malloc((unsigned)base64_encoded_len(v->u.str.len) + 1);
    if (!enc) {
      s->oom = 1;
      return;
    }
    base64_encode((const unsigned char *)v->u.str.data, v->u.str.len, enc);
    sb_str(s, "<data>");
    sb_str(s, enc);
    sb_str(s, "</data>\n");
    free(enc);
    return;
  }
  case PLIST_ARRAY:
    if (!v->u.array.count) {
      sb_str(s, "<array/>\n");
      return;
    }
    sb_str(s, "<array>\n");
    for (int i = 0; i < v->u.array.count; i++)
      x_write(s, v->u.array.items[i], depth + 1);
    x_indent(s, depth);
    sb_str(s, "</array>\n");
    return;
  case PLIST_DICT:
    if (!v->u.dict.count) {
      sb_str(s, "<dict/>\n");
      return;
    }
    sb_str(s, "<dict>\n");
    for (int i = 0; i < v->u.dict.count; i++) {
      x_indent(s, depth + 1);
      sb_str(s, "<key>");
      x_esc(s, v->u.dict.keys[i], (int)strlen(v->u.dict.keys[i]));
      sb_str(s, "</key>\n");
      x_write(s, v->u.dict.vals[i], depth + 1);
    }
    x_indent(s, depth);
    sb_str(s, "</dict>\n");
    return;
  }
}

char *plist_to_xml(const plist *v, int *out_len)
{
  sbuf s = {0, 0, 0, 0};
  if (!v)
    return 0;
  sb_str(&s, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n");
  x_write(&s, v, 0);
  sb_str(&s, "</plist>\n");
  if (s.oom) {
    free(s.b);
    return 0;
  }
  if (out_len)
    *out_len = s.n;
  return (char *)s.b;
}

/* ---- binary writer ---- */

/* One object of the flattened tree; a dict key is its own string object. refs lists a container's children. */
typedef struct {
  const plist *n;
  const char *key;
  int *refs, nrefs;
} wobj;

typedef struct {
  wobj *o;
  int n, cap, oom;
} wlist;

static int w_add(wlist *l, const plist *n, const char *key)
{
  if (l->n == l->cap) {
    int nc = l->cap ? l->cap * 2 : 32;
    wobj *no = (wobj *)realloc(l->o, (unsigned)nc * sizeof *no);
    if (!no) {
      l->oom = 1;
      return -1;
    }
    l->o = no;
    l->cap = nc;
  }
  memset(&l->o[l->n], 0, sizeof l->o[0]);
  l->o[l->n].n = n;
  l->o[l->n].key = key;
  return l->n++;
}

/* Number the objects in preorder and record each container's child numbers; -1 on failure */
static int w_flatten(wlist *l, const plist *n, const char *key, int depth)
{
  int me, count = 0;
  if (depth > MAX_DEPTH) {
    l->oom = 1;
    return -1;
  }
  me = w_add(l, n, key);
  if (me < 0)
    return -1;
  if (key)
    return me;
  if (n->type == PLIST_ARRAY)
    count = n->u.array.count;
  else if (n->type == PLIST_DICT)
    count = n->u.dict.count * 2;
  if (count) {
    int *refs = (int *)malloc((unsigned)count * sizeof *refs);
    if (!refs) {
      l->oom = 1;
      return -1;
    }
    l->o[me].refs = refs;
    l->o[me].nrefs = count;
  }
  for (int i = 0; i < count; i++) {
    int c;
    if (n->type == PLIST_ARRAY)
      c = w_flatten(l, n->u.array.items[i], 0, depth + 1);
    else if (i < n->u.dict.count)
      c = w_flatten(l, n, n->u.dict.keys[i], depth + 1);
    else
      c = w_flatten(l, n->u.dict.vals[i - n->u.dict.count], 0, depth + 1);
    if (c < 0)
      return -1;
    l->o[me].refs[i] = c; /* o may have moved: index through l */
  }
  return me;
}

static void b_int_bytes(sbuf *s, unsigned long long v, int n)
{
  for (int i = n - 1; i >= 0; i--)
    sb_byte(s, (int)(v >> (i * 8)));
}

static int min_bytes(unsigned long long v) { return v < 0x100 ? 1 : v < 0x10000 ? 2 : v < 0x100000000ULL ? 4 : 8; }
static int size_log2(int nb) { return nb == 1 ? 0 : nb == 2 ? 1 : nb == 4 ? 2 : 3; }

static void b_head(sbuf *s, int type, long long count)
{
  if (count < 15) {
    sb_byte(s, type << 4 | (int)count);
  } else {
    int nb = min_bytes((unsigned long long)count);
    sb_byte(s, type << 4 | 0xF);
    sb_byte(s, 0x10 | size_log2(nb));
    b_int_bytes(s, (unsigned long long)count, nb);
  }
}

/* A string as ASCII, or UTF-16BE when it holds anything else */
static void b_string(sbuf *s, const char *p, int len)
{
  int ascii = 1;
  for (int k = 0; k < len; k++)
    if ((unsigned char)p[k] >= 0x80)
      ascii = 0;
  if (ascii) {
    b_head(s, 0x5, len);
    sb_put(s, p, len);
    return;
  }
  {
    sbuf u16 = {0, 0, 0, 0};
    int units = 0;
    for (int k = 0; k < len;) {
      unsigned char c = (unsigned char)p[k];
      unsigned cp = c;
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      if (extra) {
        cp = c & (0x3F >> extra);
        for (int e = 1; e <= extra && k + e < len; e++)
          cp = cp << 6 | ((unsigned char)p[k + e] & 0x3F);
      }
      k += extra + 1;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        b_int_bytes(&u16, 0xD800 + (cp >> 10), 2);
        b_int_bytes(&u16, 0xDC00 + (cp & 0x3FF), 2);
        units += 2;
      } else {
        b_int_bytes(&u16, cp, 2);
        units++;
      }
    }
    b_head(s, 0x6, units);
    if (u16.n)
      sb_put(s, u16.b, u16.n);
    free(u16.b);
  }
}

static void b_write_obj(sbuf *s, const wobj *o, int refsz)
{
  const plist *n = o->n;
  if (o->key) {
    b_string(s, o->key, (int)strlen(o->key));
    return;
  }
  switch (n->type) {
  case PLIST_BOOL:
    sb_byte(s, n->u.boolean ? 0x09 : 0x08);
    break;
  case PLIST_INT:
    if (n->u.integer >= 0) {
      int nb = min_bytes((unsigned long long)n->u.integer);
      sb_byte(s, 0x10 | size_log2(nb));
      b_int_bytes(s, (unsigned long long)n->u.integer, nb);
    } else {
      sb_byte(s, 0x13);
      b_int_bytes(s, (unsigned long long)n->u.integer, 8);
    }
    break;
  case PLIST_UID: {
    int nb = min_bytes((unsigned long long)n->u.integer);
    sb_byte(s, 0x80 | (nb - 1));
    b_int_bytes(s, (unsigned long long)n->u.integer, nb);
    break;
  }
  case PLIST_REAL:
  case PLIST_DATE: {
    unsigned long long u;
    memcpy(&u, &n->u.real, 8);
    sb_byte(s, n->type == PLIST_REAL ? 0x23 : 0x33);
    b_int_bytes(s, u, 8);
    break;
  }
  case PLIST_DATA:
    b_head(s, 0x4, n->u.str.len);
    if (n->u.str.len)
      sb_put(s, n->u.str.data, n->u.str.len);
    break;
  case PLIST_STRING:
    b_string(s, n->u.str.data, n->u.str.len);
    break;
  case PLIST_ARRAY:
  case PLIST_DICT:
    b_head(s, n->type == PLIST_DICT ? 0xD : 0xA, n->type == PLIST_DICT ? n->u.dict.count : n->u.array.count);
    for (int i = 0; i < o->nrefs; i++)
      b_int_bytes(s, (unsigned)o->refs[i], refsz);
    break;
  }
}

unsigned char *plist_to_bin(const plist *v, int *out_len)
{
  wlist l = {0, 0, 0, 0};
  sbuf s = {0, 0, 0, 0};
  long long *offs = 0;
  unsigned char *result = 0;
  int refsz, offsz;
  if (!v)
    return 0;
  if (w_flatten(&l, v, 0, 0) < 0)
    goto done;
  refsz = min_bytes((unsigned long long)l.n - 1);
  offs = (long long *)malloc((unsigned)l.n * sizeof *offs);
  if (!offs)
    goto done;
  sb_put(&s, "bplist00", 8);
  for (int i = 0; i < l.n; i++) {
    offs[i] = s.n;
    b_write_obj(&s, &l.o[i], refsz);
  }
  offsz = min_bytes((unsigned long long)s.n);
  {
    long long table = s.n;
    for (int i = 0; i < l.n; i++)
      b_int_bytes(&s, (unsigned long long)offs[i], offsz);
    for (int i = 0; i < 6; i++)
      sb_byte(&s, 0);
    sb_byte(&s, offsz);
    sb_byte(&s, refsz);
    b_int_bytes(&s, (unsigned)l.n, 8);
    b_int_bytes(&s, 0, 8);
    b_int_bytes(&s, (unsigned long long)table, 8);
  }
  if (!s.oom) {
    result = s.b;
    s.b = 0;
    if (out_len)
      *out_len = s.n;
  }
done:
  for (int i = 0; i < l.n; i++)
    free(l.o[i].refs);
  free(l.o);
  free(offs);
  free(s.b);
  return result;
}
