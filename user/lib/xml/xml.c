/* xml.c: see xml.h. */

#include "xml.h"
#include "owos.h"

#define MAXDEPTH 32

enum { T_OPEN, T_CLOSE, T_SELF, T_SKIP, T_END };

static int starts(const char *p, const char *end, const char *lit)
{
  int n = strlen(lit);
  return end - p >= n && memcmp(p, lit, (unsigned)n) == 0;
}

static const char *skip_to(const char *p, const char *end, const char *lit)
{
  int n = strlen(lit);
  for (; end - p >= n; p++)
    if (memcmp(p, lit, (unsigned)n) == 0)
      return p + n;
  return end;
}

struct tok {
  int kind;
  const char *start, *q, *pfx, *name;
  int qlen, plen, nlen;
};

/* Classify the markup at the next '<' at or after p. For an element tag, q/qlen is its qualified name and
 * pfx/plen, name/nlen its prefix and local parts. Returns the position after the tag. */
static const char *tag(const char *p, const char *end, struct tok *t)
{
  while (p < end && *p != '<')
    p++;
  t->start = p;
  if (p >= end) {
    t->kind = T_END;
    return end;
  }
  t->kind = T_SKIP;
  if (starts(p, end, "<!--"))
    return skip_to(p + 4, end, "-->");
  if (starts(p, end, "<?"))
    return skip_to(p + 2, end, "?>");
  if (starts(p, end, "<![CDATA["))
    return skip_to(p + 9, end, "]]>");
  if (starts(p, end, "<!"))
    return skip_to(p + 2, end, ">");

  const char *q = p + 1;
  t->kind = T_OPEN;
  if (q < end && *q == '/') {
    t->kind = T_CLOSE;
    q++;
  }
  t->q = q;
  while (q < end && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n')
    q++;
  t->qlen = (int)(q - t->q);
  t->pfx = t->q;
  t->plen = 0;
  t->name = t->q;
  t->nlen = t->qlen;
  for (const char *c = t->q; c < q; c++)
    if (*c == ':') {
      t->plen = (int)(c - t->q);
      t->name = c + 1;
      t->nlen = (int)(q - c - 1);
      break;
    }

  char quote = 0;
  for (; q < end; q++) {
    if (quote) {
      if (*q == quote)
        quote = 0;
    } else if (*q == '"' || *q == '\'') {
      quote = *q;
    } else if (*q == '>') {
      if (t->kind == T_OPEN && q[-1] == '/')
        t->kind = T_SELF;
      return q + 1;
    }
  }
  t->kind = T_END;
  return end;
}

/* Value of the namespace declaration for prefix (plen 0 = default) on the open tag t, or 0 if it has none. */
static const char *decl(const struct tok *t, const char *end, const char *pfx, int plen, int *vlen)
{
  const char *p = t->q + t->qlen;
  while (p < end && *p != '>') {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/'))
      p++;
    const char *n = p;
    while (p < end && *p != '=' && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      p++;
    int nl = (int)(p - n);
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
    if (p >= end || *p != '=')
      continue;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
    if (p >= end || (*p != '"' && *p != '\''))
      return 0;
    char quote = *p++;
    const char *v = p;
    while (p < end && *p != quote)
      p++;
    if (p >= end)
      return 0;
    int isdecl = plen ? nl == 6 + plen && memcmp(n, "xmlns:", 6) == 0 && memcmp(n + 6, pfx, (unsigned)plen) == 0
                      : nl == 5 && memcmp(n, "xmlns", 5) == 0;
    if (isdecl) {
      *vlen = (int)(p - v);
      return v;
    }
    p++;
  }
  return 0;
}

/* Whether element t, nested in the open tags open[0..depth), is in namespace ns. */
static int in_ns(const struct tok *t, const char *end, const char *const *open, int depth, const char *ns)
{
  int nl = strlen(ns), vl = 0;
  const char *v = decl(t, end, t->pfx, t->plen, &vl);
  for (int i = depth < MAXDEPTH ? depth : MAXDEPTH; !v && i-- > 0;) {
    struct tok a;
    tag(open[i], end, &a);
    v = decl(&a, end, t->pfx, t->plen, &vl);
  }
  if (!v)
    vl = 0;
  return vl == nl && memcmp(v, ns, (unsigned)nl) == 0;
}

struct xml_span xml_all(const char *doc)
{
  struct xml_span sp = {doc, doc + strlen(doc), doc};
  return sp;
}

int xml_next_ns(struct xml_span *scan, const char *ns, const char *name, struct xml_span *inner)
{
  int tl = strlen(name);
  const char *end = scan->e;
  const char *p = ns && scan->doc ? scan->doc : scan->s;
  const char *open[MAXDEPTH];
  int depth = 0, mdepth = -1;
  for (;;) {
    struct tok t;
    const char *after = tag(p, end, &t);
    if (t.kind == T_END)
      return 0;
    p = after;
    if (t.kind == T_SKIP)
      continue;
    if (t.kind == T_CLOSE) {
      if (depth > 0)
        depth--;
      if (depth == mdepth) {
        inner->e = t.start;
        inner->doc = scan->doc;
        scan->s = after;
        return 1;
      }
      continue;
    }
    if (mdepth < 0 && t.start >= scan->s && t.nlen == tl && memcmp(t.name, name, (unsigned)tl) == 0 &&
        (!ns || in_ns(&t, end, open, depth, ns))) {
      inner->doc = scan->doc;
      if (t.kind == T_SELF) {
        inner->s = inner->e = after;
        scan->s = after;
        return 1;
      }
      inner->s = after;
      mdepth = depth;
    }
    if (t.kind == T_OPEN) {
      if (depth < MAXDEPTH)
        open[depth] = t.start;
      depth++;
    }
  }
}

int xml_next(struct xml_span *scan, const char *name, struct xml_span *inner)
{
  return xml_next_ns(scan, 0, name, inner);
}

int xml_text_ns(struct xml_span in, const char *ns, const char *name, char *out, int cap)
{
  struct xml_span el;
  if (!xml_next_ns(&in, ns, name, &el))
    return -1;
  static const struct {
    const char *ent;
    char ch;
  } ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
  int n = 0;
  for (const char *c = el.s; c < el.e;) {
    char ch = *c;
    int adv = 1;
    if (ch == '<') {
      if (!starts(c, el.e, "<![CDATA["))
        break;
      const char *stop = skip_to(c + 9, el.e, "]]>");
      const char *dend = stop - 3 >= c + 9 && starts(stop - 3, el.e, "]]>") ? stop - 3 : stop;
      for (const char *d = c + 9; d < dend; d++) {
        if (n + 1 >= cap)
          return -1;
        out[n++] = *d;
      }
      c = stop;
      continue;
    }
    if (ch == '&')
      for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++) {
        int el_len = strlen(ents[i].ent);
        if (starts(c, el.e, ents[i].ent)) {
          ch = ents[i].ch;
          adv = el_len;
          break;
        }
      }
    if (n + 1 >= cap)
      return -1;
    out[n++] = ch;
    c += adv;
  }
  out[n] = 0;
  return n;
}

int xml_text(struct xml_span in, const char *name, char *out, int cap) { return xml_text_ns(in, 0, name, out, cap); }

int xml_escape(char *out, int len, int cap, const char *s)
{
  for (; *s; s++) {
    const char *rep = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : 0;
    int rl = rep ? strlen(rep) : 1;
    if (len + rl >= cap)
      return -1;
    if (rep)
      memcpy(out + len, rep, (unsigned)rl);
    else
      out[len] = *s;
    len += rl;
  }
  out[len] = 0;
  return len;
}
