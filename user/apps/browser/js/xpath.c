/* XPath 1.0: document.evaluate, XPathEvaluator, XPathExpression and XPathResult. */

#include "bindings.h"
#include "math.h"

enum { V_NODES, V_NUM, V_STR, V_BOOL };
enum { E_OR, E_AND, E_EQ, E_REL, E_ADD, E_MUL, E_NEG, E_UNION, E_NUM, E_STR, E_FUNC, E_PATH };
enum {
  AX_ANCESTOR,
  AX_ANCESTOR_OR_SELF,
  AX_ATTRIBUTE,
  AX_CHILD,
  AX_DESCENDANT,
  AX_DESCENDANT_OR_SELF,
  AX_FOLLOWING,
  AX_FOLLOWING_SIBLING,
  AX_NAMESPACE,
  AX_PARENT,
  AX_PRECEDING,
  AX_PRECEDING_SIBLING,
  AX_SELF
};
enum { T_NAME, T_ANY, T_TEXT, T_COMMENT, T_NODE, T_PI };
enum { PARSE_DEPTH_MAX = 100, EVAL_DEPTH_MAX = 200 };

/* a node, or an attribute of element `n` when `a` is set */
typedef struct {
  struct dom_node *n;
  struct dom_attr *a;
} xn;
typedef struct {
  xn *v;
  int n, cap;
} xl;
typedef struct {
  int t;
  xl ns;
  double num;
  const char *str;
  int b;
} xv;

struct xe;
struct xstep {
  int axis, test;
  char name[64];
  struct xe **preds;
  int npreds;
};
struct xe {
  int k, op;
  double num;
  const char *str;
  struct xe *a, *b;
  struct xe **args;
  int nargs;
  int abs;
  struct xe *filter;
  struct xe **fpreds;
  int nfpreds;
  struct xstep *steps;
  int nsteps;
};

/* One evaluation at a time: every allocation lives in `blk` and is freed together. */
enum { ERR_SYNTAX, ERR_NAMESPACE, ERR_THROWN };
struct xstate {
  void **blk;
  int nb, cap;
  const char *err;
  int err_kind;
  const char *p;
  int depth;
  js_value resolver;
  struct {
    char prefix[32];
    const char *uri;
  } pfx[16];
  int npfx;
};
static struct xstate X;

static void *palloc(unsigned n)
{
  void *p = calloc(1, n ? n : 1);
  if (!p) {
    X.err = "out of memory";
    return 0;
  }
  if (X.nb == X.cap) {
    int nc = X.cap ? X.cap * 2 : 64;
    void **nb = (void **)realloc(X.blk, (unsigned)nc * sizeof *nb);
    if (!nb) {
      free(p);
      X.err = "out of memory";
      return 0;
    }
    X.blk = nb;
    X.cap = nc;
  }
  X.blk[X.nb++] = p;
  return p;
}
static void pool_reset(void)
{
  for (int i = 0; i < X.nb; i++)
    free(X.blk[i]);
  free(X.blk);
  memset(&X, 0, sizeof X);
}
static const char *pstr(const char *s, int n)
{
  char *d = (char *)palloc((unsigned)n + 1);
  if (!d)
    return "";
  memcpy(d, s, (unsigned)n);
  return d;
}

static int ncmp(const char *a, const char *b, int n)
{
  for (; n > 0; n--, a++, b++) {
    if (*a != *b)
      return (unsigned char)*a - (unsigned char)*b;
    if (!*a)
      return 0;
  }
  return 0;
}
static const char *xstr(const char *h, const char *nd)
{
  int n = strlen(nd);
  for (; *h; h++)
    if (!ncmp(h, nd, n))
      return h;
  return n ? 0 : h;
}

static void xl_push(xl *l, xn x)
{
  if (l->n == l->cap) {
    int nc = l->cap ? l->cap * 2 : 8;
    xn *nv = (xn *)palloc((unsigned)nc * sizeof *nv);
    if (!nv)
      return;
    if (l->n)
      memcpy(nv, l->v, (unsigned)l->n * sizeof *nv);
    l->v = nv;
    l->cap = nc;
  }
  l->v[l->n++] = x;
}
static xn mk(struct dom_node *n)
{
  xn x = {n, 0};
  return x;
}

/* ------------------------------------------------------- parser ----------- */

