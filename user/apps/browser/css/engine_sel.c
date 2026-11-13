#include "engine_int.h"

/* selectors */
static struct sel_list *parse_sel_list(const char *s, const char *e);
static void pseudo_free(struct pseudo *pc);
static void selector_finish(struct selector *sel);
static char *ident_dup(const char *s, const char *e);

/* True if byte c is in the NUL-terminated set. */
static bool chr_in(const char *set, char c)
{
  for (; *set; set++)
    if (*set == c)
      return true;
  return false;
}

static int hex_val(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

#define ESC_NONE 0xffffffffu /* the escape decodes to nothing; the bytes after it carry the character */

/* Past the CSS escape at s ('\'): up to six hex digits plus one whitespace, or the one escaped character. */
static const char *esc_end(const char *s, const char *e, unsigned *cp)
{
  const char *p = s + 1;
  if (p >= e || *p == '\n' || *p == '\r' || *p == '\f') {
    if (cp)
      *cp = '\\';
    return p; /* a lone backslash stands for itself */
  }
  if (hex_val(*p) >= 0) {
    unsigned v = 0;
    for (int i = 0; i < 6 && p < e && hex_val(*p) >= 0; i++)
      v = v * 16 + (unsigned)hex_val(*p++);
    if (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\f'))
      p++;
    else if (p + 1 < e && p[0] == '\r' && p[1] == '\n')
      p += 2;
    else if (p < e && *p == '\r')
      p++;
    if (cp)
      *cp = (v == 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) ? 0xfffd : v;
    return p;
  }
  if ((unsigned char)*p >= 0x80) {
    if (cp)
      *cp = ESC_NONE;
    return p; /* an escaped non-ASCII character is just its own UTF-8 bytes */
  }
  if (cp)
    *cp = (unsigned char)*p;
  return p + 1;
}

/* First byte of `stop` at bracket depth 0; (...) and [...] are skipped as a unit, escapes as one character. */
const char *scan_top(const char *s, const char *e, const char *stop)
{
  int depth = 0;
  for (; s < e; s++) {
    char c = *s;
    if (c == '\\') {
      s = esc_end(s, e, 0) - 1;
      continue;
    }
    if (c == '(' || c == '[')
      depth++;
    else if (c == ')' || c == ']') {
      if (depth)
        depth--;
    } else if (depth == 0 && chr_in(stop, c))
      break;
  }
  return s;
}

bool is_sel_name(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         (unsigned char)c >= 0x80;
}

/* Pseudo-classes: dynamic or unevaluable ones become PC_NEVER; empty :is()/:where() match nothing. */

/* an+b microsyntax (odd|even|<int>|an+b|-n+b) for the :nth-* pseudo-classes. */
static void parse_anb(const char *s, const char *e, int *a, int *b)
{
  *a = 0;
  *b = 0;
  while (s < e && (*s == ' ' || *s == '\t'))
    s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
    e--;
  if (span_is(s, e, "odd")) {
    *a = 2;
    *b = 1;
    return;
  }
  if (span_is(s, e, "even")) {
    *a = 2;
    *b = 0;
    return;
  }
  int sign = 1;
  bool has1 = false;
  int num1 = 0;
  if (s < e && (*s == '+' || *s == '-')) {
    if (*s == '-')
      sign = -1;
    s++;
  }
  while (s < e && *s >= '0' && *s <= '9') {
    num1 = num1 * 10 + (*s++ - '0');
    has1 = true;
  }
  if (s < e && (*s == 'n' || *s == 'N')) {
    s++;
    *a = sign * (has1 ? num1 : 1);
    while (s < e && (*s == ' ' || *s == '\t'))
      s++;
    int bsign = 1;
    bool has2 = false;
    int num2 = 0;
    if (s < e && (*s == '+' || *s == '-')) {
      if (*s == '-')
        bsign = -1;
      s++;
    }
    while (s < e && (*s == ' ' || *s == '\t'))
      s++;
    while (s < e && *s >= '0' && *s <= '9') {
      num2 = num2 * 10 + (*s++ - '0');
      has2 = true;
    }
    *b = has2 ? bsign * num2 : 0;
  } else {
    *b = sign * num1;
  }
}

/* Parse a single simple selector (no combinators) for :not(). */
static char *span_dup(const char *s, const char *e)
{
  char *d = malloc((unsigned)(e - s) + 1);
  if (!d)
    return 0;
  memcpy(d, s, (unsigned)(e - s));
  d[e - s] = '\0';
  return d;
}
static void cls_push(char ***v, int *n, const char *s, const char *e)
{
  if (e <= s)
    return;
  char *d = ident_dup(s, e);
  if (!d)
    return;
  char **nv = realloc(*v, (unsigned)(*n + 1) * sizeof *nv);
  if (!nv) {
    free(d);
    return;
  }
  nv[(*n)++] = d;
  *v = nv;
}
static void cls_free(char **v, int n)
{
  for (int i = 0; i < n; i++)
    free(v[i]);
  free(v);
}
/* Copy a tag name span, lowercased and cut to the DOM's tag width. */
static void tag_copy(char tag[16], const char *s, const char *e)
{
  int k = 0;
  for (; s + k < e && k < 15; k++)
    tag[k] = lower(s[k]);
  tag[k] = '\0';
}
static const char *name_end(const char *s, const char *e)
{
  while (s < e) {
    if (is_sel_name(*s))
      s++;
    else if (*s == '\\')
      s = esc_end(s, e, 0);
    else
      break;
  }
  return s;
}

/* Heap copy of the identifier [s,e) with its escapes decoded to UTF-8 (".md\:flex" names class "md:flex"). */
static char *ident_dup(const char *s, const char *e)
{
  char *d = malloc((unsigned)(e - s) * 2 + 1); /* "\0" (2 bytes) decodes to U+FFFD (3 bytes) */
  if (!d)
    return 0;
  int n = 0;
  while (s < e) {
    if (*s != '\\') {
      d[n++] = *s++;
      continue;
    }
    unsigned cp;
    s = esc_end(s, e, &cp);
    if (cp != ESC_NONE)
      n += utf8_encode(cp, d + n);
  }
  d[n] = '\0';
  return d;
}

static void parse_simple_sel(const char *s, const char *e, struct simple_sel *ss)
{
  ss->tag[0] = '\0';
  ss->id = 0;
  ss->cls = 0;
  ss->ncls = 0;
  while (s < e) {
    if (*s == '.') {
      const char *ne = name_end(++s, e);
      cls_push(&ss->cls, &ss->ncls, s, ne);
      s = ne;
    } else if (*s == '#') {
      const char *ne = name_end(++s, e);
      free(ss->id);
      ss->id = ident_dup(s, ne);
      s = ne;
    } else if (*s == '*') {
      s++;
    } else if (is_sel_name(*s)) {
      const char *ne = name_end(s, e);
      tag_copy(ss->tag, s, ne);
      s = ne;
    } else
      s++;
  }
}

/* Classify a pseudo-class name span (+ optional arg) into *pc; 0 to ignore (:is()/:where()). */
static bool classify_pseudo(const char *ns, const char *ne, const char *as, const char *ae, struct pseudo *pc)
{
  pc->kind = PC_NEVER;
  pc->a = 0;
  pc->b = 0;
  pc->list = 0;
  pc->an = pc->av = 0;
  pc->aop = 0;
  pc->aci = false;
  pc->arg.tag[0] = '\0';
  pc->arg.id = 0;
  pc->arg.cls = 0;
  pc->arg.ncls = 0;

  if (span_is(ns, ne, "not")) {
    bool complex = false;
    for (const char *p = as; as && p < ae; p++)
      complex |= *p == '[' || *p == ',' || *p == ':' || *p == ' ' || *p == '>';
    if (complex) {
      pc->list = parse_sel_list(as, ae);
      pc->kind = pc->list ? PC_NOT_LIST : PC_NEVER;
      return true;
    }
    pc->kind = PC_NOT;
    if (as)
      parse_simple_sel(as, ae, &pc->arg);
    return true;
  }
  if (span_is(ns, ne, "first-child")) {
    pc->kind = PC_FIRST_CHILD;
    return true;
  }
  if (span_is(ns, ne, "last-child")) {
    pc->kind = PC_LAST_CHILD;
    return true;
  }
  if (span_is(ns, ne, "only-child")) {
    pc->kind = PC_ONLY_CHILD;
    return true;
  }
  if (span_is(ns, ne, "first-of-type")) {
    pc->kind = PC_FIRST_OF_TYPE;
    return true;
  }
  if (span_is(ns, ne, "last-of-type")) {
    pc->kind = PC_LAST_OF_TYPE;
    return true;
  }
  if (span_is(ns, ne, "only-of-type")) {
    pc->kind = PC_ONLY_OF_TYPE;
    return true;
  }
  if (span_is(ns, ne, "nth-child")) {
    pc->kind = PC_NTH_CHILD;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return true;
  }
  if (span_is(ns, ne, "nth-last-child")) {
    pc->kind = PC_NTH_LAST_CHILD;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return true;
  }
  if (span_is(ns, ne, "nth-of-type")) {
    pc->kind = PC_NTH_OF_TYPE;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return true;
  }
  if (span_is(ns, ne, "nth-last-of-type")) {
    pc->kind = PC_NTH_LAST_OF_TYPE;
    if (as)
      parse_anb(as, ae, &pc->a, &pc->b);
    return true;
  }
  if (span_is(ns, ne, "root")) {
    pc->kind = PC_ROOT;
    return true;
  }
  if (span_is(ns, ne, "empty")) {
    pc->kind = PC_EMPTY;
    return true;
  }
  if (span_is(ns, ne, "checked")) {
    pc->kind = PC_CHECKED;
    return true;
  }
  if (span_is(ns, ne, "disabled")) {
    pc->kind = PC_DISABLED;
    return true;
  }
  if (span_is(ns, ne, "enabled")) {
    pc->kind = PC_ENABLED;
    return true;
  }
  if (span_is(ns, ne, "required")) {
    pc->kind = PC_REQUIRED;
    return true;
  }
  if (span_is(ns, ne, "optional")) {
    pc->kind = PC_OPTIONAL;
    return true;
  }
  if (span_is(ns, ne, "link") || span_is(ns, ne, "any-link")) {
    pc->kind = PC_LINK;
    return true;
  }
  if (span_is(ns, ne, "popover-open")) {
    pc->kind = PC_POPOVER_OPEN;
    return true;
  }
  if (span_is(ns, ne, "modal")) {
    pc->kind = PC_MODAL;
    return true;
  }
  if (span_is(ns, ne, "host") || span_is(ns, ne, "host-context")) {
    pc->kind = span_is(ns, ne, "host") ? PC_HOST : PC_HOST_CONTEXT;
    pc->list = (as && as < ae) ? parse_sel_list(as, ae) : 0;
    if (pc->kind == PC_HOST_CONTEXT && !pc->list)
      pc->kind = PC_NEVER;
    return true;
  }
  if (span_is(ns, ne, "is") || span_is(ns, ne, "where")) {
    /* :is()/:where() match if any sub-selector matches; an empty or unparsable argument matches nothing. */
    struct sel_list *l = (as && as < ae) ? parse_sel_list(as, ae) : 0;
    if (!l)
      return true; /* PC_NEVER (matches nothing) */
    pc->kind = PC_MATCHES;
    pc->list = l;
    pc->a = span_is(ns, ne, "where"); /* :where() carries zero specificity */
    return true;
  }
  if (span_is(ns, ne, "hover") || span_is(ns, ne, "active") || span_is(ns, ne, "focus") ||
      span_is(ns, ne, "focus-visible") || span_is(ns, ne, "focus-within")) {
    pc->kind = span_is(ns, ne, "hover")          ? PC_HOVER
               : span_is(ns, ne, "active")       ? PC_ACTIVE
               : span_is(ns, ne, "focus-within") ? PC_FOCUS_WITHIN
                                                 : PC_FOCUS;
    g_state_used = true;
    return true;
  }
  return true; /* :has() / unsupported -> PC_NEVER */
}

static void add_pseudo_class(struct sel_step *st, const char *ns, const char *ne, const char *as, const char *ae)
{
  struct pseudo pc;
  if (!classify_pseudo(ns, ne, as, ae, &pc))
    return;
  if (pc.kind >= PC_FIRST_CHILD && pc.kind <= PC_EMPTY)
    dom_structural_styles = true;
  struct pseudo *np = realloc(st->pc, (unsigned)(st->npc + 1) * sizeof *np);
  if (!np) {
    pseudo_free(&pc);
    return;
  }
  st->pc = np;
  st->pc[st->npc++] = pc;
}

/* Parse the inside of [...] starting after the '['; returns the position after the ']'. */
static const char *parse_attr_sel(const char *s, const char *e, struct sel_step *st)
{
  struct pseudo pc;
  const char *close = s, *p;
  char q = 0;
  while (close < e && (q || *close != ']')) {
    if (q && *close == q)
      q = 0;
    else if (!q && (*close == '"' || *close == '\''))
      q = *close;
    close++;
  }
  const char *next = close < e ? close + 1 : close;
  memset(&pc, 0, sizeof pc);
  pc.kind = PC_ATTR;
  while (s < close && (*s == ' ' || *s == '\t'))
    s++;
  p = s;
  while (p < close && !chr_in("~|^$*= \t", *p))
    p++;
  if (p == s)
    return next;
  pc.an = span_dup(s, p);
  if (!pc.an)
    return next;
  for (char *c = pc.an; *c; c++)
    *c = lower(*c);
  while (p < close && (*p == ' ' || *p == '\t'))
    p++;
  if (p < close && *p != '=' && p + 1 < close && p[1] == '=') {
    pc.aop = *p;
    p += 2;
  } else if (p < close && *p == '=') {
    pc.aop = '=';
    p++;
  }
  if (pc.aop) {
    while (p < close && (*p == ' ' || *p == '\t'))
      p++;
    const char *vs = p, *ve;
    if (p < close && (*p == '"' || *p == '\'')) {
      char qc = *p++;
      vs = p;
      while (p < close && *p != qc)
        p++;
      ve = p;
      if (p < close)
        p++;
    } else {
      while (p < close && *p != ' ' && *p != '\t')
        p++;
      ve = p;
    }
    pc.av = span_dup(vs, ve);
    while (p < close && (*p == ' ' || *p == '\t'))
      p++;
    pc.aci = p < close && (*p == 'i' || *p == 'I');
  }
  struct pseudo *np = realloc(st->pc, (unsigned)(st->npc + 1) * sizeof *np);
  if (!np) {
    pseudo_free(&pc);
    return next;
  }
  st->pc = np;
  st->pc[st->npc++] = pc;
  return next;
}

/* Parse one compound selector token (between combinators) into *st. */
static void parse_compound(const char *s, const char *e, struct sel_step *st)
{
  st->tag[0] = '\0';
  st->id = 0;
  st->cls = 0;
  st->ncls = 0;
  st->pc = 0;
  st->npc = 0;
  st->pseudo_el = 0;
  st->pe_list = 0;
  st->pe_names = 0;
  while (s < e) {
    if (*s == '.') {
      const char *ne = name_end(++s, e);
      cls_push(&st->cls, &st->ncls, s, ne);
      s = ne;
    } else if (*s == '#') {
      const char *ne = name_end(++s, e);
      free(st->id);
      st->id = ident_dup(s, ne);
      s = ne;
    } else if (*s == ':') {
      s++;
      bool dbl = false;
      if (s < e && *s == ':') {
        s++;
        dbl = true;
      }
      const char *ns = s;
      while (s < e && is_sel_name(*s))
        s++;
      const char *ne = s;
      const char *as = 0, *ae = 0; /* an optional ( ... ) argument */
      if (s < e && *s == '(') {
        int depth = 1;
        s++;
        as = s;
        while (s < e && depth) {
          if (*s == '(')
            depth++;
          else if (*s == ')') {
            depth--;
            if (!depth)
              break;
          }
          s++;
        }
        ae = s;
        if (s < e)
          s++;
      }
      /* ::before/::after render content; other pseudo-elements never match a real element. */
      if (span_is(ns, ne, "before"))
        st->pseudo_el = PE_BEFORE;
      else if (span_is(ns, ne, "after"))
        st->pseudo_el = PE_AFTER;
      else if (dbl && (span_is(ns, ne, "selection") || span_is(ns, ne, "-moz-selection")))
        st->pseudo_el = PE_SELECTION;
      else if (dbl && span_is(ns, ne, "backdrop"))
        st->pseudo_el = PE_BACKDROP;
      else if (span_is(ns, ne, "marker") || span_is(ns, ne, "-webkit-details-marker"))
        st->pseudo_el = PE_MARKER;
      else if (span_is(ns, ne, "-webkit-scrollbar"))
        st->pseudo_el = PE_SCROLLBAR;
      else if (span_is(ns, ne, "-webkit-scrollbar-thumb"))
        st->pseudo_el = PE_SB_THUMB;
      else if (span_is(ns, ne, "-webkit-scrollbar-track"))
        st->pseudo_el = PE_SB_TRACK;
      else if (dbl && span_is(ns, ne, "slotted") && as) {
        st->pseudo_el = PE_SLOTTED;
        st->pe_list = parse_sel_list(as, ae);
        if (!st->pe_list)
          st->pseudo_el = PE_OTHER;
      } else if (dbl && span_is(ns, ne, "part") && as) {
        st->pseudo_el = PE_PART;
        free(st->pe_names);
        st->pe_names = span_dup(as, ae);
      } else if (dbl || span_is(ns, ne, "first-line") || span_is(ns, ne, "first-letter"))
        st->pseudo_el = PE_OTHER;
      else
        add_pseudo_class(st, ns, ne, as, ae);
    } else if (*s == '[') {
      s = parse_attr_sel(s + 1, e, st);
    } else if (*s == '*') {
      s++; /* universal: no constraint */
    } else if (is_sel_name(*s)) {
      const char *ne = name_end(s, e);
      tag_copy(st->tag, s, ne);
      s = ne;
    } else {
      s++;
    }
  }
}

/* Parse one full selector (no commas) into *sel; 0 if empty or too deep. */
bool parse_selector(const char *s, const char *e, struct selector *sel)
{
  sel->n = 0;
  sel->step = 0;
  bool pending_child = false;
  while (s < e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
      s++;
    if (s >= e)
      break;
    if (*s == '>') {
      pending_child = true;
      s++;
      continue;
    }
    /* A compound runs to the next top-level combinator; scan_top skips (...) and [...]. */
    const char *tok = s;
    s = scan_top(s, e, " \t\n\r>");
    struct sel_step *ns = realloc(sel->step, (unsigned)(sel->n + 1) * sizeof *ns);
    if (!ns) {
      selector_free(sel);
      return false;
    }
    sel->step = ns;
    struct sel_step *st = &sel->step[sel->n];
    parse_compound(tok, s, st);
    st->child = false;
    if (sel->n > 0 && pending_child)
      sel->step[sel->n - 1].child = true;
    pending_child = false;
    sel->n++;
  }
  selector_finish(sel);
  return sel->n > 0;
}

/* Free everything a parsed selector owns. */
static void free_sel_list(struct sel_list *l);
static void simple_sel_free(struct simple_sel *ss)
{
  free(ss->id);
  cls_free(ss->cls, ss->ncls);
}
static void pseudo_free(struct pseudo *pc)
{
  free_sel_list(pc->list);
  simple_sel_free(&pc->arg);
  free(pc->an);
  free(pc->av);
}
void selector_free(struct selector *sel)
{
  for (int i = 0; i < sel->n; i++) {
    struct sel_step *st = &sel->step[i];
    free(st->id);
    cls_free(st->cls, st->ncls);
    for (int j = 0; j < st->npc; j++)
      pseudo_free(&st->pc[j]);
    free(st->pc);
    free_sel_list(st->pe_list);
    free(st->pe_names);
  }
  free(sel->step);
  sel->step = 0;
  sel->n = 0;
}
static void free_sel_list(struct sel_list *l)
{
  if (!l)
    return;
  for (int i = 0; i < l->n; i++)
    selector_free(&l->sels[i]);
  free(l->sels);
  free(l);
}

/* Split [s,e) on top-level commas into a heap list of parsed selectors; 0 if none parse (depth-guarded). */
static int g_is_depth;
static struct sel_list *parse_sel_list(const char *s, const char *e)
{
  if (g_is_depth >= 8)
    return 0;
  int cap = 1; /* pieces = top-level commas + 1 */
  for (const char *p = s; p < e;) {
    const char *c = scan_top(p, e, ",");
    if (c < e)
      cap++;
    p = (c < e) ? c + 1 : e;
  }
  struct selector *sels = malloc((unsigned)cap * sizeof *sels);
  if (!sels)
    return 0;
  int n = 0;
  g_is_depth++;
  for (const char *q = s; q < e && n < cap;) {
    const char *comma = scan_top(q, e, ",");
    if (parse_selector(q, comma, &sels[n]))
      n++;
    else
      selector_free(&sels[n]);
    q = (comma < e) ? comma + 1 : e;
  }
  g_is_depth--;
  if (n == 0) {
    free(sels);
    return 0;
  }
  struct sel_list *l = malloc(sizeof *l);
  if (!l) {
    for (int i = 0; i < n; i++)
      selector_free(&sels[i]);
    free(sels);
    return 0;
  }
  l->n = n;
  l->sels = sels;
  return l;
}

struct sel_list *css_sel_parse(const char *s) { return parse_sel_list(s, s + strlen(s)); }
void css_sel_free(struct sel_list *l) { free_sel_list(l); }
bool css_sel_match(const struct sel_list *l, const struct dom_node *el)
{
  for (int i = 0; l && i < l->n; i++)
    if (selector_match(&l->sels[i], el))
      return true;
  return false;
}

int selector_spec(const struct selector *sel)
{
  int a = 0, b = 0, c = 0;
  for (int i = 0; i < sel->n; i++) {
    const struct sel_step *st = &sel->step[i];
    if (st->id)
      a++;
    b += st->ncls;
    if (st->tag[0])
      c++;
    for (int j = 0; j < st->npc; j++) { /* pseudo-classes add specificity */
      const struct pseudo *pc = &st->pc[j];
      if (pc->kind == PC_NOT) { /* :not() contributes its argument's */
        if (pc->arg.id)
          a++;
        b += pc->arg.ncls;
        if (pc->arg.tag[0])
          c++;
      } else if (pc->kind == PC_MATCHES) {
        /* :is() takes its most specific argument; :where() contributes nothing. */
        if (!pc->a && pc->list) {
          int best = 0;
          for (int k = 0; k < pc->list->n; k++) {
            int s = selector_spec(&pc->list->sels[k]);
            if (s > best)
              best = s;
          }
          a += best / 10000;
          b += (best / 100) % 100;
          c += best % 100;
        }
      } else {
        b++; /* a pseudo-class counts like a class */
      }
    }
  }
  return a * 10000 + b * 100 + c;
}

/* matching */
static bool node_has_class(const struct dom_node *n, const char *cls)
{
  const char *list = dom_get_attr(n, "class");
  if (!list)
    return false;
  int cl = strlen(cls);
  for (const char *p = list; *p;) {
    while (*p == ' ' || *p == '\t')
      p++;
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t')
      q++;
    if (q - p == cl) {
      int i = 0;
      while (i < cl && p[i] == cls[i])
        i++;
      if (i == cl)
        return true;
    }
    p = q;
  }
  return false;
}

/* pseudo-class matching */
/* idx satisfies an+b for some integer k >= 0. */
static bool anb_match(int a, int b, int idx)
{
  int d = idx - b;
  if (a == 0)
    return d == 0;
  if ((a > 0 && d < 0) || (a < 0 && d > 0))
    return false;
  return d % a == 0;
}
static bool is_form_control(const char *tag)
{
  return streq(tag, "input") || streq(tag, "button") || streq(tag, "select") || streq(tag, "textarea") ||
         streq(tag, "option") || streq(tag, "optgroup") || streq(tag, "fieldset");
}
static bool simple_match(const struct simple_sel *ss, const struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return false;
  if (ss->tag[0] && !streq(ss->tag, n->tag))
    return false;
  if (ss->id) {
    const char *id = dom_get_attr(n, "id");
    if (!id || !streq_ci(id, ss->id))
      return false;
  }
  for (int i = 0; i < ss->ncls; i++)
    if (!node_has_class(n, ss->cls[i]))
      return false;
  return true;
}
static bool attr_eq(const char *a, const char *b, int len, bool ci)
{
  for (int i = 0; i < len; i++)
    if (ci ? lower(a[i]) != lower(b[i]) : a[i] != b[i])
      return false;
  return true;
}
static bool attr_value_match(const struct pseudo *pc, const char *v)
{
  const char *w = pc->av ? pc->av : "";
  int vl = (int)strlen(v), wl = (int)strlen(w);
  switch (pc->aop) {
  case '=':
    return vl == wl && attr_eq(v, w, wl, pc->aci);
  case '^':
    return wl && vl >= wl && attr_eq(v, w, wl, pc->aci);
  case '$':
    return wl && vl >= wl && attr_eq(v + vl - wl, w, wl, pc->aci);
  case '*':
    for (int i = 0; wl && i + wl <= vl; i++)
      if (attr_eq(v + i, w, wl, pc->aci))
        return true;
    return false;
  case '|':
    return vl >= wl && attr_eq(v, w, wl, pc->aci) && (vl == wl || v[wl] == '-');
  case '~':
    for (const char *p = v; wl && *p;) {
      while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
      const char *q = p;
      while (*q && *q != ' ' && *q != '\t' && *q != '\n')
        q++;
      if ((int)(q - p) == wl && attr_eq(p, w, wl, pc->aci))
        return true;
      p = q;
    }
    return false;
  }
  return false;
}

/* True when `n` is `from` or one of its flat-tree ancestors. */
static bool in_chain(const struct dom_node *from, const struct dom_node *n)
{
  for (const struct dom_node *m = from; m; m = dom_flat_parent(m))
    if (m == n)
      return true;
  return false;
}

static bool pseudo_match(const struct pseudo *pc, const struct dom_node *n)
{
  switch (pc->kind) {
  case PC_FIRST_CHILD:
    return css_elem_index(n) == 1;
  case PC_LAST_CHILD:
    return css_elem_index(n) == css_elem_count(n);
  case PC_ONLY_CHILD:
    return css_elem_count(n) == 1;
  case PC_FIRST_OF_TYPE:
    return css_type_index(n) == 1;
  case PC_LAST_OF_TYPE:
    return css_type_index(n) == css_type_count(n);
  case PC_ONLY_OF_TYPE:
    return css_type_count(n) == 1;
  case PC_NTH_CHILD:
    return anb_match(pc->a, pc->b, css_elem_index(n));
  case PC_NTH_LAST_CHILD:
    return anb_match(pc->a, pc->b, css_elem_count(n) - css_elem_index(n) + 1);
  case PC_NTH_OF_TYPE:
    return anb_match(pc->a, pc->b, css_type_index(n));
  case PC_NTH_LAST_OF_TYPE:
    return anb_match(pc->a, pc->b, css_type_count(n) - css_type_index(n) + 1);
  case PC_ROOT:
    return n->parent && !n->parent->parent;
  case PC_EMPTY:
    for (const struct dom_node *c = n->first_child; c; c = c->next) {
      if (c->type == DOM_ELEMENT)
        return false;
      if (c->type == DOM_TEXT && c->text)
        for (const char *t = c->text; *t; t++)
          if (*t != ' ' && *t != '\t' && *t != '\n' && *t != '\r' && *t != '\f')
            return false;
    }
    return true;
  case PC_CHECKED:
    return dom_get_attr(n, "checked") != 0 || dom_get_attr(n, "selected") != 0;
  case PC_DISABLED:
    return dom_get_attr(n, "disabled") != 0;
  case PC_ENABLED:
    return dom_get_attr(n, "disabled") == 0 && is_form_control(n->tag);
  case PC_REQUIRED:
    return dom_get_attr(n, "required") != 0;
  case PC_OPTIONAL:
    return dom_get_attr(n, "required") == 0 && is_form_control(n->tag);
  case PC_LINK:
    return (streq(n->tag, "a") || streq(n->tag, "area") || streq(n->tag, "link")) && dom_get_attr(n, "href") != 0;
  case PC_POPOVER_OPEN:
    return dom_get_attr(n, "popover-open") != 0;
  case PC_MODAL:
    return dom_get_attr(n, "dialog-modal") != 0 && dom_get_attr(n, "open") != 0;
  case PC_NOT:
    return !simple_match(&pc->arg, n);
  case PC_MATCHES:
    if (!pc->list)
      return false;
    for (int i = 0; i < pc->list->n; i++)
      if (selector_match(&pc->list->sels[i], n))
        return true;
    return false;
  case PC_ATTR: {
    const char *v = dom_get_attr(n, pc->an);
    if (!v)
      return false;
    if (!pc->aop)
      return true;
    return attr_value_match(pc, v);
  }
  case PC_HOVER:
    return in_chain(g_state_hover, n);
  case PC_ACTIVE:
    return in_chain(g_state_active, n);
  case PC_FOCUS:
    return n == g_state_focus;
  case PC_FOCUS_WITHIN:
    return in_chain(g_state_focus, n);
  case PC_NOT_LIST:
    for (int i = 0; i < pc->list->n; i++)
      if (selector_match(&pc->list->sels[i], n))
        return false;
    return true;
  case PC_HOST:
    if (n != g_css_scope_host)
      return false;
    if (!pc->list)
      return true;
    for (int i = 0; i < pc->list->n; i++)
      if (selector_match(&pc->list->sels[i], n))
        return true;
    return false;
  case PC_HOST_CONTEXT:
    if (n != g_css_scope_host)
      return false;
    for (const struct dom_node *m = n; m; m = dom_flat_parent(m))
      for (int i = 0; i < pc->list->n; i++)
        if (selector_match(&pc->list->sels[i], m))
          return true;
    return false;
  default:
    return false; /* PC_NEVER */
  }
}

/* Match a step's tag/id/class/pseudo-class against `n`, ignoring any pseudo-element. */
static bool step_box_match(const struct sel_step *st, const struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return false;
  if (st->tag[0] && !streq(st->tag, n->tag))
    return false;
  if (st->id) {
    const char *id = dom_get_attr(n, "id");
    if (!id || !streq_ci(id, st->id))
      return false;
  }
  for (int i = 0; i < st->ncls; i++)
    if (!node_has_class(n, st->cls[i]))
      return false;
  for (int i = 0; i < st->npc; i++)
    if (!pseudo_match(&st->pc[i], n))
      return false;
  return true;
}

static bool step_match(const struct sel_step *st, const struct dom_node *n)
{
  if (st->pseudo_el)
    return false; /* ::before/::selection/… don't style a box */
  return step_box_match(st, n);
}

/* ---- ancestor bloom: per-element 128-bit filter of ancestor tag/id/classes ---- */
enum { BK_TAG = 1, BK_ID, BK_CLS };
static unsigned bloom_hash(unsigned kind, const char *s, int n, bool fold)
{
  unsigned h = (2166136261u ^ kind) * 16777619u;
  for (int i = 0; i < n; i++) {
    h ^= (unsigned char)(fold ? lower(s[i]) : s[i]);
    h *= 16777619u;
  }
  return h ^ (h >> 15);
}
static void bloom_set(unsigned b[4], unsigned h)
{
  unsigned x = h & 127, y = (h >> 7) & 127;
  b[x >> 5] |= 1u << (x & 31);
  b[y >> 5] |= 1u << (y & 31);
}
static void bloom_add_node(unsigned b[4], const struct dom_node *n)
{
  bloom_set(b, bloom_hash(BK_TAG, n->tag, (int)strlen(n->tag), false));
  const char *id = dom_get_attr(n, "id");
  if (id && id[0])
    bloom_set(b, bloom_hash(BK_ID, id, (int)strlen(id), true));
  const char *cl = dom_get_attr(n, "class");
  for (const char *p = cl ? cl : ""; *p;) {
    while (*p == ' ' || *p == '\t')
      p++;
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t')
      q++;
    if (q > p)
      bloom_set(b, bloom_hash(BK_CLS, p, (int)(q - p), false));
    p = q;
  }
}
static void selector_finish(struct selector *sel)
{
  memset(sel->anc_req, 0, sizeof sel->anc_req);
  sel->anc_never = false;
  for (int i = 0; i < sel->n - 1; i++) {
    const struct sel_step *st = &sel->step[i];
    if (st->pseudo_el)
      sel->anc_never = true;
    if (st->tag[0])
      bloom_set(sel->anc_req, bloom_hash(BK_TAG, st->tag, (int)strlen(st->tag), false));
    if (st->id)
      bloom_set(sel->anc_req, bloom_hash(BK_ID, st->id, (int)strlen(st->id), true));
    for (int c = 0; c < st->ncls; c++)
      bloom_set(sel->anc_req, bloom_hash(BK_CLS, st->cls[c], (int)strlen(st->cls[c]), false));
  }
}

#define BLOOM_MAX_DEPTH 100
static const unsigned *ancestor_bloom(const struct dom_node *n, int depth)
{
  static const unsigned all[4] = {~0u, ~0u, ~0u, ~0u};
  struct dom_node *m = (struct dom_node *)n;
  unsigned stamp = dom_mutation_seq + 1; /* 0 = never computed */
  if (m->anc_bloom_seq == stamp)
    return m->anc_bloom;
  if (depth > BLOOM_MAX_DEPTH)
    return all;
  const struct dom_node *p = n->parent;
  if (!p) {
    memset(m->anc_bloom, 0, sizeof m->anc_bloom);
  } else {
    memcpy(m->anc_bloom, ancestor_bloom(p, depth + 1), sizeof m->anc_bloom);
    if (p->type == DOM_ELEMENT)
      bloom_add_node(m->anc_bloom, p);
  }
  m->anc_bloom_seq = stamp;
  return m->anc_bloom;
}

static bool ancestors_possible(const struct selector *sel, const struct dom_node *subject)
{
  if (sel->n < 2)
    return true;
  if (sel->anc_never)
    return false;
  const unsigned *b = ancestor_bloom(subject, 0);
  for (int w = 0; w < 4; w++)
    if (sel->anc_req[w] & ~b[w])
      return false;
  return true;
}

const struct dom_node *g_css_scope_host;
int g_css_mode;

static bool step_has_host(const struct sel_step *st)
{
  for (int i = 0; i < st->npc; i++)
    if (st->pc[i].kind == PC_HOST || st->pc[i].kind == PC_HOST_CONTEXT)
      return true;
  return false;
}

/* Parent of `n` for matching `st`: a shadow root continues to its host only for a :host step */
static const struct dom_node *anc_up(const struct dom_node *n, const struct sel_step *st)
{
  return n->parent ? n->parent : (n->host && step_has_host(st) ? n->host : 0);
}

bool selector_subject_is_host(const struct selector *sel) { return step_has_host(&sel->step[sel->n - 1]); }

static bool match_ancestors(const struct selector *sel, int top, const struct dom_node *subject);

/* A ::slotted(sel) rule against a light child `el`: sel matches el, the rest matches the slot it is assigned to */
bool selector_match_slotted(const struct selector *sel, const struct dom_node *el)
{
  int i = sel->n - 1;
  const struct sel_step *st = &sel->step[i];
  const struct dom_node *slot = st->pseudo_el == PE_SLOTTED ? dom_assigned_slot(el) : 0;
  bool arg_ok = false;
  if (!slot)
    return false;
  for (int k = 0; k < st->pe_list->n && !arg_ok; k++)
    arg_ok = selector_match(&st->pe_list->sels[k], el);
  return arg_ok && step_box_match(st, slot) && ancestors_possible(sel, slot) && match_ancestors(sel, i - 1, slot);
}

/* A ::part(name) rule against `el` inside a shadow tree: the rest of the selector matches the tree's host */
bool selector_match_part(const struct selector *sel, const struct dom_node *el)
{
  int i = sel->n - 1;
  const struct sel_step *st = &sel->step[i];
  const char *have = dom_get_attr(el, "part");
  const struct dom_node *host = el;
  if (st->pseudo_el != PE_PART || !have || !st->pe_names)
    return false;
  while (host->parent)
    host = host->parent;
  host = host->host;
  if (!host)
    return false;
  for (const char *p = st->pe_names; *p;) { /* every requested name must be on the element */
    while (*p == ' ')
      p++;
    const char *q = p;
    while (*q && *q != ' ')
      q++;
    if (q > p) {
      bool found = false;
      for (const char *h = have; *h && !found;) {
        while (*h == ' ')
          h++;
        const char *he = h;
        while (*he && *he != ' ')
          he++;
        found = he - h == q - p && memcmp(h, p, (unsigned)(q - p)) == 0;
        h = he;
      }
      if (!found)
        return false;
    }
    p = q;
  }
  return step_box_match(st, host) && ancestors_possible(sel, host) && match_ancestors(sel, i - 1, host);
}

/* Match steps [0, top] of `sel` against `subject`'s ancestors, right to left; greedy, no backtracking. */
static bool match_ancestors(const struct selector *sel, int top, const struct dom_node *subject)
{
  const struct dom_node *cur = subject;
  for (int i = top; i >= 0; i--) {
    cur = cur ? anc_up(cur, &sel->step[i]) : 0;
    if (sel->step[i].child) {
      if (!step_match(&sel->step[i], cur))
        return false;
    } else {
      while (cur && !step_match(&sel->step[i], cur))
        cur = anc_up(cur, &sel->step[i]);
      if (!cur)
        return false;
    }
  }
  return true;
}

bool selector_match(const struct selector *sel, const struct dom_node *el)
{
  int i = sel->n - 1;
  if (!step_match(&sel->step[i], el))
    return false;
  return ancestors_possible(sel, el) && match_ancestors(sel, i - 1, el);
}

/* Match a ::before/::after rule against its originating element `el`. */
bool selector_match_pseudo(const struct selector *sel, const struct dom_node *el, int which)
{
  int i = sel->n - 1;
  if (sel->step[i].pseudo_el != which)
    return false;
  if (!step_box_match(&sel->step[i], el))
    return false;
  return ancestors_possible(sel, el) && match_ancestors(sel, i - 1, el);
}
