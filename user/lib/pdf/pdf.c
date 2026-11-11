/* pdf.c, PDF objects: the tokenizer, cross-reference tables, object streams and the page tree */
#include "owos.h"
#include "pdfint.h"

#define MAX_XREF (1 << 20)
#define MAX_PAGES 100000
#define MAX_DEPTH 32

/* arena */

struct ablock {
  struct ablock *next;
  unsigned used, cap;
  unsigned char mem[];
};

void *arena_alloc(struct arena *a, unsigned n)
{
  n = (n + 7) & ~7u;
  if (!a->head || a->head->cap - a->head->used < n) {
    unsigned cap = n > 65536 ? n : 65536;
    struct ablock *b = malloc(sizeof *b + cap);
    if (!b)
      return 0;
    b->used = 0;
    b->cap = cap;
    b->next = a->head;
    a->head = b;
  }
  void *p = a->head->mem + a->head->used;
  a->head->used += n;
  memset(p, 0, n);
  return p;
}

void arena_free(struct arena *a)
{
  while (a->head) {
    struct ablock *n = a->head->next;
    free(a->head);
    a->head = n;
  }
}

void arena_reset(struct arena *a)
{
  struct ablock *first = a->head;
  while (first && first->next) {
    struct ablock *n = first->next;
    free(first);
    first = n;
  }
  a->head = first;
  if (first)
    first->used = 0;
}

/* tokenizer */