static struct xe *enew(int k)
{
  struct xe *e = (struct xe *)palloc(sizeof *e);
  if (e)
    e->k = k;
  return e;
}
static void fail(const char *m)
{
  if (!X.err)
    X.err = m;
}
static void ws(void)
{
  while (*X.p == ' ' || *X.p == '\t' || *X.p == '\n' || *X.p == '\r')
    X.p++;
}
static int accept(const char *s)
{
  unsigned n = strlen(s);
  ws();
  if (ncmp(X.p, s, n))
    return 0;
  X.p += n;
  return 1;
}
static int is_ns(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c & 0x80); }
static int is_nc(char c) { return is_ns(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
static int accept_word(const char *w)
{
  unsigned n = strlen(w);
  ws();
  if (ncmp(X.p, w, n) || is_nc(X.p[n]))
    return 0;
  X.p += n;
  return 1;
}
/* a QName or prefix:* into out; returns its length (0 if none) */
static int read_name(char *out, int cap)
{
  int n = 0;
  if (!is_ns(*X.p))
    return 0;
  while (is_nc(*X.p) && n < cap - 1)
    out[n++] = lower(*X.p++);
  if (*X.p == ':' && X.p[1] != ':' && (is_ns(X.p[1]) || X.p[1] == '*') && n < cap - 1) {
    out[n++] = *X.p++;
    while ((is_nc(*X.p) || *X.p == '*') && n < cap - 1)
      out[n++] = lower(*X.p++);
  }
  out[n] = 0;
  return n;
}

static double str_to_num(const char *s, int n)
{
  int i = 0, e = n, digits = 0;
  js_value str;
  while (i < e && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
    i++;
  while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r'))
    e--;
  {
    int j = i;
    if (j < e && s[j] == '-')
      j++;
    while (j < e && s[j] >= '0' && s[j] <= '9')
      j++, digits++;
    if (j < e && s[j] == '.') {
      j++;
      while (j < e && s[j] >= '0' && s[j] <= '9')
        j++, digits++;
    }
    if (j != e || !digits)
      return __builtin_nan("");
  }
  str = js_mk_string(g_ctx, s + i, e - i);
  return js_to_number(g_ctx, str);
}

static struct xe *parse_or(void);

static struct xe *bin(int k, int op, struct xe *a, struct xe *b)
{
  struct xe *e = enew(k);
  if (e) {
    e->op = op;
    e->a = a;
    e->b = b;
  }
  return e;
}
static void add_step(struct xe *e, int axis, int test, const char *name)
{
  struct xstep *ns = (struct xstep *)palloc((unsigned)(e->nsteps + 1) * sizeof *ns);
  if (!ns)
    return;
  if (e->nsteps)
    memcpy(ns, e->steps, (unsigned)e->nsteps * sizeof *ns);
  e->steps = ns;
  ns += e->nsteps++;
  ns->axis = axis;
  ns->test = test;
  if (name)
    strlcpy(ns->name, name, sizeof ns->name);
}
static struct xe **append_expr(struct xe **arr, int n, struct xe *x)
{
  struct xe **na = (struct xe **)palloc((unsigned)(n + 1) * sizeof *na);
  if (!na)
    return arr;
  if (n)
    memcpy(na, arr, (unsigned)n * sizeof *na);
  na[n] = x;
  return na;
}
static void parse_preds(struct xe ***arr, int *n)
{
  while (!X.err && accept("[")) {
    struct xe *p = parse_or();
    if (!accept("]"))
      fail("expected ]");
    *arr = append_expr(*arr, *n, p);
    (*n)++;
  }
}

static int axis_by_name(const char *s)
{
  static const struct {
    const char *n;
    int ax;
  } t[] = {{"ancestor", AX_ANCESTOR},
           {"ancestor-or-self", AX_ANCESTOR_OR_SELF},
           {"attribute", AX_ATTRIBUTE},
           {"child", AX_CHILD},
           {"descendant", AX_DESCENDANT},
           {"descendant-or-self", AX_DESCENDANT_OR_SELF},
           {"following", AX_FOLLOWING},
           {"following-sibling", AX_FOLLOWING_SIBLING},
           {"namespace", AX_NAMESPACE},
           {"parent", AX_PARENT},
           {"preceding", AX_PRECEDING},
           {"preceding-sibling", AX_PRECEDING_SIBLING},
           {"self", AX_SELF}};
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(s, t[i].n))
      return t[i].ax;
  return -1;
}
static int node_type_test(const char *s)
{
  if (!strcmp(s, "text"))
    return T_TEXT;
  if (!strcmp(s, "comment"))
    return T_COMMENT;
  if (!strcmp(s, "node"))
    return T_NODE;
  if (!strcmp(s, "processing-instruction"))
    return T_PI;
  return -1;
}

static void parse_step(struct xe *e)
{
  char name[64];
  int axis = AX_CHILD, test, tt;
  ws();
  if (X.p[0] == '.' && X.p[1] == '.') {
    X.p += 2;
    add_step(e, AX_PARENT, T_NODE, 0);
    return;
  }
  if (*X.p == '.') {
    X.p++;
    add_step(e, AX_SELF, T_NODE, 0);
    return;
  }
  if (*X.p == '@') {
    X.p++;
    axis = AX_ATTRIBUTE;
  } else {
    const char *save = X.p;
    if (read_name(name, sizeof name)) {
      ws();
      if (X.p[0] == ':' && X.p[1] == ':') {
        axis = axis_by_name(name);
        if (axis < 0) {
          fail("unknown axis");
          return;
        }
        X.p += 2;
      } else
        X.p = save;
    } else
      X.p = save;
  }
  ws();
  name[0] = 0;
  if (*X.p == '*') {
    X.p++;
    test = T_ANY;
  } else if (read_name(name, sizeof name)) {
    int n = (int)strlen(name);
    if (n > 1 && name[n - 1] == '*') {
      test = T_ANY; /* prefix:* matches by prefix */
    } else if ((tt = node_type_test(name)) >= 0 && (ws(), *X.p == '(')) {
      X.p++;
      ws();
      if (tt == T_PI && (*X.p == '"' || *X.p == '\'')) {
        char q = *X.p++;
        while (*X.p && *X.p != q)
          X.p++;
        if (*X.p)
          X.p++;
      }
      if (!accept(")")) {
        fail("expected )");
        return;
      }
      test = tt;
      name[0] = 0;
    } else
      test = T_NAME;
  } else {
    fail("expected a node test");
    return;
  }
  add_step(e, axis, test, name);
  if (e->nsteps) {
    struct xstep *st = &e->steps[e->nsteps - 1];
    parse_preds(&st->preds, &st->npreds);
  }
}
static void parse_steps(struct xe *e)
{
  for (;;) {
    parse_step(e);
    if (X.err)
      return;
    ws();
    if (X.p[0] == '/' && X.p[1] == '/') {
      X.p += 2;
      add_step(e, AX_DESCENDANT_OR_SELF, T_NODE, 0);
    } else if (*X.p == '/')
      X.p++;
    else
      return;
  }
}
static int step_start(void)
{
  ws();
  return is_ns(*X.p) || *X.p == '*' || *X.p == '@' || *X.p == '.';
}
static int primary_start(void)
{
  ws();
  if ((*X.p >= '0' && *X.p <= '9') || *X.p == '"' || *X.p == '\'' || *X.p == '(' || *X.p == '$')
    return 1;
  if (X.p[0] == '.' && X.p[1] >= '0' && X.p[1] <= '9')
    return 1;
  if (is_ns(*X.p)) {
    const char *save = X.p;
    char name[64];
    int r;
    read_name(name, sizeof name);
    ws();
    r = *X.p == '(' && node_type_test(name) < 0;
    X.p = save;
    return r;
  }
  return 0;
}
static struct xe *parse_primary(void)
{
  struct xe *e;
  ws();
  if (*X.p == '(') {
    X.p++;
    e = parse_or();
    if (!accept(")"))
      fail("expected )");
    return e;
  }
  if (*X.p == '"' || *X.p == '\'') {
    char q = *X.p++;
    const char *s = X.p;
    while (*X.p && *X.p != q)
      X.p++;
    if (!*X.p) {
      fail("unterminated string");
      return 0;
    }
    e = enew(E_STR);
    if (e)
      e->str = pstr(s, (int)(X.p - s));
    X.p++;
    return e;
  }
  if ((*X.p >= '0' && *X.p <= '9') || *X.p == '.') {
    const char *s = X.p;
    while (*X.p >= '0' && *X.p <= '9')
      X.p++;
    if (*X.p == '.') {
      X.p++;
      while (*X.p >= '0' && *X.p <= '9')
        X.p++;
    }
    e = enew(E_NUM);
    if (e)
      e->num = str_to_num(s, (int)(X.p - s));
    return e;
  }
  if (is_ns(*X.p)) {
    char name[64];
    read_name(name, sizeof name);
    e = enew(E_FUNC);
    if (!e)
      return 0;
    e->str = pstr(name, (int)strlen(name));
    if (!accept("(")) {
      fail("expected (");
      return 0;
    }
    if (!accept(")")) {
      do {
        e->args = append_expr(e->args, e->nargs, parse_or());
        e->nargs++;
      } while (!X.err && accept(","));
      if (!accept(")"))
        fail("expected )");
    }
    return e;
  }
  fail("unsupported expression");
  return 0;
}
static struct xe *parse_path(void)
{
  struct xe *e = enew(E_PATH);
  if (!e)
    return 0;
  ws();
  if (*X.p == '/') {
    e->abs = 1;
    X.p++;
    if (*X.p == '/') {
      X.p++;
      add_step(e, AX_DESCENDANT_OR_SELF, T_NODE, 0);
      parse_steps(e);
    } else if (step_start())
      parse_steps(e);
    return e;
  }
  if (primary_start()) {
    e->filter = parse_primary();
    parse_preds(&e->fpreds, &e->nfpreds);
    ws();
    if (X.p[0] == '/' && X.p[1] == '/') {
      X.p += 2;
      add_step(e, AX_DESCENDANT_OR_SELF, T_NODE, 0);
      parse_steps(e);
    } else if (*X.p == '/') {
      X.p++;
      parse_steps(e);
    }
    if (!e->nfpreds && !e->nsteps)
      return e->filter;
    return e;
  }
  parse_steps(e);
  return e;
}
static struct xe *parse_union(void)
{
  struct xe *l = parse_path();
  for (;;) {
    ws();
    if (X.err || *X.p != '|')
      return l;
    X.p++;
    l = bin(E_UNION, 0, l, parse_path());
  }
}
static struct xe *parse_unary(void)
{
  ws();
  if (*X.p == '-') {
    X.p++;
    return bin(E_NEG, 0, parse_unary(), 0);
  }
  return parse_union();
}
static struct xe *parse_mul(void)
{
  struct xe *l = parse_unary();
  while (!X.err) {
    int op;
    ws();
    if (*X.p == '*') {
      X.p++;
      op = 0;
    } else if (accept_word("div"))
      op = 1;
    else if (accept_word("mod"))
      op = 2;
    else
      break;
    l = bin(E_MUL, op, l, parse_unary());
  }
  return l;
}
static struct xe *parse_add(void)
{
  struct xe *l = parse_mul();
  while (!X.err) {
    int op;
    ws();
    if (*X.p == '+')
      op = 0;
    else if (*X.p == '-')
      op = 1;
    else
      break;
    X.p++;
    l = bin(E_ADD, op, l, parse_mul());
  }
  return l;
}
static struct xe *parse_rel(void)
{
  struct xe *l = parse_add();
  while (!X.err) {
    int op;
    if (accept("<="))
      op = 3;
    else if (accept(">="))
      op = 5;
    else if (accept("<"))
      op = 2;
    else if (accept(">"))
      op = 4;
    else
      break;
    l = bin(E_REL, op, l, parse_add());
  }
  return l;
}
static struct xe *parse_eq(void)
{
  struct xe *l = parse_rel();
  while (!X.err) {
    int op;
    if (accept("!="))
      op = 1;
    else if (accept("="))
      op = 0;
    else
      break;
    l = bin(E_EQ, op, l, parse_rel());
  }
  return l;
}
static struct xe *parse_and(void)
{
  struct xe *l = parse_eq();
  while (!X.err && accept_word("and"))
    l = bin(E_AND, 0, l, parse_eq());
  return l;
}
static struct xe *parse_or(void)
{
  struct xe *l;
  if (++X.depth > PARSE_DEPTH_MAX) {
    fail("expression too deeply nested");
    X.depth--;
    return 0;
  }
  l = parse_and();
  while (!X.err && accept_word("or"))
    l = bin(E_OR, 0, l, parse_and());
  X.depth--;
  return l;
}
static struct xe *parse(const char *src)
{
  struct xe *e;
  X.p = src;
  e = parse_or();
  ws();
  if (!X.err && *X.p)
    fail("unexpected token");
  return X.err ? 0 : e;
}

/* ------------------------------------------------------- evaluation -------- */

static int is_element(const struct dom_node *n) { return n->type == DOM_ELEMENT && n->tag[0] != '#'; }

static int depth_of(const struct dom_node *n)
{
  int d = 0;
  while (n->parent)
    n = n->parent, d++;
  return d;
}
static int doc_order(xn a, xn b)
{
  struct dom_node *x = a.n, *y = b.n, *s;
  int da, db, d0;
  if (x == y) {
    struct dom_attr *at;
    if (a.a == b.a)
      return 0;
    if (!a.a)
      return -1;
    if (!b.a)
      return 1;
    for (at = x->attrs; at; at = at->next) {
      if (at == a.a)
        return -1;
      if (at == b.a)
        return 1;
    }
    return 0;
  }
  da = depth_of(x);
  db = depth_of(y);
  d0 = da - db;
  while (da > db)
    x = x->parent, da--;
  while (db > da)
    y = y->parent, db--;
  if (x == y)
    return d0 < 0 ? -1 : 1;
  while (x->parent != y->parent) {
    if (!x->parent || !y->parent)
      return x < y ? -1 : 1;
    x = x->parent;
    y = y->parent;
  }
  if (!x->parent)
    return x < y ? -1 : 1;
  for (s = x; s; s = s->next)
    if (s == y)
      return -1;
  return 1;
}
static int same_xn(xn a, xn b) { return a.n == b.n && a.a == b.a; }

static void sort_dedupe(xl *l)
{
  int sorted = 1, w = 0;
  for (int i = 1; i < l->n; i++)
    if (doc_order(l->v[i - 1], l->v[i]) > 0) {
      sorted = 0;
      break;
    }
  if (!sorted && l->n > 1) {
    xn *tmp = (xn *)palloc((unsigned)l->n * sizeof *tmp);
    if (!tmp)
      return;
    for (int width = 1; width < l->n; width *= 2) {
      for (int lo = 0; lo < l->n; lo += 2 * width) {
        int mid = lo + width < l->n ? lo + width : l->n;
        int hi = lo + 2 * width < l->n ? lo + 2 * width : l->n;
        int i = lo, j = mid, k = lo;
        while (i < mid && j < hi)
          tmp[k++] = doc_order(l->v[i], l->v[j]) <= 0 ? l->v[i++] : l->v[j++];
        while (i < mid)
          tmp[k++] = l->v[i++];
        while (j < hi)
          tmp[k++] = l->v[j++];
      }
      memcpy(l->v, tmp, (unsigned)l->n * sizeof *tmp);
    }
  }
  for (int i = 0; i < l->n; i++)
    if (!w || !same_xn(l->v[w - 1], l->v[i]))
      l->v[w++] = l->v[i];
  l->n = w;
}

static void text_of(const struct dom_node *n, sbuf *sb, int depth)
{
  if (n->type == DOM_TEXT) {
    if (n->text)
      sb_puts(sb, n->text);
    return;
  }
  if (depth > 256 || node_is_comment(n))
    return;
  for (const struct dom_node *c = n->first_child; c; c = c->next)
    text_of(c, sb, depth + 1);
}
static const char *string_of(xn x)
{
  sbuf sb = {0, 0, 0, 0};
  const char *r;
  if (x.a)
    return x.a->value ? x.a->value : "";
  if (node_is_comment(x.n)) {
    const char *d = dom_get_attr(x.n, "data");
    return d ? d : "";
  }
  text_of(x.n, &sb, 0);
  r = pstr(sb.b ? sb.b : "", sb.n);
  free(sb.b);
  return r;
}

static xv vnum(double d)
{
  xv v = {V_NUM, {0, 0, 0}, d, 0, 0};
  return v;
}
static xv vstr(const char *s)
{
  xv v = {V_STR, {0, 0, 0}, 0, s, 0};
  return v;
}
static xv vbool(int b)
{
  xv v = {V_BOOL, {0, 0, 0}, 0, 0, b != 0};
  return v;
}
static xv vnodes(xl l)
{
  xv v = {V_NODES, l, 0, 0, 0};
  return v;
}

static const char *num_to_str(double d)
{
  js_value s = js_to_string(g_ctx, js_number(d));
  return s.type == JV_STR ? pstr(s.u.str->data, (int)strlen(s.u.str->data)) : "";
}
static const char *to_str(xv v)
{
  switch (v.t) {
  case V_NODES:
    return v.ns.n ? string_of(v.ns.v[0]) : "";
  case V_NUM:
    return num_to_str(v.num);
  case V_BOOL:
    return v.b ? "true" : "false";
  }
  return v.str ? v.str : "";
}
static double to_num(xv v)
{
  const char *s;
  switch (v.t) {
  case V_NUM:
    return v.num;
  case V_BOOL:
    return v.b;
  }
  s = to_str(v);
  return str_to_num(s, (int)strlen(s));
}
static int to_bool(xv v)
{
  switch (v.t) {
  case V_NODES:
    return v.ns.n > 0;
  case V_NUM:
    return v.num == v.num && v.num != 0;
  case V_BOOL:
    return v.b;
  }
  return v.str && *v.str;
}

static int cmp_scalar(int op, xv a, xv b)
{
  if (op < 2) {
    int eq;
    if (a.t == V_BOOL || b.t == V_BOOL)
      eq = to_bool(a) == to_bool(b);
    else if (a.t == V_NUM || b.t == V_NUM)
      eq = to_num(a) == to_num(b);
    else
      eq = !strcmp(to_str(a), to_str(b));
    return op == 0 ? eq : !eq;
  }
  {
    double x = to_num(a), y = to_num(b);
    switch (op) {
    case 2:
      return x < y;
    case 3:
      return x <= y;
    case 4:
      return x > y;
    }
    return x >= y;
  }
}
static int compare(int op, xv a, xv b)
{
  if (a.t == V_NODES && b.t == V_NODES) {
    for (int i = 0; i < a.ns.n; i++)
      for (int j = 0; j < b.ns.n; j++)
        if (cmp_scalar(op, vstr(string_of(a.ns.v[i])), vstr(string_of(b.ns.v[j]))))
          return 1;
    return 0;
  }
  if (a.t == V_NODES || b.t == V_NODES) {
    int flip = b.t == V_NODES;
    xv set = flip ? b : a, other = flip ? a : b;
    if (other.t == V_BOOL)
      return flip ? cmp_scalar(op, other, vbool(to_bool(set))) : cmp_scalar(op, vbool(to_bool(set)), other);
    for (int i = 0; i < set.ns.n; i++) {
      xv s = vstr(string_of(set.ns.v[i]));
      if (flip ? cmp_scalar(op, other, s) : cmp_scalar(op, s, other))
        return 1;
    }
    return 0;
  }
  return cmp_scalar(op, a, b);
}

/* The namespace URI a prefix in the expression stands for, via the caller's resolver */
static const char *resolve_prefix(const char *prefix, int plen)
{
  js_value arg, res = js_undef(), fn;
  const char *u = 0;
  for (int i = 0; i < X.npfx; i++)
    if ((int)strlen(X.pfx[i].prefix) == plen && !ncmp(X.pfx[i].prefix, prefix, plen))
      return X.pfx[i].uri;
  if (X.resolver.type == JV_OBJ && plen < 32) {
    gc_push_scope();
    arg = js_mk_string(g_ctx, prefix, plen);
    if (arg.type == JV_STR)
      gc_keep(arg.u.str);
    fn = js_is_callable(X.resolver) ? X.resolver : js_get(g_ctx, X.resolver, "lookupNamespaceURI");
    if (js_is_callable(fn))
      res = js_call(g_ctx, fn, js_is_callable(X.resolver) ? js_undef() : X.resolver, &arg, 1);
    if (g_ctx->has_exc) {
      X.err_kind = ERR_THROWN;
      fail("the namespace resolver threw");
    } else if (res.type == JV_STR && *res.u.str->data)
      u = pstr(res.u.str->data, (int)strlen(res.u.str->data));
    gc_pop_scope();
  }
  if (!u) {
    if (!X.err)
      X.err_kind = ERR_NAMESPACE;
    fail("the prefix cannot be resolved to a namespace");
    return "";
  }
  if (X.npfx < 16) {
    memcpy(X.pfx[X.npfx].prefix, prefix, (unsigned)plen);
    X.pfx[X.npfx++].uri = u;
  }
  return u;
}
static int is_xmlns_attr(const struct dom_attr *a) { return !strcmp(a->name, "xmlns") || !ncmp(a->name, "xmlns:", 6); }

static int name_matches(xn x, const struct xstep *st, int attr_axis)
{
  const char *qname, *ns;
  int pl, tl;
  if (attr_axis != (x.a != 0) && st->test != T_NODE)
    return 0;
  if (x.a && is_xmlns_attr(x.a))
    return 0;
  switch (st->test) {
  case T_NODE:
    return 1;
  case T_TEXT:
    return !x.a && x.n->type == DOM_TEXT;
  case T_COMMENT:
    return !x.a && node_is_comment(x.n);
  case T_PI:
    return 0;
  }
  if (!x.a && !is_element(x.n))
    return 0;
  qname = x.a ? x.a->name : x.n->tag;
  ns = x.a ? dom_attr_ns(x.a) : dom_ns(x.n);
  tl = (int)strlen(st->name);
  pl = st->test == T_ANY ? (tl ? tl - 2 : 0) : dom_prefix_len(st->name);
  if (st->test == T_ANY && !tl)
    return 1;
  if (pl > 0 || (st->test == T_ANY && tl)) {
    const char *uri = resolve_prefix(st->name, pl);
    if (strcmp(ns, uri))
      return 0;
    return st->test == T_ANY || !strcmp(dom_local_name(qname), dom_local_name(st->name));
  }
  /* unprefixed: no namespace for attributes; HTML (or none) for elements */
  if (x.a ? *ns : strcmp(ns, DOM_NS_HTML) && *ns)
    return 0;
  return !strcmp(qname, st->name);
}

static void push_subtree(struct dom_node *root, xl *out, int include_root)
{
  struct dom_node *n = root;
  if (include_root)
    xl_push(out, mk(root));
  if (!n->first_child)
    return;
  n = n->first_child;
  for (;;) {
    xl_push(out, mk(n));
    if (n->first_child) {
      n = n->first_child;
      continue;
    }
    while (!n->next) {
      n = n->parent;
      if (n == root)
        return;
    }
    n = n->next;
  }
}
static void reverse_xl(xl *l)
{
  for (int i = 0, j = l->n - 1; i < j; i++, j--) {
    xn t = l->v[i];
    l->v[i] = l->v[j];
    l->v[j] = t;
  }
}

/* nodes on the axis from c, in proximity order (reverse document order for reverse axes) */
static void axis_nodes(xn c, int axis, xl *out)
{
  struct dom_node *n = c.n, *a, *s;
  if (c.a) {
    if (axis == AX_SELF || axis == AX_ANCESTOR_OR_SELF)
      xl_push(out, c);
    if (axis == AX_PARENT)
      xl_push(out, mk(n));
    if (axis == AX_ANCESTOR || axis == AX_ANCESTOR_OR_SELF)
      for (a = n; a; a = a->parent)
        xl_push(out, mk(a));
    return;
  }
  switch (axis) {
  case AX_CHILD:
    for (s = n->first_child; s; s = s->next)
      xl_push(out, mk(s));
    break;
  case AX_DESCENDANT:
    push_subtree(n, out, 0);
    break;
  case AX_DESCENDANT_OR_SELF:
    push_subtree(n, out, 1);
    break;
  case AX_PARENT:
    if (n->parent)
      xl_push(out, mk(n->parent));
    break;
  case AX_ANCESTOR:
  case AX_ANCESTOR_OR_SELF:
    for (a = axis == AX_ANCESTOR ? n->parent : n; a; a = a->parent)
      xl_push(out, mk(a));
    break;
  case AX_SELF:
    xl_push(out, c);
    break;
  case AX_ATTRIBUTE:
    if (is_element(n))
      for (struct dom_attr *at = n->attrs; at; at = at->next) {
        xn x = {n, at};
        if (!is_xmlns_attr(at))
          xl_push(out, x);
      }
    break;
  case AX_FOLLOWING_SIBLING:
    for (s = n->next; s; s = s->next)
      xl_push(out, mk(s));
    break;
  case AX_PRECEDING_SIBLING:
    if (n->parent)
      for (s = n->parent->first_child; s && s != n; s = s->next)
        xl_push(out, mk(s));
    reverse_xl(out);
    break;
  case AX_FOLLOWING:
    for (a = n; a; a = a->parent)
      for (s = a->next; s; s = s->next)
        push_subtree(s, out, 1);
    break;
  case AX_PRECEDING: {
    struct dom_node *top = n;
    xl all = {0, 0, 0};
    while (top->parent)
      top = top->parent;
    push_subtree(top, &all, 1);
    for (int i = 0; i < all.n && all.v[i].n != n; i++)
      if (!dom_is_ancestor(all.v[i].n, n))
        xl_push(out, all.v[i]);
    reverse_xl(out);
    break;
  }
  }
}

static xv eval(struct xe *e, xn c, int pos, int size);

static void filter_preds(xl *l, struct xe **preds, int np)
{
  for (int p = 0; p < np && !X.err; p++) {
    xl keep = {0, 0, 0};
    for (int i = 0; i < l->n; i++) {
      xv r = eval(preds[p], l->v[i], i + 1, l->n);
      int ok = r.t == V_NUM ? r.num == (double)(i + 1) : to_bool(r);
      if (ok)
        xl_push(&keep, l->v[i]);
    }
    *l = keep;
  }
}
static xl eval_step(xl in, const struct xstep *st)
{
  xl out = {0, 0, 0};
  int attr = st->axis == AX_ATTRIBUTE;
  for (int i = 0; i < in.n && !X.err; i++) {
    xl raw = {0, 0, 0}, hit = {0, 0, 0};
    axis_nodes(in.v[i], st->axis, &raw);
    for (int j = 0; j < raw.n; j++)
      if (name_matches(raw.v[j], st, attr))
        xl_push(&hit, raw.v[j]);
    filter_preds(&hit, st->preds, st->npreds);
    for (int j = 0; j < hit.n; j++)
      xl_push(&out, hit.v[j]);
  }
  if (in.n > 1 || st->axis == AX_ANCESTOR || st->axis == AX_ANCESTOR_OR_SELF || st->axis == AX_PRECEDING ||
      st->axis == AX_PRECEDING_SIBLING)
    sort_dedupe(&out);
  return out;
}

static xv eval_path(struct xe *e, xn c)
{
  xl cur = {0, 0, 0};
  if (e->filter) {
    xv f = eval(e->filter, c, 1, 1);
    if (f.t != V_NODES) {
      fail("expression is not a node-set");
      return vnodes(cur);
    }
    cur = f.ns;
    filter_preds(&cur, e->fpreds, e->nfpreds);
  } else if (e->abs) {
    struct dom_node *top = c.n;
    while (top->parent)
      top = top->parent;
    xl_push(&cur, mk(top));
  } else
    xl_push(&cur, c);
  for (int i = 0; i < e->nsteps && !X.err; i++)
    cur = eval_step(cur, &e->steps[i]);
  return vnodes(cur);
}

static struct dom_node *find_id(struct dom_node *root, const char *id, int n)
{
  xl all = {0, 0, 0};
  push_subtree(root, &all, 1);
  for (int i = 0; i < all.n; i++) {
    const char *v = is_element(all.v[i].n) ? dom_get_attr(all.v[i].n, "id") : 0;
    if (v && (int)strlen(v) == n && !ncmp(v, id, (unsigned)n))
      return all.v[i].n;
  }
  return 0;
}
static double xround(double d)
{
  if (d != d || d - d != 0)
    return d;
  return floor(d + 0.5);
}
static xn first_or(xv v, xn c) { return v.ns.n ? v.ns.v[0] : c; }
static const char *node_name(xn x) { return x.a ? x.a->name : (is_element(x.n) ? x.n->tag : ""); }

static xv call_function(struct xe *e, xn c, int pos, int size)
{
  const char *f = e->str;
  xv a[3] = {0};
  int n = e->nargs;
  for (int i = 0; i < n && i < 3; i++)
    a[i] = eval(e->args[i], c, pos, size);
  if (X.err)
    return vbool(0);
#define IS(s) (!strcmp(f, s))
#define NEED(lo, hi)                                                                                                   \
  if (n < (lo) || n > (hi)) {                                                                                          \
    fail("wrong number of arguments");                                                                                 \
    return vbool(0);                                                                                                   \
  }
  if (IS("last")) {
    NEED(0, 0)
    return vnum(size);
  }
  if (IS("position")) {
    NEED(0, 0)
    return vnum(pos);
  }
  if (IS("count")) {
    NEED(1, 1)
    if (a[0].t != V_NODES) {
      fail("count() needs a node-set");
      return vnum(0);
    }
    return vnum(a[0].ns.n);
  }
  if (IS("sum")) {
    double s = 0;
    NEED(1, 1)
    if (a[0].t != V_NODES) {
      fail("sum() needs a node-set");
      return vnum(0);
    }
    for (int i = 0; i < a[0].ns.n; i++) {
      const char *t = string_of(a[0].ns.v[i]);
      s += str_to_num(t, (int)strlen(t));
    }
    return vnum(s);
  }
  if (IS("id")) {
    xl out = {0, 0, 0};
    struct dom_node *top = c.n;
    const char *s;
    NEED(1, 1)
    while (top->parent)
      top = top->parent;
    s = to_str(a[0]);
    while (*s) {
      const char *w;
      struct dom_node *hit;
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
      w = s;
      while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
        s++;
      if (s > w && (hit = find_id(top, w, (int)(s - w))))
        xl_push(&out, mk(hit));
    }
    sort_dedupe(&out);
    return vnodes(out);
  }
  if (IS("local-name") || IS("name")) {
    NEED(0, 1)
    if (n && a[0].t != V_NODES) {
      fail("name() needs a node-set");
      return vstr("");
    }
    return vstr(IS("name") ? node_name(first_or(a[0], c)) : dom_local_name(node_name(first_or(a[0], c))));
  }
  if (IS("namespace-uri")) {
    xn x;
    NEED(0, 1)
    if (n && a[0].t != V_NODES) {
      fail("namespace-uri() needs a node-set");
      return vstr("");
    }
    x = first_or(a[0], c);
    return vstr(x.a ? dom_attr_ns(x.a) : (is_element(x.n) ? dom_ns(x.n) : ""));
  }
  if (IS("string")) {
    NEED(0, 1)
    if (!n) {
      xl one = {0, 0, 0};
      xl_push(&one, c);
      return vstr(to_str(vnodes(one)));
    }
    return vstr(to_str(a[0]));
  }
  if (IS("concat")) {
    sbuf sb = {0, 0, 0, 0};
    xv r;
    if (n < 2) {
      fail("wrong number of arguments");
      return vstr("");
    }
    for (int i = 0; i < n; i++) {
      xv v = i < 3 ? a[i] : eval(e->args[i], c, pos, size);
      sb_puts(&sb, to_str(v));
    }
    r = vstr(pstr(sb.b ? sb.b : "", sb.n));
    free(sb.b);
    return r;
  }
  if (IS("starts-with")) {
    const char *s, *p;
    NEED(2, 2)
    s = to_str(a[0]);
    p = to_str(a[1]);
    return vbool(!ncmp(s, p, strlen(p)));
  }
  if (IS("contains")) {
    NEED(2, 2)
    return vbool(xstr(to_str(a[0]), to_str(a[1])) != 0);
  }
  if (IS("substring-before") || IS("substring-after")) {
    const char *s, *p, *hit;
    NEED(2, 2)
    s = to_str(a[0]);
    p = to_str(a[1]);
    hit = xstr(s, p);
    if (!hit)
      return vstr("");
    if (IS("substring-before"))
      return vstr(pstr(s, (int)(hit - s)));
    return vstr(hit + strlen(p));
  }
  if (IS("substring")) {
    const char *s;
    int len, from = 0, to;
    double st, en;
    NEED(2, 3)
    s = to_str(a[0]);
    len = (int)strlen(s);
    st = xround(to_num(a[1]));
    en = n == 3 ? st + xround(to_num(a[2])) : 1e300;
    if (st != st || en != en)
      return vstr("");
    from = st < 1 ? 0 : (st > len ? len : (int)st - 1);
    to = en < 1 ? 0 : (en > len + 1 ? len : (int)en - 1);
    return vstr(to > from ? pstr(s + from, to - from) : "");
  }
  if (IS("string-length")) {
    NEED(0, 1)
    if (!n) {
      xl one = {0, 0, 0};
      xl_push(&one, c);
      return vnum((double)strlen(to_str(vnodes(one))));
    }
    return vnum((double)strlen(to_str(a[0])));
  }
  if (IS("normalize-space")) {
    sbuf sb = {0, 0, 0, 0};
    const char *s;
    xv r;
    int pending = 0;
    NEED(0, 1)
    if (!n) {
      xl one = {0, 0, 0};
      xl_push(&one, c);
      s = to_str(vnodes(one));
    } else
      s = to_str(a[0]);
    for (; *s; s++) {
      if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        pending = sb.n > 0;
        continue;
      }
      if (pending)
        sb_putc(&sb, ' ');
      pending = 0;
      sb_putc(&sb, *s);
    }
    r = vstr(pstr(sb.b ? sb.b : "", sb.n));
    free(sb.b);
    return r;
  }
  if (IS("translate")) {
    const char *s, *from, *to;
    char *out;
    int w = 0;
    NEED(3, 3)
    s = to_str(a[0]);
    from = to_str(a[1]);
    to = to_str(a[2]);
    out = (char *)palloc(strlen(s) + 1);
    if (!out)
      return vstr("");
    for (; *s; s++) {
      const char *hit = strchr(from, *s);
      if (!hit)
        out[w++] = *s;
      else if ((unsigned)(hit - from) < strlen(to))
        out[w++] = to[hit - from];
    }
    return vstr(out);
  }
  if (IS("boolean")) {
    NEED(1, 1)
    return vbool(to_bool(a[0]));
  }
  if (IS("not")) {
    NEED(1, 1)
    return vbool(!to_bool(a[0]));
  }
  if (IS("true")) {
    NEED(0, 0)
    return vbool(1);
  }
  if (IS("false") || IS("lang")) {
    return vbool(0);
  }
  if (IS("number")) {
    NEED(0, 1)
    if (!n) {
      xl one = {0, 0, 0};
      xl_push(&one, c);
      return vnum(to_num(vnodes(one)));
    }
    return vnum(to_num(a[0]));
  }
  if (IS("floor")) {
    NEED(1, 1)
    return vnum(floor(to_num(a[0])));
  }
  if (IS("ceiling")) {
    NEED(1, 1)
    return vnum(ceil(to_num(a[0])));
  }
  if (IS("round")) {
    NEED(1, 1)
    return vnum(xround(to_num(a[0])));
  }
#undef IS
#undef NEED
  fail("unknown function");
  return vbool(0);
}

static xv eval(struct xe *e, xn c, int pos, int size)
{
  xv l, r;
  if (X.err || !e)
    return vbool(0);
  if (++X.depth > EVAL_DEPTH_MAX) {
    X.depth--;
    fail("expression too deeply nested");
    return vbool(0);
  }
  switch (e->k) {
  case E_NUM:
    l = vnum(e->num);
    break;
  case E_STR:
    l = vstr(e->str);
    break;
  case E_PATH:
    l = eval_path(e, c);
    break;
  case E_FUNC:
    l = call_function(e, c, pos, size);
    break;
  case E_NEG:
    l = vnum(-to_num(eval(e->a, c, pos, size)));
    break;
  case E_OR:
    l = vbool(to_bool(eval(e->a, c, pos, size)) || to_bool(eval(e->b, c, pos, size)));
    break;
  case E_AND:
    l = vbool(to_bool(eval(e->a, c, pos, size)) && to_bool(eval(e->b, c, pos, size)));
    break;
  case E_UNION:
    l = eval(e->a, c, pos, size);
    r = eval(e->b, c, pos, size);
    if (l.t != V_NODES || r.t != V_NODES) {
      fail("union needs node-sets");
      l = vbool(0);
      break;
    }
    for (int i = 0; i < r.ns.n; i++)
      xl_push(&l.ns, r.ns.v[i]);
    sort_dedupe(&l.ns);
    break;
  case E_EQ:
  case E_REL:
    l = eval(e->a, c, pos, size);
    r = eval(e->b, c, pos, size);
    l = vbool(compare(e->op, l, r));
    break;
  default: {
    double x = to_num(eval(e->a, c, pos, size)), y = to_num(eval(e->b, c, pos, size));
    if (e->k == E_ADD)
      l = vnum(e->op ? x - y : x + y);
    else if (e->op == 0)
      l = vnum(x * y);
    else if (e->op == 1)
      l = vnum(x / y);
    else
      l = vnum(fmod(x, y));
  }
  }
  X.depth--;
  return l;
}

/* ------------------------------------------------------- JS surface -------- */

enum {
  R_ANY,
  R_NUMBER,
  R_STRING,
  R_BOOLEAN,
  R_UNORDERED_ITER,
  R_ORDERED_ITER,
  R_UNORDERED_SNAP,
  R_ORDERED_SNAP,
  R_ANY_UNORDERED,
  R_FIRST_ORDERED
};

static js_value res_iterate_next(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value nodes = js_get(ctx, t, "@nodes"), idx = js_get(ctx, t, "@i"), v;
  unsigned i = (unsigned)js_to_number(ctx, idx);
  (void)args;
  (void)argc;
  if (nodes.type != JV_OBJ || i >= js_array_len(nodes.u.obj))
    return js_null();
  v = js_get_index(ctx, nodes, i);
  js_set(ctx, t, "@i", js_number(i + 1));
  return v;
}
static js_value res_snapshot_item(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value nodes = js_get(ctx, t, "@nodes"), v;
  double i = js_to_number(ctx, A(0));
  if (nodes.type != JV_OBJ || !(i >= 0) || i >= js_array_len(nodes.u.obj))
    return js_null();
  v = js_get_index(ctx, nodes, (unsigned)i);
  return v.type == JV_UNDEF ? js_null() : v;
}

static js_value make_result(js_ctx *ctx, xv v, int type)
{
  js_object *res, *arr = 0;
  js_value rv;
  if (type == R_ANY) {
    type = v.t == V_NUM ? R_NUMBER : v.t == V_STR ? R_STRING : v.t == V_BOOL ? R_BOOLEAN : R_UNORDERED_ITER;
  }
  if (type >= R_UNORDERED_ITER && v.t != V_NODES)
    return js_throw_type(ctx, "the expression result is not a node-set");
  gc_push_scope();
  res = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  rv = js_obj_value(res);
  js_set(ctx, rv, "resultType", js_number(type));
  switch (type) {
  case R_NUMBER:
    js_set(ctx, rv, "numberValue", js_number(to_num(v)));
    break;
  case R_STRING:
    js_set(ctx, rv, "stringValue", js_mk_cstring(ctx, to_str(v)));
    break;
  case R_BOOLEAN:
    js_set(ctx, rv, "booleanValue", js_bool(to_bool(v)));
    break;
  default:
    arr = (js_object *)gc_keep(js_new_array(ctx, 0));
    for (int i = 0; i < v.ns.n; i++)
      if (!v.ns.v[i].a)
        js_array_push(ctx, arr, wrap_node(v.ns.v[i].n));
    js_set(ctx, rv, "@nodes", js_obj_value(arr));
    if (type == R_ANY_UNORDERED || type == R_FIRST_ORDERED) {
      js_value first = js_array_len(arr) ? js_get_index(ctx, js_obj_value(arr), 0) : js_null();
      js_set(ctx, rv, "singleNodeValue", first);
    } else if (type == R_UNORDERED_ITER || type == R_ORDERED_ITER) {
      js_set(ctx, rv, "@i", js_number(0));
      js_set(ctx, rv, "invalidIteratorState", js_bool(0));
      js_def_native(ctx, res, "iterateNext", res_iterate_next, 0);
    } else {
      js_set(ctx, rv, "snapshotLength", js_number(js_array_len(arr)));
      js_def_native(ctx, res, "snapshotItem", res_snapshot_item, 1);
    }
  }
  gc_pop_scope();
  return rv;
}

static js_value xpath_error(js_ctx *ctx, int kind, const char *msg)
{
  char buf[128];
  if (kind == ERR_THROWN)
    return js_undef();
  snprintf(buf, sizeof buf, kind == ERR_NAMESPACE ? "%s" : "invalid XPath expression: %s", msg);
  return js_throw_error(ctx, ctx->error_proto, kind == ERR_NAMESPACE ? "NamespaceError" : "SyntaxError", buf);
}
/* a resolver callback may evaluate again, so each evaluation keeps its own state */
static js_value evaluate_on(js_ctx *ctx, const char *expr, struct dom_node *ctxnode, js_value resolver,
                            js_value type_arg)
{
  struct xstate saved = X;
  struct xe *e;
  js_value out = js_undef();
  double ty = type_arg.type == JV_UNDEF ? 0 : js_to_number(ctx, type_arg);
  if (!(ty >= 0 && ty <= R_FIRST_ORDERED))
    return js_throw_type(ctx, "invalid XPathResult type");
  if (!ctxnode)
    return js_throw_type(ctx, "XPath evaluate: missing context node");
  memset(&X, 0, sizeof X);
  X.resolver = resolver;
  e = parse(expr);
  if (e) {
    xv v = eval(e, mk(ctxnode), 1, 1);
    if (!X.err)
      out = make_result(ctx, v, (int)ty);
  }
  if (X.err) {
    int kind = X.err_kind;
    char msg[96];
    strlcpy(msg, X.err, sizeof msg);
    pool_reset();
    X = saved;
    return xpath_error(ctx, kind, msg);
  }
  pool_reset();
  X = saved;
  return out;
}
static struct dom_node *context_arg(js_value v, js_value fallback)
{
  return v.type == JV_OBJ ? doc_root_of(v) : doc_root_of(fallback);
}

js_value doc_evaluate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return evaluate_on(ctx, arg_str(ctx, A(0)), context_arg(A(1), t), A(2), A(3));
}
static js_value xpe_evaluate(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value src = js_get(ctx, t, "@src");
  return evaluate_on(ctx, src.type == JV_STR ? src.u.str->data : "", context_arg(A(0), t), js_get(ctx, t, "@resolver"),
                     A(1));
}
js_value doc_create_expression(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  const char *expr = arg_str(ctx, A(0));
  struct xstate saved = X;
  js_object *o;
  js_value ov;
  (void)t;
  memset(&X, 0, sizeof X);
  parse(expr);
  if (X.err) {
    char msg[96];
    strlcpy(msg, X.err, sizeof msg);
    pool_reset();
    X = saved;
    return xpath_error(ctx, ERR_SYNTAX, msg);
  }
  pool_reset();
  X = saved;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  ov = js_obj_value(o);
  js_set(ctx, ov, "@src", js_mk_cstring(ctx, expr));
  js_set(ctx, ov, "@resolver", A(1));
  js_def_native(ctx, o, "evaluate", xpe_evaluate, 3);
  gc_pop_scope();
  return ov;
}
js_value doc_create_ns_resolver(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  return A(0);
}
static js_value xpeval_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  js_object *o;
  (void)thisv;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  js_def_native(ctx, o, "evaluate", doc_evaluate, 5);
  js_def_native(ctx, o, "createExpression", doc_create_expression, 2);
  js_def_native(ctx, o, "createNSResolver", doc_create_ns_resolver, 1);
  gc_pop_scope();
  return js_obj_value(o);
}
static js_value xpresult_ctor(js_ctx *ctx, js_value thisv, js_value *args, int argc)
{
  (void)thisv;
  (void)args;
  (void)argc;
  return js_throw_type(ctx, "Illegal constructor");
}

