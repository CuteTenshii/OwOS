#include "engine_int.h"

/* selectors */
static struct sel_list *parse_sel_list(const char *s, const char *e);
static void pseudo_free(struct pseudo *pc);
static void selector_finish(struct selector *sel);

/* True if byte c is in the NUL-terminated set. */
static bool chr_in(const char *set, char c)
{
  for (; *set; set++)
    if (*set == c)
      return true;
  return false;
}

/* First byte of `stop` at bracket depth 0; (...) and [...] are skipped as a unit. */
const char *scan_top(const char *s, const char *e, const char *stop)
{
  int depth = 0;
  for (; s < e; s++) {
    char c = *s;
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
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
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
  char *d = span_dup(s, e);
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
  while (s < e && is_sel_name(*s))
    s++;
  return s;
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
      ss->id = span_dup(s, ne);
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
  pc->arg.tag[0] = '\0';
  pc->arg.id = 0;
  pc->arg.cls = 0;
  pc->arg.ncls = 0;

  if (span_is(ns, ne, "not")) {
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
  return true; /* dynamic state / :has() / unsupported -> PC_NEVER */
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
  while (s < e) {
    if (*s == '.') {
      const char *ne = name_end(++s, e);
      cls_push(&st->cls, &st->ncls, s, ne);
      s = ne;
    } else if (*s == '#') {
      const char *ne = name_end(++s, e);
      free(st->id);
      st->id = span_dup(s, ne);
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
      else if (dbl || span_is(ns, ne, "first-line") || span_is(ns, ne, "first-letter"))
        st->pseudo_el = PE_OTHER;
      else
        add_pseudo_class(st, ns, ne, as, ae);
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
  case PC_NOT:
    return !simple_match(&pc->arg, n);
  case PC_MATCHES:
    if (!pc->list)
      return false;
    for (int i = 0; i < pc->list->n; i++)
      if (selector_match(&pc->list->sels[i], n))
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

/* Match steps [0, top] of `sel` against `subject`'s ancestors, right to left; greedy, no backtracking. */
static bool match_ancestors(const struct selector *sel, int top, const struct dom_node *subject)
{
  const struct dom_node *cur = subject;
  for (int i = top; i >= 0; i--) {
    cur = cur ? cur->parent : 0;
    if (sel->step[i].child) {
      if (!step_match(&sel->step[i], cur))
        return false;
    } else {
      while (cur && !step_match(&sel->step[i], cur))
        cur = cur->parent;
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