static int is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int is_delim(int c)
{
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}
static int is_reg(int c) { return !is_ws(c) && !is_delim(c); }
static int hexval(int c)
{
  return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

struct pobj pdf_null(void)
{
  struct pobj o;
  memset(&o, 0, sizeof o);
  return o;
}

static void skip_ws(struct lex *l)
{
  while (l->pos < l->len) {
    int c = l->b[l->pos];
    if (is_ws(c))
      l->pos++;
    else if (c == '%')
      while (l->pos < l->len && l->b[l->pos] != 10 && l->b[l->pos] != 13)
        l->pos++;
    else
      break;
  }
}

static int parse_num(const unsigned char *s, int n, struct pobj *o)
{
  int i = 0, neg = 0, digits = 0, dot = 0;
  double v = 0, scale = 1;
  while (i < n && (s[i] == '+' || s[i] == '-')) {
    if (s[i] == '-')
      neg = 1;
    i++;
  }
  for (; i < n; i++) {
    if (s[i] >= '0' && s[i] <= '9') {
      digits++;
      if (dot) {
        scale /= 10;
        v += (s[i] - '0') * scale;
      } else
        v = v * 10 + (s[i] - '0');
    } else if (s[i] == '.' && !dot)
      dot = 1;
    else
      return 0;
  }
  if (!digits)
    return 0;
  if (neg)
    v = -v;
  memset(o, 0, sizeof *o);
  if (!dot && v >= -2147483647.0 && v <= 2147483647.0) {
    o->t = PO_INT;
    o->n = (int)v;
  } else {
    o->t = PO_REAL;
    o->u.r = v;
  }
  return 1;
}

static const char *copy_str(struct lex *l, const unsigned char *s, int n)
{
  char *p = arena_alloc(l->ar, (unsigned)n + 1);
  if (p)
    memcpy(p, s, (unsigned)n);
  return p;
}

static void lex_name(struct lex *l, struct pobj *o)
{
  char tmp[128];
  int n = 0;
  while (l->pos < l->len && is_reg(l->b[l->pos])) {
    int c = l->b[l->pos++];
    if (c == '#' && l->pos + 1 < l->len && hexval(l->b[l->pos]) >= 0 && hexval(l->b[l->pos + 1]) >= 0) {
      c = hexval(l->b[l->pos]) * 16 + hexval(l->b[l->pos + 1]);
      l->pos += 2;
    }
    if (n < 127)
      tmp[n++] = (char)c;
  }
  o->t = PO_NAME;
  o->n = n;
  o->u.s = copy_str(l, (unsigned char *)tmp, n);
}

static void lex_string(struct lex *l, struct pobj *o)
{
  int start = l->pos, depth = 1, i = start;
  while (i < l->len && depth) {
    int c = l->b[i++];
    if (c == '\\')
      i++;
    else if (c == '(')
      depth++;
    else if (c == ')')
      depth--;
  }
  int end = depth ? l->len : i - 1;
  if (end < start)
    end = start;
  unsigned char *out = arena_alloc(l->ar, (unsigned)(end - start) + 1);
  int n = 0;
  for (i = start; out && i < end; i++) {
    int c = l->b[i];
    if (c == '\\' && i + 1 < end) {
      c = l->b[++i];
      if (c >= '0' && c <= '7') {
        int v = c - '0', k = 1;
        while (k < 3 && i + 1 < end && l->b[i + 1] >= '0' && l->b[i + 1] <= '7') {
          v = v * 8 + l->b[++i] - '0';
          k++;
        }
        c = v & 255;
      } else if (c == 'n')
        c = 10;
      else if (c == 'r')
        c = 13;
      else if (c == 't')
        c = 9;
      else if (c == 'b')
        c = 8;
      else if (c == 'f')
        c = 12;
      else if (c == 13) {
        if (i + 1 < end && l->b[i + 1] == 10)
          i++;
        continue;
      } else if (c == 10)
        continue;
    }
    out[n++] = (unsigned char)c;
  }
  l->pos = depth ? l->len : i + 1;
  o->t = PO_STR;
  o->n = n;
  o->u.s = (const char *)out;
}

static void lex_hex(struct lex *l, struct pobj *o)
{
  int i = l->pos;
  while (i < l->len && l->b[i] != '>')
    i++;
  unsigned char *out = arena_alloc(l->ar, (unsigned)(i - l->pos) / 2 + 2);
  int n = 0, hi = -1;
  for (int k = l->pos; out && k < i; k++) {
    int h = hexval(l->b[k]);
    if (h < 0)
      continue;
    if (hi < 0)
      hi = h;
    else {
      out[n++] = (unsigned char)(hi * 16 + h);
      hi = -1;
    }
  }
  if (out && hi >= 0)
    out[n++] = (unsigned char)(hi * 16);
  l->pos = i < l->len ? i + 1 : i;
  o->t = PO_STR;
  o->n = n;
  o->u.s = (const char *)out;
}

struct vec {
  struct pobj *p;
  int n, cap;
};

static int vec_push(struct vec *v, const struct pobj *o)
{
  if (v->n == v->cap) {
    int cap = v->cap ? v->cap * 2 : 8;
    struct pobj *p = realloc(v->p, (unsigned)cap * sizeof *p);
    if (!p)
      return 0;
    v->p = p;
    v->cap = cap;
  }
  v->p[v->n++] = *o;
  return 1;
}

static int lex_depth(struct lex *l, struct pobj *o, int depth);

static int is_kw(const struct pobj *o, const char *s) { return o->t == PO_KW && !strcmp(o->u.s, s); }

static void lex_array(struct lex *l, struct pobj *o, int depth)
{
  struct vec v = {0, 0, 0};
  struct pobj e;
  while (lex_depth(l, &e, depth + 1)) {
    if (is_kw(&e, "]"))
      break;
    if (e.t == PO_KW)
      break;
    if (!vec_push(&v, &e))
      break;
  }
  o->t = PO_ARR;
  o->n = v.n;
  o->u.a = arena_alloc(l->ar, (unsigned)v.n * sizeof *v.p);
  if (o->u.a)
    memcpy(o->u.a, v.p, (unsigned)v.n * sizeof *v.p);
  else
    o->n = 0;
  free(v.p);
}

static void lex_dict(struct lex *l, struct pobj *o, int depth)
{
  struct vec v = {0, 0, 0};
  struct pobj k, e;
  while (lex_depth(l, &k, depth + 1)) {
    if (is_kw(&k, ">>"))
      break;
    if (k.t == PO_KW && !strcmp(k.u.s, "endobj"))
      break;
    if (k.t != PO_NAME)
      continue;
    if (!lex_depth(l, &e, depth + 1) || is_kw(&e, ">>"))
      break;
    if (e.t == PO_KW)
      continue;
    struct pobj pair = k;
    pair.aux = 0;
    if (!vec_push(&v, &pair) || !vec_push(&v, &e))
      break;
  }
  int n = v.n / 2;
  o->t = PO_DICT;
  o->n = n;
  o->u.kv = arena_alloc(l->ar, (unsigned)n * sizeof(struct pkv));
  if (o->u.kv)
    for (int i = 0; i < n; i++) {
      o->u.kv[i].k = v.p[i * 2].u.s;
      o->u.kv[i].v = v.p[i * 2 + 1];
    }
  else
    o->n = 0;
  free(v.p);
}

static int lex_depth(struct lex *l, struct pobj *o, int depth)
{
  memset(o, 0, sizeof *o);
  skip_ws(l);
  if (l->pos >= l->len)
    return 0;
  int c = l->b[l->pos];
  if (depth > MAX_DEPTH + 8) {
    l->pos = l->len;
    return 0;
  }
  if (c == '/') {
    l->pos++;
    lex_name(l, o);
    return 1;
  }
  if (c == '(') {
    l->pos++;
    lex_string(l, o);
    return 1;
  }
  if (c == '[') {
    l->pos++;
    lex_array(l, o, depth);
    return 1;
  }
  if (c == '<') {
    if (l->pos + 1 < l->len && l->b[l->pos + 1] == '<') {
      l->pos += 2;
      lex_dict(l, o, depth);
    } else {
      l->pos++;
      lex_hex(l, o);
    }
    return 1;
  }
  int start = l->pos;
  if (c == '>' && l->pos + 1 < l->len && l->b[l->pos + 1] == '>')
    l->pos += 2;
  else if (is_delim(c))
    l->pos++;
  else
    while (l->pos < l->len && is_reg(l->b[l->pos]))
      l->pos++;
  int n = l->pos - start;
  const unsigned char *s = l->b + start;
  if (!is_delim(c) && parse_num(s, n, o)) {
    if (o->t == PO_INT && o->n >= 0 && l->refs) {
      int save = l->pos;
      skip_ws(l);
      int gs = l->pos;
      while (l->pos < l->len && l->b[l->pos] >= '0' && l->b[l->pos] <= '9')
        l->pos++;
      if (l->pos > gs && l->pos - gs < 6 && l->pos < l->len && is_ws(l->b[l->pos])) {
        int gen = 0;
        for (int i = gs; i < l->pos; i++)
          gen = gen * 10 + l->b[i] - '0';
        skip_ws(l);
        if (l->pos < l->len && l->b[l->pos] == 'R' && (l->pos + 1 >= l->len || !is_reg(l->b[l->pos + 1]))) {
          l->pos++;
          o->t = PO_REF;
          o->aux = gen;
          return 1;
        }
      }
      l->pos = save;
    }
    return 1;
  }
  if (n == 4 && !memcmp(s, "true", 4)) {
    o->t = PO_BOOL;
    o->n = 1;
    return 1;
  }
  if (n == 5 && !memcmp(s, "false", 5)) {
    o->t = PO_BOOL;
    return 1;
  }
  if (n == 4 && !memcmp(s, "null", 4))
    return 1;
  o->t = PO_KW;
  o->n = n;
  o->u.s = copy_str(l, s, n);
  return 1;
}

int pdf_lex(struct lex *l, struct pobj *o) { return lex_depth(l, o, 0); }

/* object access */

double pdf_num(struct pobj o, double def) { return o.t == PO_INT ? (double)o.n : o.t == PO_REAL ? o.u.r : def; }
int pdf_name_is(struct pobj o, const char *s) { return o.t == PO_NAME && !strcmp(o.u.s, s); }
int pdf_is_dict(struct pobj o) { return o.t == PO_DICT || o.t == PO_STREAM; }

static int ensure_x(struct pdf *d, int n)
{
  if (n > MAX_XREF)
    return 0;
  if (n <= d->xcap) {
    if (n > d->nx)
      d->nx = n;
    return 1;
  }
  int cap = d->xcap ? d->xcap : 256;
  while (cap < n)
    cap *= 2;
  struct xent *x = realloc(d->x, (unsigned)cap * sizeof *x);
  if (!x)
    return 0;
  memset(x + d->xcap, 0, (unsigned)(cap - d->xcap) * sizeof *x);
  d->x = x;
  d->xcap = cap;
  d->nx = n;
  return 1;
}

static void lex_init(struct lex *l, struct pdf *d, int pos)
{
  l->b = d->buf;
  l->len = d->len;
  l->pos = pos;
  l->ar = &d->ar;
  l->refs = 1;
}

/* Parse "N G obj <value> [stream]" at `off`; *num gets N. */
static int parse_at(struct pdf *d, int off, int *num, struct pobj *out)
{
  struct lex l;
  struct pobj a, b, c;
  if (off < 0 || off >= d->len)
    return 0;
  lex_init(&l, d, off);
  l.refs = 0;
  if (!pdf_lex(&l, &a) || a.t != PO_INT || !pdf_lex(&l, &b) || b.t != PO_INT || !pdf_lex(&l, &c) || !is_kw(&c, "obj"))
    return 0;
  l.refs = 1;
  if (num)
    *num = a.n;
  if (!pdf_lex(&l, out))
    return 0;
  if (out->t == PO_KW)
    *out = pdf_null();
  if (out->t == PO_DICT) {
    int save = l.pos;
    struct pobj k;
    if (pdf_lex(&l, &k) && is_kw(&k, "stream")) {
      int p = l.pos;
      if (p < d->len && d->buf[p] == 13)
        p++;
      if (p < d->len && d->buf[p] == 10)
        p++;
      out->t = PO_STREAM;
      out->aux = p;
    } else
      l.pos = save;
  }
  return 1;
}

static const unsigned char *find_bytes(const unsigned char *h, int hn, const char *needle)
{
  int n = strlen(needle);
  for (int i = 0; i + n <= hn; i++)
    if (h[i] == (unsigned char)needle[0] && !memcmp(h + i, needle, (unsigned)n))
      return h + i;
  return 0;
}

static struct ostm *ostm_get(struct pdf *d, int num);

static struct pobj *obj_load(struct pdf *d, int num)
{
  static struct pobj null_obj;
  if (num < 0 || num >= d->nx)
    return &null_obj;
  struct xent *e = &d->x[num];
  if (e->state == 2)
    return &e->obj;
  if (e->state == 1 || e->type == 0 || e->type == 3)
    return &null_obj;
  e->state = 1;
  struct pobj o = pdf_null();
  if (e->type == 1) {
    int n = -1;
    if (!parse_at(d, e->a, &n, &o) || n != num)
      o = pdf_null();
  } else {
    struct ostm *os = ostm_get(d, e->a);
    if (os) {
      int idx = e->b;
      if (idx < 0 || idx >= os->n || os->nums[idx] != num) {
        idx = -1;
        for (int i = 0; i < os->n; i++)
          if (os->nums[i] == num)
            idx = i;
      }
      if (idx >= 0) {
        struct lex l;
        lex_init(&l, d, 0);
        l.b = os->data;
        l.len = os->len;
        l.pos = os->first + os->offs[idx];
        if (l.pos >= os->len || !pdf_lex(&l, &o) || o.t == PO_KW)
          o = pdf_null();
      }
    }
  }
  e = &d->x[num];
  e->obj = o;
  e->state = 2;
  return &e->obj;
}

struct pobj pdf_deref(struct pdf *d, struct pobj o)
{
  for (int i = 0; i < 16 && o.t == PO_REF; i++)
    o = *obj_load(d, o.n);
  return o.t == PO_REF ? pdf_null() : o;
}

struct pobj pdf_getraw(struct pdf *d, struct pobj dict, const char *key)
{
  (void)d;
  if (dict.t != PO_DICT && dict.t != PO_STREAM)
    return pdf_null();
  for (int i = 0; i < dict.n; i++)
    if (!strcmp(dict.u.kv[i].k, key))
      return dict.u.kv[i].v;
  return pdf_null();
}

struct pobj pdf_get(struct pdf *d, struct pobj dict, const char *key) { return pdf_deref(d, pdf_getraw(d, dict, key)); }

struct pobj pdf_at(struct pdf *d, struct pobj arr, int i)
{
  if (arr.t != PO_ARR || i < 0 || i >= arr.n)
    return pdf_null();
  return pdf_deref(d, arr.u.a[i]);
}

/* streams */

static int stream_len(struct pdf *d, struct pobj st)
{
  int start = st.aux, avail = d->len - start;
  if (avail < 0)
    return 0;
  struct pobj lo = pdf_get(d, st, "Length");
  if (lo.t == PO_INT && lo.n >= 0 && lo.n <= avail) {
    int p = start + lo.n;
    while (p < d->len && is_ws(d->buf[p]))
      p++;
    if (d->len - p >= 9 && !memcmp(d->buf + p, "endstream", 9))
      return lo.n;
  }
  const unsigned char *e = find_bytes(d->buf + start, avail, "endstream");
  int n = e ? (int)(e - (d->buf + start)) : avail;
  if (n > 0 && d->buf[start + n - 1] == 10)
    n--;
  if (n > 0 && d->buf[start + n - 1] == 13)
    n--;
  return n;
}

unsigned char *pdf_stream_data(struct pdf *d, struct pobj st, int *len, const char **left, struct pobj *left_parms)
{
  if (st.t != PO_STREAM) {
    if (left)
      *left = 0;
    unsigned char *z = malloc(1);
    *len = 0;
    return z;
  }
  int n = stream_len(d, st);
  struct pobj fl = pdf_get(d, st, "Filter"), dp = pdf_get(d, st, "DecodeParms");
  if (fl.t == PO_NULL)
    fl = pdf_get(d, st, "F");
  if (dp.t == PO_NULL)
    dp = pdf_get(d, st, "DP");
  return pdf_decode(d, d->buf + st.aux, n, fl, dp, len, left, left_parms);
}

static struct ostm *ostm_get(struct pdf *d, int num)
{
  for (int i = 0; i < 8; i++)
    if (d->os[i].data && d->os[i].num == num)
      return &d->os[i];
  if (num < 0 || num >= d->nx || d->x[num].type != 1)
    return 0;
  struct pobj st = *obj_load(d, num);
  if (st.t != PO_STREAM)
    return 0;
  int n = (int)pdf_num(pdf_get(d, st, "N"), 0), first = (int)pdf_num(pdf_get(d, st, "First"), 0);
  if (n <= 0 || n > MAX_XREF || first < 0)
    return 0;
  struct ostm *os = &d->os[d->os_next];
  d->os_next = (d->os_next + 1) & 7;
  free(os->data);
  free(os->nums);
  free(os->offs);
  memset(os, 0, sizeof *os);
  int len = 0;
  os->data = pdf_stream_data(d, st, &len, 0, 0);
  os->nums = malloc((unsigned)n * sizeof(int));
  os->offs = malloc((unsigned)n * sizeof(int));
  if (!os->data || !os->nums || !os->offs || first > len) {
    free(os->data);
    free(os->nums);
    free(os->offs);
    memset(os, 0, sizeof *os);
    return 0;
  }
  os->len = len;
  os->first = first;
  struct lex l = {os->data, first, 0, &d->ar, 0};
  int got = 0;
  for (; got < n; got++) {
    struct pobj a, b;
    if (!pdf_lex(&l, &a) || a.t != PO_INT || !pdf_lex(&l, &b) || b.t != PO_INT)
      break;
    os->nums[got] = a.n;
    os->offs[got] = b.n;
  }
  os->n = got;
  os->num = num;
  return os;
}

/* cross-reference loading */

static void set_entry(struct pdf *d, int num, int type, int a, int b)
{
  if (num < 0 || !ensure_x(d, num + 1) || d->x[num].type)
    return;
  d->x[num].type = (unsigned char)type;
  d->x[num].a = a;
  d->x[num].b = b;
}

static void take_trailer(struct pdf *d, struct pobj t)
{
  if (d->trailer.t == PO_NULL && t.t != PO_NULL)
    d->trailer = t;
}

/* One section at `off`; *prev gets /Prev, *stm /XRefStm (or -1). 0 if it is unreadable. */
static int xref_section(struct pdf *d, int off, int *prev, int *stm)
{
  struct lex l;
  struct pobj t;
  *prev = *stm = -1;
  if (off < 0 || off >= d->len)
    return 0;
  lex_init(&l, d, off);
  l.refs = 0;
  if (pdf_lex(&l, &t) && is_kw(&t, "xref")) {
    for (;;) {
      struct pobj a, b;
      int save = l.pos;
      if (!pdf_lex(&l, &a))
        return 0;
      if (is_kw(&a, "trailer"))
        break;
      if (a.t != PO_INT || !pdf_lex(&l, &b) || b.t != PO_INT) {
        l.pos = save;
        return 0;
      }
      int start = a.n;
      for (int i = 0; i < b.n; i++) {
        struct pobj o, g, k;
        if (!pdf_lex(&l, &o) || !pdf_lex(&l, &g) || !pdf_lex(&l, &k) || k.t != PO_KW)
          return 0;
        if (i == 0 && start == 1 && k.u.s[0] == 'f' && pdf_num(g, 0) == 65535)
          start = 0;
        if (k.u.s[0] == 'n')
          set_entry(d, start + i, 1, (int)pdf_num(o, 0), (int)pdf_num(g, 0));
        else
          set_entry(d, start + i, 3, 0, 0);
      }
    }
    l.refs = 1;
    struct pobj tr;
    if (!pdf_lex(&l, &tr) || tr.t != PO_DICT)
      return 0;
    take_trailer(d, tr);
    *prev = (int)pdf_num(pdf_getraw(d, tr, "Prev"), -1);
    *stm = (int)pdf_num(pdf_getraw(d, tr, "XRefStm"), -1);
    return 1;
  }
  struct pobj o;
  int num;
  if (!parse_at(d, off, &num, &o) || o.t != PO_STREAM)
    return 0;
  struct pobj w = pdf_get(d, o, "W"), idx = pdf_get(d, o, "Index");
  int w0 = (int)pdf_num(pdf_at(d, w, 0), 0), w1 = (int)pdf_num(pdf_at(d, w, 1), 0),
      w2 = (int)pdf_num(pdf_at(d, w, 2), 0);
  int size = (int)pdf_num(pdf_get(d, o, "Size"), 0);
  if (w.t != PO_ARR || w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8 || w0 + w1 + w2 == 0)
    return 0;
  int len = 0;
  unsigned char *data = pdf_stream_data(d, o, &len, 0, 0);
  if (!data)
    return 0;
  int row = w0 + w1 + w2, pos = 0, nsub = idx.t == PO_ARR ? idx.n / 2 : 1;
  for (int s = 0; s < nsub; s++) {
    int start = idx.t == PO_ARR ? (int)pdf_num(pdf_at(d, idx, s * 2), 0) : 0;
    int count = idx.t == PO_ARR ? (int)pdf_num(pdf_at(d, idx, s * 2 + 1), 0) : size;
    for (int i = 0; i < count && pos + row <= len; i++, pos += row) {
      unsigned long long f[3] = {w0 ? 0 : 1, 0, 0};
      const unsigned char *p = data + pos;
      for (int k = 0; k < 3; k++) {
        int wk = k == 0 ? w0 : k == 1 ? w1 : w2;
        unsigned long long v = 0;
        for (int j = 0; j < wk; j++)
          v = (v << 8) | *p++;
        if (wk)
          f[k] = v;
      }
      if (f[0] == 1)
        set_entry(d, start + i, 1, (int)f[1], (int)f[2]);
      else if (f[0] == 2)
        set_entry(d, start + i, 2, (int)f[1], (int)f[2]);
      else
        set_entry(d, start + i, 3, 0, 0);
    }
  }
  free(data);
  take_trailer(d, o);
  *prev = (int)pdf_num(pdf_getraw(d, o, "Prev"), -1);
  return 1;
}

static int load_xref(struct pdf *d)
{
  int from = d->len > 2048 ? d->len - 2048 : 0;
  const unsigned char *p = 0, *q = d->buf + from;
  while ((q = find_bytes(q, d->len - (int)(q - d->buf), "startxref")) != 0) {
    p = q;
    q += 9;
  }
  if (!p)
    return 0;
  struct lex l;
  struct pobj o;
  lex_init(&l, d, (int)(p - d->buf) + 9);
  l.refs = 0;
  if (!pdf_lex(&l, &o) || o.t != PO_INT)
    return 0;
  int off = o.n, prev, stm;
  for (int hops = 0; hops < 256 && off >= 0; hops++) {
    if (!xref_section(d, off, &prev, &stm))
      return hops > 0;
    if (stm >= 0) {
      int p2, s2;
      xref_section(d, stm, &p2, &s2);
    }
    off = prev;
  }
  return 1;
}

/* Rebuild the table by scanning for "N G obj" and taking the last trailer found. */
static void rebuild_xref(struct pdf *d)
{
  d->rebuilt = 1;
  for (int i = 0; i < d->nx; i++)
    d->x[i].type = d->x[i].state = 0;
  d->trailer = pdf_null();
  for (int i = 0; i < 8; i++) {
    free(d->os[i].data);
    free(d->os[i].nums);
    free(d->os[i].offs);
    memset(&d->os[i], 0, sizeof d->os[i]);
  }
  for (int i = 3; i + 3 <= d->len; i++) {
    if (d->buf[i] == 'o' && d->buf[i + 1] == 'b' && d->buf[i + 2] == 'j' &&
        (i + 3 == d->len || !is_reg(d->buf[i + 3])) && !(i >= 3 && !memcmp(d->buf + i - 3, "end", 3))) {
      int j = i - 1;
      while (j >= 0 && is_ws(d->buf[j]))
        j--;
      int ge = j + 1;
      while (j >= 0 && d->buf[j] >= '0' && d->buf[j] <= '9')
        j--;
      int gs = j + 1;
      if (gs == ge || j < 0 || !is_ws(d->buf[j]))
        continue;
      while (j >= 0 && is_ws(d->buf[j]))
        j--;
      int ne = j + 1;
      while (j >= 0 && d->buf[j] >= '0' && d->buf[j] <= '9')
        j--;
      int ns = j + 1;
      if (ns == ne || ne - ns > 9)
        continue;
      int num = 0;
      for (int k = ns; k < ne; k++)
        num = num * 10 + d->buf[k] - '0';
      if (!ensure_x(d, num + 1))
        continue;
      d->x[num].type = 1;
      d->x[num].state = 0;
      d->x[num].a = ns;
      d->x[num].b = 0;
    } else if (d->buf[i] == 't' && d->len - i >= 7 && !memcmp(d->buf + i, "trailer", 7)) {
      struct lex l;
      struct pobj t;
      lex_init(&l, d, i + 7);
      if (pdf_lex(&l, &t) && t.t == PO_DICT && pdf_getraw(d, t, "Root").t == PO_REF) {
        d->trailer = pdf_null();
        take_trailer(d, t);
      }
    }
  }
  int root = -1;
  for (int num = 0; num < d->nx; num++) {
    if (d->x[num].type != 1)
      continue;
    struct pobj o = *obj_load(d, num);
    struct pobj ty = pdf_get(d, o, "Type");
    if (pdf_name_is(ty, "ObjStm") && o.t == PO_STREAM) {
      struct ostm *os = ostm_get(d, num);
      if (os)
        for (int k = 0; k < os->n; k++)
          if (os->nums[k] >= 0 && ensure_x(d, os->nums[k] + 1) && !d->x[os->nums[k]].type)
            set_entry(d, os->nums[k], 2, num, k);
    } else if (pdf_name_is(ty, "Catalog"))
      root = num;
    else if (pdf_name_is(ty, "XRef") && d->trailer.t == PO_NULL && pdf_getraw(d, o, "Root").t == PO_REF)
      d->trailer = o;
  }
  for (int num = 0; root < 0 && num < d->nx; num++)
    if (d->x[num].type == 2 && pdf_name_is(pdf_get(d, *obj_load(d, num), "Type"), "Catalog"))
      root = num;
  if (pdf_getraw(d, d->trailer, "Root").t != PO_REF && root >= 0) {
    struct pkv *kv = arena_alloc(&d->ar, sizeof *kv);
    if (kv) {
      kv->k = "Root";
      kv->v.t = PO_REF;
      kv->v.n = root;
      d->trailer.t = PO_DICT;
      d->trailer.n = 1;
      d->trailer.u.kv = kv;
    }
  }
}

/* page tree */

struct inherit {
  struct pobj res, media, crop;
  int rot;
};

static int g_nodes;

static void walk_pages(struct pdf *d, struct pobj node, struct inherit in, int depth)
{
  node = pdf_deref(d, node);
  if (node.t != PO_DICT || depth > MAX_DEPTH || d->npages >= MAX_PAGES || ++g_nodes > 4 * MAX_PAGES)
    return;
  struct pobj v;
  if ((v = pdf_get(d, node, "Resources")).t == PO_DICT)
    in.res = v;
  if ((v = pdf_get(d, node, "MediaBox")).t == PO_ARR)
    in.media = v;
  if ((v = pdf_get(d, node, "CropBox")).t == PO_ARR)
    in.crop = v;
  if ((v = pdf_get(d, node, "Rotate")).t == PO_INT || v.t == PO_REAL)
    in.rot = (int)pdf_num(v, 0);
  struct pobj kids = pdf_get(d, node, "Kids");
  if (kids.t == PO_ARR && !pdf_name_is(pdf_get(d, node, "Type"), "Page")) {
    for (int i = 0; i < kids.n; i++)
      walk_pages(d, kids.u.a[i], in, depth + 1);
    return;
  }
  if (d->npages == d->pcap) {
    int cap = d->pcap ? d->pcap * 2 : 64;
    struct ppage *np = realloc(d->pages, (unsigned)cap * sizeof *np);
    if (!np)
      return;
    d->pages = np;
    d->pcap = cap;
  }
  d->pages[d->npages++] = (struct ppage){node, in.res, in.media, in.crop, in.rot};
}

static int find_pages(struct pdf *d)
{
  struct pobj root = pdf_get(d, d->trailer, "Root");
  struct pobj pages = pdf_getraw(d, root, "Pages");
  free(d->pages);
  d->pages = 0;
  d->npages = d->pcap = 0;
  g_nodes = 0;
  struct inherit in;
  memset(&in, 0, sizeof in);
  walk_pages(d, pages, in, 0);
  return d->npages > 0;
}

int pdf_open(const unsigned char *data, int len, struct pdf **out)
{
  *out = 0;
  int skip = -1;
  for (int i = 0; i + 5 <= len && i < 1024 && skip < 0; i++)
    if (!memcmp(data + i, "%PDF-", 5))
      skip = i;
  if (skip < 0)
    return PDF_EFORMAT;
  struct pdf *d = calloc(1, sizeof *d);
  if (!d)
    return PDF_ENOMEM;
  d->buf = data + skip;
  d->len = len - skip;
  if (!ensure_x(d, 1)) {
    free(d);
    return PDF_ENOMEM;
  }
  int ok = load_xref(d) && pdf_get(d, d->trailer, "Root").t == PO_DICT && find_pages(d);
  if (!ok) {
    rebuild_xref(d);
    ok = pdf_get(d, d->trailer, "Root").t == PO_DICT && find_pages(d);
  }
  int rc = ok ? PDF_OK : PDF_EFORMAT;
  if (ok && pdf_getraw(d, d->trailer, "Encrypt").t != PO_NULL)
    rc = PDF_EENCRYPT;
  if (rc) {
    pdf_close(d);
    return rc;
  }
  *out = d;
  return PDF_OK;
}

void pdf_close(struct pdf *d)
{
  if (!d)
    return;
  pdf_fonts_free(d);
  for (int i = 0; i < 8; i++) {
    free(d->os[i].data);
    free(d->os[i].nums);
    free(d->os[i].offs);
  }
  free(d->pages);
  free(d->x);
  arena_free(&d->ar);
  free(d);
}

void pdf_set_fonts(struct pdf *d, const struct font *sans, const struct font *mono)
{
  d->sans = sans;
  d->mono = mono;
}

int pdf_page_count(const struct pdf *d) { return d->npages; }