void xpath_install(js_ctx *ctx, js_object *win)
{
  static const struct {
    const char *name;
    int v;
  } consts[] = {{"ANY_TYPE", R_ANY},
                {"NUMBER_TYPE", R_NUMBER},
                {"STRING_TYPE", R_STRING},
                {"BOOLEAN_TYPE", R_BOOLEAN},
                {"UNORDERED_NODE_ITERATOR_TYPE", R_UNORDERED_ITER},
                {"ORDERED_NODE_ITERATOR_TYPE", R_ORDERED_ITER},
                {"UNORDERED_NODE_SNAPSHOT_TYPE", R_UNORDERED_SNAP},
                {"ORDERED_NODE_SNAPSHOT_TYPE", R_ORDERED_SNAP},
                {"ANY_UNORDERED_NODE_TYPE", R_ANY_UNORDERED},
                {"FIRST_ORDERED_NODE_TYPE", R_FIRST_ORDERED}};
  js_object *rc = js_new_native(ctx, xpresult_ctor, "XPathResult", 0);
  gc_push_scope();
  gc_keep(rc);
  for (unsigned i = 0; i < sizeof consts / sizeof consts[0]; i++)
    js_def(rc, consts[i].name, js_number(consts[i].v), 0);
  js_def(win, "XPathResult", js_obj_value(rc), P_WRITABLE | P_CONFIGURABLE);
  js_def_native(ctx, win, "XPathEvaluator", xpeval_ctor, 0);
  def_method(g_document_proto, "evaluate", doc_evaluate, 5);
  def_method(g_document_proto, "createExpression", doc_create_expression, 2);
  def_method(g_document_proto, "createNSResolver", doc_create_ns_resolver, 1);
  gc_pop_scope();
}
