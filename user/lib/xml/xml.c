/* xml.c: see xml.h. */

#include "xml.h"
#include "owos.h"

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

/* Classify the markup at the next '<' at or after p; name and nlen give the local name of an element tag.
 * Returns the position after the tag. */
static const char *tag(const char *p, const char *end, int *kind, const char **name, int *nlen)
{
  while (p < end && *p != '<')
    p++;
  if (p >= end) {
    *kind = T_END;
    return end;
  }
  *kind = T_SKIP;
  if (starts(p, end, "<!--"))
    return skip_to(p + 4, end, "-->");
  if (starts(p, end, "<?"))
    return skip_to(p + 2, end, "?>");
  if (starts(p, end, "<![CDATA["))
    return skip_to(p + 9, end, "]]>");
  if (starts(p, end, "<!"))
    return skip_to(p + 2, end, ">");

  const char *q = p + 1;
  *kind = T_OPEN;
  if (q < end && *q == '/') {
    *kind = T_CLOSE;
    q++;
  }
  const char *n = q;
  while (q < end && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n')
    q++;
  for (const char *c = n; c < q; c++)
    if (*c == ':')
      n = c + 1;
  *name = n;
  *nlen = (int)(q - n);

  char quote = 0;
  for (; q < end; q++) {
    if (quote) {
      if (*q == quote)
        quote = 0;
    } else if (*q == '"' || *q == '\'') {
      quote = *q;
    } else if (*q == '>') {
      if (*kind == T_OPEN && q[-1] == '/')
        *kind = T_SELF;
      return q + 1;
    }
  }
  *kind = T_END;
  return end;
}

struct xml_span xml_all(const char *doc)
{
  struct xml_span sp = {doc, doc + strlen(doc)};
  return sp;
}

int xml_next(struct xml_span *scan, const char *name, struct xml_span *inner)
{
  int tl = strlen(name);
  const char *p = scan->s, *end = scan->e;
  for (;;) {
    int kind, nl;
    const char *nm;
    const char *after = tag(p, end, &kind, &nm, &nl);
    if (kind == T_END)
      return 0;
    p = after;
    if ((kind != T_OPEN && kind != T_SELF) || nl != tl || memcmp(nm, name, (unsigned)tl) != 0)
      continue;
    if (kind == T_SELF) {
      inner->s = inner->e = after;
      scan->s = after;
      return 1;
    }
    inner->s = after;
    for (int depth = 1;;) {
      const char *start = p;
      p = tag(p, end, &kind, &nm, &nl);
      if (kind == T_END)
        return 0;
      if (nl != tl || memcmp(nm, name, (unsigned)tl) != 0)
        continue;
      if (kind == T_OPEN)
        depth++;
      else if (kind == T_CLOSE && --depth == 0) {
        while (start < end && start[0] != '<')
          start++;
        inner->e = start;
        scan->s = p;
        return 1;
      }
    }
  }
}

int xml_text(struct xml_span in, const char *name, char *out, int cap)
{
  struct xml_span el;
  if (!xml_next(&in, name, &el))
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
