/* xmltree.c: see xmltree.h. */

#include "xmltree.h"
#include "owos.h"

#define NS_XML "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"
#define MAX_DEPTH 256
#define OOM "out of memory"

struct buf {
  char *p;
  int len, cap;
};

static int b_add(struct buf *b, const char *s, int n)
{
  if (b->len + n + 1 > b->cap) {
    int cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1)
      cap *= 2;
    char *p = realloc(b->p, (unsigned)cap);
    if (!p)
      return -1;
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->len, s, (unsigned)n);
  b->len += n;
  b->p[b->len] = 0;
  return 0;
}

static char *b_take(struct buf *b)
{
  char *p = b->p ? b->p : strdup("");
  memset(b, 0, sizeof *b);
  return p;
}

static char *dupn(const char *s, int n)
{
  char *p = malloc((unsigned)n + 1);
  if (p) {
    memcpy(p, s, (unsigned)n);
    p[n] = 0;
  }
  return p;
}

static const char *find(const char *p, const char *end, const char *lit)
{
  int n = strlen(lit);
  for (; end - p >= n; p++)
    if (memcmp(p, lit, (unsigned)n) == 0)
      return p;
  return 0;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int name_start(unsigned char c)
{
  return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_' || c == ':' || c >= 0x80;
}

static int name_char(unsigned char c) { return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

static int valid_char(unsigned long cp)
{
  return cp == 9 || cp == 10 || cp == 13 || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

/* One name with at most one colon, neither first nor last. */
static int valid_qname(const char *s, int n)
{
  if (n <= 0 || !name_start((unsigned char)s[0]))
    return 0;
  int colons = 0;
  for (int i = 0; i < n; i++) {
    if (!name_char((unsigned char)s[i]))
      return 0;
    if (s[i] == ':' && (++colons > 1 || i == 0 || i == n - 1))
      return 0;
  }
  return 1;
}

/* ---- tree ---- */

static struct xml_node *alloc_node(enum xml_type t)
{
  struct xml_node *n = calloc(1, sizeof *n);
  if (n)
    n->type = t;
  return n;
}

static const char *local_of(const char *q)
{
  const char *c = strchr(q, ':');
  return c ? c + 1 : q;
}

static void unlink_node(struct xml_node *n)
{
  if (!n->parent)
    return;
  if (n->prev)
    n->prev->next = n->next;
  else
    n->parent->first = n->next;
  if (n->next)
    n->next->prev = n->prev;
  else
    n->parent->last = n->prev;
  n->parent = n->prev = n->next = 0;
}

static void link_child(struct xml_node *parent, struct xml_node *child)
{
  child->parent = parent;
  child->prev = parent->last;
  child->next = 0;
  if (parent->last)
    parent->last->next = child;
  else
    parent->first = child;
  parent->last = child;
}

static void free_one(struct xml_node *n)
{
  while (n->attrs) {
    struct xml_attr *a = n->attrs;
    n->attrs = a->next;
    free(a->name);
    free(a->value);
    free(a->ns);
    free(a);
  }
  free(n->name);
  free(n->ns);
  free(n->value);
  free(n);
}

void xml_free(struct xml_node *n)
{
  if (!n)
    return;
  unlink_node(n);
  for (struct xml_node *c = n; c;) {
    while (c->first)
      c = c->first;
    struct xml_node *par = c == n ? 0 : c->parent;
    if (par)
      par->first = c->next;
    struct xml_node *next = par ? (par->first ? par->first : par) : 0;
    free_one(c);
    c = next;
  }
}

struct xml_node *xml_new_element(const char *qname, const char *ns)
{
  if (!valid_qname(qname, strlen(qname)))
    return 0;
  struct xml_node *n = alloc_node(XML_ELEM);
  if (!n)
    return 0;
  n->name = strdup(qname);
  n->ns = strdup(ns ? ns : "");
  if (!n->name || !n->ns) {
    free_one(n);
    return 0;
  }
  n->local = local_of(n->name);
  return n;
}

struct xml_node *xml_new(enum xml_type type, const char *name, const char *value)
{
  if (type == XML_ELEM || (type == XML_PI && !name))
    return 0;
  struct xml_node *n = alloc_node(type);
  if (!n)
    return 0;
  if (type == XML_PI)
    n->name = strdup(name);
  if (type != XML_DOC)
    n->value = strdup(value ? value : "");
  if ((type == XML_PI && !n->name) || (type != XML_DOC && !n->value)) {
    free_one(n);
    return 0;
  }
  return n;
}

int xml_append(struct xml_node *parent, struct xml_node *child)
{
  int owned = !child->parent, ok = owned;
  for (struct xml_node *a = parent; a; a = a->parent)
    if (a == child)
      owned = ok = 0;
  ok = ok && (parent->type == XML_DOC || parent->type == XML_ELEM) && child->type != XML_DOC;
  if (ok && parent->type == XML_DOC) {
    ok = child->type != XML_TEXT && child->type != XML_CDATA;
    for (struct xml_node *c = parent->first; ok && child->type == XML_ELEM && c; c = c->next)
      ok = c->type != XML_ELEM;
  }
  if (!ok) {
    if (owned)
      xml_free(child);
    return -1;
  }
  link_child(parent, child);
  return 0;
}

static int is_decl(const char *name)
{
  int n = strlen(name);
  return n == 5 ? memcmp(name, "xmlns", 5) == 0 : n > 6 && memcmp(name, "xmlns:", 6) == 0;
}

static struct xml_attr *attr_get(const struct xml_node *el, const char *qname)
{
  for (struct xml_attr *a = el->attrs; a; a = a->next)
    if (!strcmp(a->name, qname))
      return a;
  return 0;
}

int xml_set_attr(struct xml_node *el, const char *qname, const char *ns, const char *value)
{
  if (el->type != XML_ELEM || !valid_qname(qname, strlen(qname)))
    return -1;
  char *v = strdup(value);
  char *u = strdup(is_decl(qname) ? NS_XMLNS : ns ? ns : "");
  struct xml_attr *a = attr_get(el, qname);
  if (!v || !u) {
    free(v);
    free(u);
    return -1;
  }
  if (a) {
    free(a->value);
    free(a->ns);
  } else {
    a = calloc(1, sizeof *a);
    if (a)
      a->name = strdup(qname);
    if (!a || !a->name) {
      free(a);
      free(v);
      free(u);
      return -1;
    }
    struct xml_attr **tail = &el->attrs;
    while (*tail)
      tail = &(*tail)->next;
    *tail = a;
  }
  a->value = v;
  a->ns = u;
  return 0;
}

const char *xml_attr(const struct xml_node *el, const char *qname)
{
  struct xml_attr *a = attr_get(el, qname);
  return a ? a->value : 0;
}

const char *xml_attr_ns(const struct xml_node *el, const char *ns, const char *local)
{
  for (struct xml_attr *a = el->attrs; a; a = a->next)
    if (!strcmp(local_of(a->name), local) && !strcmp(a->ns, ns))
      return a->value;
  return 0;
}

static int match(const struct xml_node *n, const char *ns, const char *local)
{
  return n->type == XML_ELEM && !strcmp(n->local, local) && (!ns || !strcmp(n->ns, ns));
}

struct xml_node *xml_find(struct xml_node *parent, const char *ns, const char *local)
{
  for (struct xml_node *c = parent->first; c; c = c->next)
    if (match(c, ns, local))
      return c;
  return 0;
}

struct xml_node *xml_find_next(struct xml_node *after, const char *ns, const char *local)
{
  for (struct xml_node *c = after->next; c; c = c->next)
    if (match(c, ns, local))
      return c;
  return 0;
}

char *xml_text_content(const struct xml_node *n)
{
  struct buf b = {0};
  const struct xml_node *c = n->first;
  if (n->type == XML_TEXT || n->type == XML_CDATA)
    c = n;
  while (c) {
    if ((c->type == XML_TEXT || c->type == XML_CDATA) && b_add(&b, c->value, strlen(c->value))) {
      free(b.p);
      return 0;
    }
    if (c->first)
      c = c->first;
    else {
      while (c != n && !c->next)
        c = c->parent;
      c = c == n ? 0 : c->next;
    }
  }
  if (!b.p && b_add(&b, "", 0))
    return 0;
  return b.p;
}

/* ---- decoder ---- */

struct ps {
  const char *p, *end;
  struct xml_node *doc, *cur;
  int depth, root, doctype;
  const char *msg;
};

static int fail(struct ps *ps, const char *msg)
{
  ps->msg = msg;
  return -1;
}

static int put(struct ps *ps, struct buf *b, const char *s, int n) { return b_add(b, s, n) ? fail(ps, OOM) : 0; }

static int lit(const struct ps *ps, const char *s)
{
  int n = strlen(s);
  return ps->end - ps->p >= n && memcmp(ps->p, s, (unsigned)n) == 0;
}

static int skip_ws(struct ps *ps)
{
  const char *s = ps->p;
  while (ps->p < ps->end && is_ws(*ps->p))
    ps->p++;
  return ps->p != s;
}

static int read_name(struct ps *ps, const char **s, int *n)
{
  const char *q = ps->p;
  if (q >= ps->end || !name_start((unsigned char)*q))
    return fail(ps, "expected a name");
  while (q < ps->end && name_char((unsigned char)*q))
    q++;
  *s = ps->p;
  *n = (int)(q - ps->p);
  if (!valid_qname(*s, *n))
    return fail(ps, "malformed qualified name");
  ps->p = q;
  return 0;
}

static int put_utf8(struct ps *ps, struct buf *b, unsigned long cp)
{
  char u[4];
  int n = 0;
  if (cp < 0x80)
    u[n++] = (char)cp;
  else if (cp < 0x800) {
    u[n++] = (char)(0xC0 | cp >> 6);
    u[n++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    u[n++] = (char)(0xE0 | cp >> 12);
    u[n++] = (char)(0x80 | (cp >> 6 & 0x3F));
    u[n++] = (char)(0x80 | (cp & 0x3F));
  } else {
    u[n++] = (char)(0xF0 | cp >> 18);
    u[n++] = (char)(0x80 | (cp >> 12 & 0x3F));
    u[n++] = (char)(0x80 | (cp >> 6 & 0x3F));
    u[n++] = (char)(0x80 | (cp & 0x3F));
  }
  return put(ps, b, u, n);
}

/* Decode the reference at ps->p ('&') into b. */
static int ref(struct ps *ps, struct buf *b)
{
  static const struct {
    const char *name;
    char ch;
  } ents[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
  const char *q = ps->p + 1;
  if (q < ps->end && *q == '#') {
    int hex = ++q < ps->end && *q == 'x', digits = 0;
    unsigned long cp = 0;
    q += hex;
    for (; q < ps->end && *q != ';'; q++, digits++) {
      int d = *q >= '0' && *q <= '9'                        ? *q - '0'
              : hex && (*q | 32) >= 'a' && (*q | 32) <= 'f' ? (*q | 32) - 'a' + 10
                                                            : -1;
      if (d < 0 || (cp = cp * (hex ? 16 : 10) + (unsigned long)d) > 0x10FFFF)
        return fail(ps, "bad character reference");
    }
    if (q >= ps->end || !digits || !valid_char(cp))
      return fail(ps, "bad character reference");
    ps->p = q + 1;
    return put_utf8(ps, b, cp);
  }
  const char *semi = q;
  while (semi < ps->end && semi - q < 8 && *semi != ';')
    semi++;
  for (unsigned i = 0; semi < ps->end && *semi == ';' && i < sizeof ents / sizeof ents[0]; i++)
    if ((int)strlen(ents[i].name) == semi - q && memcmp(q, ents[i].name, (unsigned)(semi - q)) == 0) {
      ps->p = semi + 1;
      return put(ps, b, &ents[i].ch, 1);
    }
  return fail(ps, "undefined entity");
}

/* Decode character data up to the unescaped `stop` ('<' for content, a quote for an attribute value). */
static int text_until(struct ps *ps, char stop, struct buf *b)
{
  while (ps->p < ps->end && *ps->p != stop) {
    unsigned char c = (unsigned char)*ps->p;
    if (c == '&') {
      if (ref(ps, b))
        return -1;
      continue;
    }
    if (c == '<')
      return fail(ps, "'<' in an attribute value");
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
      return fail(ps, "illegal character");
    if (c == ']' && stop == '<' && lit(ps, "]]>"))
      return fail(ps, "']]>' in character data");
    char out = (char)c;
    if (c == '\r') {
      if (ps->p + 1 < ps->end && ps->p[1] == '\n')
        ps->p++;
      out = '\n';
    }
    if (stop != '<' && (out == '\n' || out == '\t'))
      out = ' ';
    if (put(ps, b, &out, 1))
      return -1;
    ps->p++;
  }
  return ps->p < ps->end || stop == '<' ? 0 : fail(ps, "unexpected end of document");
}

/* Copy s..e verbatim (line ends normalized) into b, for comments, CDATA and PI data. */
static int raw_text(struct ps *ps, const char *s, const char *e, struct buf *b)
{
  for (; s < e; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      ps->p = s;
      return fail(ps, "illegal character");
    }
    if (c == '\r') {
      if (s + 1 < e && s[1] == '\n')
        s++;
      c = '\n';
    }
    if (put(ps, b, (const char *)&c, 1))
      return -1;
  }
  return 0;
}

static int add_leaf(struct ps *ps, enum xml_type t, char *name, struct buf *b)
{
  struct xml_node *n = alloc_node(t);
  char *v = b_take(b);
  if (!n || !v) {
    free(n);
    free(v);
    free(name);
    return fail(ps, OOM);
  }
  n->name = name;
  n->value = v;
  link_child(ps->cur, n);
  return 0;
}

static const char *ns_lookup(const struct xml_node *n, const char *pfx, int plen)
{
  if (plen == 3 && memcmp(pfx, "xml", 3) == 0)
    return NS_XML;
  for (; n; n = n->parent)
    for (struct xml_attr *a = n->attrs; a; a = a->next)
      if (plen ? (int)strlen(a->name) == 6 + plen && memcmp(a->name, "xmlns:", 6) == 0 &&
                     memcmp(a->name + 6, pfx, (unsigned)plen) == 0
               : !strcmp(a->name, "xmlns"))
        return a->value;
  return plen ? 0 : "";
}

/* Namespace URI of qname as seen from scope; NULL if its prefix is unbound. */
static const char *ns_of(const struct xml_node *scope, const char *qname, int is_attr)
{
  const char *c = strchr(qname, ':');
  if (!c)
    return is_attr ? "" : ns_lookup(scope, "", 0);
  return ns_lookup(scope, qname, (int)(c - qname));
}

static int resolve(struct ps *ps, struct xml_node *el)
{
  const char *c = strchr(el->name, ':');
  if (c && c - el->name == 5 && memcmp(el->name, "xmlns", 5) == 0)
    return fail(ps, "reserved prefix xmlns");
  const char *uri = ns_of(el, el->name, 0);
  if (!uri)
    return fail(ps, "unbound namespace prefix");
  if (!(el->ns = strdup(uri)))
    return fail(ps, OOM);
  for (struct xml_attr *a = el->attrs; a; a = a->next) {
    if (is_decl(a->name)) {
      if (strchr(a->name, ':') && !*a->value)
        return fail(ps, "empty namespace declaration");
      uri = NS_XMLNS;
    } else if (!(uri = ns_of(el, a->name, 1)))
      return fail(ps, "unbound namespace prefix");
    if (!(a->ns = strdup(uri)))
      return fail(ps, OOM);
  }
  return 0;
}

static int start_tag(struct ps *ps)
{
  const char *nm;
  int nl, empty = 0;
  ps->p++;
  if (read_name(ps, &nm, &nl))
    return -1;
  struct xml_node *el = alloc_node(XML_ELEM);
  if (!el)
    return fail(ps, OOM);
  link_child(ps->cur, el);
  if (!(el->name = dupn(nm, nl)))
    return fail(ps, OOM);
  el->local = local_of(el->name);
  for (;;) {
    int ws = skip_ws(ps);
    if (ps->p >= ps->end)
      return fail(ps, "unexpected end of document");
    if (*ps->p == '>') {
      ps->p++;
      break;
    }
    if (lit(ps, "/>")) {
      ps->p += 2;
      empty = 1;
      break;
    }
    if (!ws)
      return fail(ps, "expected whitespace before attribute");
    if (read_name(ps, &nm, &nl))
      return -1;
    struct xml_attr *a = calloc(1, sizeof *a);
    if (!a)
      return fail(ps, OOM);
    a->name = dupn(nm, nl);
    struct xml_attr **tail = &el->attrs;
    while (*tail)
      tail = &(*tail)->next;
    *tail = a;
    if (!a->name)
      return fail(ps, OOM);
    for (struct xml_attr *o = el->attrs; o != a; o = o->next)
      if (!strcmp(o->name, a->name))
        return fail(ps, "duplicate attribute");
    skip_ws(ps);
    if (ps->p >= ps->end || *ps->p != '=')
      return fail(ps, "expected '='");
    ps->p++;
    skip_ws(ps);
    if (ps->p >= ps->end || (*ps->p != '"' && *ps->p != '\''))
      return fail(ps, "expected a quoted attribute value");
    char q = *ps->p++;
    struct buf b = {0};
    if (text_until(ps, q, &b)) {
      free(b.p);
      return -1;
    }
    ps->p++;
    if (!(a->value = b_take(&b)))
      return fail(ps, OOM);
  }
  if (resolve(ps, el))
    return -1;
  if (!empty) {
    if (ps->depth >= MAX_DEPTH)
      return fail(ps, "nesting too deep");
    ps->cur = el;
    ps->depth++;
  }
  return 0;
}

static int end_tag(struct ps *ps)
{
  const char *nm;
  int nl;
  ps->p += 2;
  if (read_name(ps, &nm, &nl))
    return -1;
  skip_ws(ps);
  if (ps->p >= ps->end || *ps->p != '>')
    return fail(ps, "expected '>'");
  ps->p++;
  if (ps->cur->type != XML_ELEM)
    return fail(ps, "unexpected end tag");
  if ((int)strlen(ps->cur->name) != nl || memcmp(ps->cur->name, nm, (unsigned)nl) != 0)
    return fail(ps, "mismatched end tag");
  ps->cur = ps->cur->parent;
  ps->depth--;
  return 0;
}

/* Comment, CDATA section or PI at ps->p; body runs to the terminator `close`. */
static int markup(struct ps *ps, enum xml_type t, int open, const char *close)
{
  struct buf b = {0};
  char *target = 0;
  const char *start, *e;
  ps->p += open;
  if (t == XML_PI) {
    const char *nm;
    int nl;
    if (read_name(ps, &nm, &nl))
      return -1;
    if (nl == 3 && (nm[0] | 32) == 'x' && (nm[1] | 32) == 'm' && (nm[2] | 32) == 'l')
      return fail(ps, "reserved processing instruction target");
    if (!(target = dupn(nm, nl)))
      return fail(ps, OOM);
    if (!lit(ps, "?>") && !skip_ws(ps)) {
      free(target);
      return fail(ps, "expected whitespace after the PI target");
    }
  }
  start = ps->p;
  e = find(start, ps->end, close);
  if (!e) {
    free(target);
    return fail(ps, "unterminated markup");
  }
  if (t == XML_COMMENT && (e > start && e[-1] == '-' ? 1 : find(start, e, "--") != 0)) {
    free(target);
    return fail(ps, "'--' in a comment");
  }
  if (raw_text(ps, start, e, &b)) {
    free(b.p);
    free(target);
    return -1;
  }
  ps->p = e + strlen(close);
  return add_leaf(ps, t, target, &b);
}

static int doctype(struct ps *ps)
{
  int sub = 0;
  char q = 0;
  for (const char *p = ps->p + 9; p < ps->end; p++) {
    if (q) {
      q = *p == q ? 0 : q;
    } else if (*p == '"' || *p == '\'') {
      q = *p;
    } else if (!sub && *p == '[') {
      sub = 1;
    } else if (sub && *p == ']') {
      sub = 0;
    } else if (sub && ps->end - p >= 8 && memcmp(p, "<!ENTITY", 8) == 0) {
      ps->p = p;
      return fail(ps, "entity declarations are not supported");
    } else if (sub && (ps->end - p >= 4 && memcmp(p, "<!--", 4) == 0)) {
      const char *e = find(p + 4, ps->end, "-->");
      if (!e)
        break;
      p = e + 2;
    } else if (!sub && *p == '>') {
      ps->p = p + 1;
      return 0;
    }
  }
  return fail(ps, "unterminated DOCTYPE");
}

static const char *decl_val(const char *s, const char *e, const char *key, int *len)
{
  const char *k = find(s, e, key);
  if (!k)
    return 0;
  for (k += strlen(key); k < e && is_ws(*k); k++)
    ;
  if (k >= e || *k != '=')
    return 0;
  for (k++; k < e && is_ws(*k); k++)
    ;
  if (k >= e || (*k != '"' && *k != '\''))
    return 0;
  char q = *k++;
  const char *v = k;
  while (k < e && *k != q)
    k++;
  if (k >= e)
    return 0;
  *len = (int)(k - v);
  return v;
}

static int ieq(const char *a, int n, const char *b)
{
  if ((int)strlen(b) != n)
    return 0;
  for (int i = 0; i < n; i++)
    if ((a[i] | 32) != (b[i] | 32))
      return 0;
  return 1;
}

static int xml_decl(struct ps *ps)
{
  const char *e = find(ps->p, ps->end, "?>"), *v;
  int n;
  if (!e)
    return fail(ps, "unterminated XML declaration");
  if (!(v = decl_val(ps->p, e, "version", &n)) || n != 3 || memcmp(v, "1.0", 3) != 0)
    return fail(ps, "unsupported XML version");
  if ((v = decl_val(ps->p, e, "encoding", &n)) && !ieq(v, n, "utf-8") && !ieq(v, n, "us-ascii"))
    return fail(ps, "unsupported encoding");
  ps->p = e + 2;
  return 0;
}

static int run(struct ps *ps)
{
  if (lit(ps, "\xEF\xBB\xBF"))
    ps->p += 3;
  if (lit(ps, "<?xml") && ps->end - ps->p > 5 && is_ws(ps->p[5]) && xml_decl(ps))
    return -1;
  while (ps->p < ps->end) {
    int rc;
    if (*ps->p != '<') {
      if (ps->cur == ps->doc) {
        if (!skip_ws(ps))
          return fail(ps, "text outside the root element");
        continue;
      }
      struct buf b = {0};
      if (text_until(ps, '<', &b) || add_leaf(ps, XML_TEXT, 0, &b)) {
        free(b.p);
        return -1;
      }
      continue;
    }
    if (lit(ps, "</"))
      rc = end_tag(ps);
    else if (lit(ps, "<!--"))
      rc = markup(ps, XML_COMMENT, 4, "-->");
    else if (lit(ps, "<![CDATA["))
      rc = ps->cur == ps->doc ? fail(ps, "CDATA outside the root element") : markup(ps, XML_CDATA, 9, "]]>");
    else if (lit(ps, "<?"))
      rc = markup(ps, XML_PI, 2, "?>");
    else if (lit(ps, "<!DOCTYPE"))
      rc = ps->cur != ps->doc || ps->root || ps->doctype ? fail(ps, "misplaced DOCTYPE")
                                                         : (ps->doctype = 1, doctype(ps));
    else if (lit(ps, "<!"))
      rc = fail(ps, "unsupported markup declaration");
    else if (ps->cur == ps->doc && ps->root)
      rc = fail(ps, "more than one root element");
    else {
      ps->root = 1;
      rc = start_tag(ps);
    }
    if (rc)
      return -1;
  }
  if (ps->cur != ps->doc)
    return fail(ps, "unclosed element");
  return ps->root ? 0 : fail(ps, "no root element");
}

struct xml_node *xml_parse(const char *buf, int len, struct xml_error *err)
{
  struct ps ps = {buf, buf + len, 0, 0, 0, 0, 0, 0};
  ps.doc = ps.cur = alloc_node(XML_DOC);
  if (!ps.doc)
    ps.msg = OOM;
  else if (run(&ps) == 0)
    return ps.doc;
  if (err) {
    err->pos = (int)(ps.p - buf);
    err->line = err->col = 1;
    for (const char *c = buf; c < ps.p; c++) {
      err->col++;
      if (*c == '\n') {
        err->line++;
        err->col = 1;
      }
    }
    int i = 0;
    for (; ps.msg[i] && i < (int)sizeof err->msg - 1; i++)
      err->msg[i] = ps.msg[i];
    err->msg[i] = 0;
  }
  xml_free(ps.doc);
  return 0;
}

/* ---- encoder ---- */

static int esc(struct buf *b, const char *s, int attr)
{
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    const char *r = c == '&'            ? "&amp;"
                    : c == '<'          ? "&lt;"
                    : c == '>'          ? "&gt;"
                    : attr && c == '"'  ? "&quot;"
                    : c == '\r'         ? "&#13;"
                    : attr && c == '\n' ? "&#10;"
                    : attr && c == '\t' ? "&#9;"
                                        : 0;
    if (!r && c < 0x20 && c != '\t' && c != '\n')
      return -1;
    if (b_add(b, r ? r : s, r ? (int)strlen(r) : 1))
      return -1;
  }
  return 0;
}

static int valid_chars(const char *s)
{
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 && *s != '\t' && *s != '\n' && *s != '\r')
      return 0;
  return 1;
}

static int lead(struct buf *b, int level)
{
  if (level < 0 || !b->len)
    return 0;
  if (b_add(b, "\n", 1))
    return -1;
  for (int i = 0; i < level; i++)
    if (b_add(b, "  ", 2))
      return -1;
  return 0;
}

static int cdata(struct buf *b, const char *s)
{
  if (!valid_chars(s) || b_add(b, "<![CDATA[", 9))
    return -1;
  for (const char *e; (e = find(s, s + strlen(s), "]]>")); s = e + 2)
    if (b_add(b, s, (int)(e - s) + 2) || b_add(b, "]]><![CDATA[", 12))
      return -1;
  return b_add(b, s, strlen(s)) || b_add(b, "]]>", 3);
}

static int emit(struct buf *b, const struct xml_node *n, int level, int depth)
{
  if (depth > MAX_DEPTH)
    return -1;
  switch (n->type) {
  case XML_DOC:
    for (const struct xml_node *c = n->first; c; c = c->next)
      if (emit(b, c, level, depth + 1))
        return -1;
    return 0;
  case XML_TEXT:
    return esc(b, n->value, 0);
  case XML_CDATA:
    return cdata(b, n->value);
  case XML_COMMENT: {
    int len = strlen(n->value);
    if (!valid_chars(n->value) || find(n->value, n->value + len, "--") || (len && n->value[len - 1] == '-'))
      return -1;
    return lead(b, level) || b_add(b, "<!--", 4) || b_add(b, n->value, len) || b_add(b, "-->", 3);
  }
  case XML_PI:
    if (!valid_qname(n->name, strlen(n->name)) || strchr(n->name, ':') || ieq(n->name, strlen(n->name), "xml") ||
        !valid_chars(n->value) || find(n->value, n->value + strlen(n->value), "?>"))
      return -1;
    return lead(b, level) || b_add(b, "<?", 2) || b_add(b, n->name, strlen(n->name)) ||
           (*n->value && (b_add(b, " ", 1) || b_add(b, n->value, strlen(n->value)))) || b_add(b, "?>", 2);
  case XML_ELEM: {
    int mixed = 0;
    if (!valid_qname(n->name, strlen(n->name)) || lead(b, level) || b_add(b, "<", 1) ||
        b_add(b, n->name, strlen(n->name)))
      return -1;
    for (const struct xml_attr *a = n->attrs; a; a = a->next)
      if (!valid_qname(a->name, strlen(a->name)) || b_add(b, " ", 1) || b_add(b, a->name, strlen(a->name)) ||
          b_add(b, "=\"", 2) || esc(b, a->value, 1) || b_add(b, "\"", 1))
        return -1;
    if (!n->first)
      return b_add(b, "/>", 2);
    if (b_add(b, ">", 1))
      return -1;
    for (const struct xml_node *c = n->first; c; c = c->next)
      mixed |= c->type == XML_TEXT || c->type == XML_CDATA;
    int cl = mixed || level < 0 ? -1 : level + 1;
    for (const struct xml_node *c = n->first; c; c = c->next)
      if (emit(b, c, cl, depth + 1))
        return -1;
    if (cl >= 0 && lead(b, level))
      return -1;
    return b_add(b, "</", 2) || b_add(b, n->name, strlen(n->name)) || b_add(b, ">", 1);
  }
  }
  return -1;
}

char *xml_write(const struct xml_node *n, int flags, int *out_len)
{
  static const char decl[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  struct buf b = {0};
  int level = flags & XML_PRETTY ? 0 : -1;
  if (((flags & XML_DECL) && b_add(&b, decl, sizeof decl - 1)) || emit(&b, n, level, 0) ||
      (level >= 0 && b.len && b_add(&b, "\n", 1)) || (!b.p && b_add(&b, "", 0))) {
    free(b.p);
    return 0;
  }
  if (out_len)
    *out_len = b.len;
  return b.p;
}
