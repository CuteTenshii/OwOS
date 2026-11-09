/* dom.c: HTML tokenizer + tree builder. Not a full HTML5 state machine (no adoption agency etc). */
#include "dom.h"
#include "owos.h"
#include <stdbool.h>

#define MAX_DEPTH 256 /* bounds tree depth */

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_name_char(char c) { return is_alpha(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '_'; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static char *dupn(const char *s, int n)
{
  if (n < 0)
    n = 0;
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return 0;
  for (int i = 0; i < n; i++)
    d[i] = s[i];
  d[n] = '\0';
  return d;
}
static char *dupn_lower(const char *s, int n)
{
  if (n < 0)
    n = 0;
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return 0;
  for (int i = 0; i < n; i++)
    d[i] = lower(s[i]);
  d[n] = '\0';
  return d;
}

static bool name_in(const char *n, const char *const *set)
{
  for (int i = 0; set[i]; i++)
    if (streq(n, set[i]))
      return true;
  return false;
}

static bool is_void(const char *name)
{
  static const char *const v[] = {"area", "base", "br",    "col",    "embed", "hr",  "img", "input",
                                  "link", "meta", "param", "source", "track", "wbr", 0};
  return name_in(name, v);
}

/* Content is literal up to the matching close tag. */
static bool is_rawtext(const char *name)
{
  static const char *const r[] = {"script", "style", "title", "textarea", 0};
  return name_in(name, r);
}

/* CSS block-level elements; only used to close an open <p>. */
static bool is_block(const char *name)
{
  static const char *const b[] = {"address",  "article",    "aside",  "blockquote", "details", "div",    "dl",
                                  "fieldset", "figcaption", "figure", "footer",     "form",    "h1",     "h2",
                                  "h3",       "h4",         "h5",     "h6",         "header",  "hgroup", "hr",
                                  "main",     "menu",       "nav",    "ol",         "p",       "pre",    "section",
                                  "table",    "ul",         0};
  return name_in(name, b);
}

struct parser {
  const char *p, *end;
  struct dom_node *root;
  struct dom_node *cur; /* insertion point */
  int depth;
  bool oom;
};

static const char empty_tag[] = "";

static void node_init(struct dom_node *n, enum dom_type type)
{
  n->type = type;
  n->tag = (char *)empty_tag;
  n->qname = 0;
  n->text = 0;
  n->ns = 0;
  n->attrs = 0;
  n->last_attr = 0;
  for (int i = 0; i < 8; i++)
    n->attr_cache[i] = 0;
  n->attr_cache_seq = ~0u;
  n->attr_seq = 0;
  n->attr_cache_valid = 0;
  n->parent = n->first_child = n->last_child = n->next = 0;
  n->sibling_meta_seq = ~0u;
  n->elem_index = n->elem_count = n->type_index = n->type_count = 0;
  n->computed_style = 0;
  n->computed_style_sheet = 0;
  n->computed_style_seq = 0;
  n->pseudo_style[0] = n->pseudo_style[1] = 0;
  n->files = 0;
  n->pseudo_sheet = 0;
  n->pseudo_seq = DOM_STYLE_STALE;
  n->anim_rec = 0;
  n->anc_bloom_seq = 0;
  n->wrapper = 0;
  n->tokenlist[0] = n->tokenlist[1] = 0;
  n->shadow = n->host = 0;
  n->shadow_sheet = 0;
  n->shadow_sheet_key = 0;
  n->shadow_open = false;
}

/* lowercased tag; the original spelling is kept only when it differs */
static bool set_tag(struct dom_node *n, const char *name, int len)
{
  char *t = dupn_lower(name, len), *q = 0;
  if (!t)
    return false;
  if (memcmp(t, name, (unsigned)len) && !(q = dupn(name, len))) {
    free(t);
    return false;
  }
  n->tag = t;
  n->qname = q;
  return true;
}

static struct dom_node *node_new(struct parser *ps, enum dom_type type)
{
  struct dom_node *n = malloc(sizeof *n);
  if (!n) {
    ps->oom = true;
    return 0;
  }
  node_init(n, type);
  return n;
}

static void node_append(struct dom_node *parent, struct dom_node *child)
{
  child->parent = parent;
  child->next = 0;
  if (parent->last_child)
    parent->last_child->next = child;
  else
    parent->first_child = child;
  parent->last_child = child;
}

static bool attr_append(struct dom_node *el, char *name, char *value)
{
  struct dom_attr *a = malloc(sizeof *a);
  if (!a) {
    free(name);
    free(value);
    return false;
  }
  a->name = name;
  a->value = value;
  a->ns = 0;
  a->qname = 0;
  a->next = 0;
  if (el->last_attr)
    el->last_attr->next = a;
  else
    el->attrs = a;
  el->last_attr = a;
  return true;
}

/* Close cur up to and including the nearest ancestor in `targets`, unless a
 * `boundary` tag is hit first (then close nothing). */
static void close_to(struct parser *ps, const char *const *targets, const char *const *boundary)
{
  struct dom_node *n = ps->cur;
  int up = 0;
  while (n && n != ps->root) {
    if (name_in(n->tag, targets)) {
      while (up-- >= 0 && ps->cur != ps->root) {
        ps->cur = ps->cur->parent;
        ps->depth--;
      }
      return;
    }
    if (boundary && name_in(n->tag, boundary))
      return;
    n = n->parent;
    up++;
  }
}

/* Implied end tags triggered by a START tag, before it is inserted. */
static void auto_close(struct parser *ps, const char *name)
{
  if (streq(name, "li")) {
    static const char *const t[] = {"li", 0};
    static const char *const b[] = {"ul", "ol", "menu", 0};
    close_to(ps, t, b);
    return;
  }
  if (streq(name, "dt") || streq(name, "dd")) {
    static const char *const t[] = {"dt", "dd", 0};
    static const char *const b[] = {"dl", 0};
    close_to(ps, t, b);
    return;
  }
  if (streq(name, "option")) {
    static const char *const t[] = {"option", 0};
    static const char *const b[] = {"select", "datalist", "optgroup", 0};
    close_to(ps, t, b);
    return;
  }
  if (streq(name, "optgroup")) {
    static const char *const to[] = {"option", 0};
    static const char *const tg[] = {"optgroup", 0};
    static const char *const b[] = {"select", 0};
    close_to(ps, to, b);
    close_to(ps, tg, b);
    return;
  }
  if (streq(name, "tr")) {
    static const char *const t[] = {"tr", 0};
    static const char *const b[] = {"table", 0};
    close_to(ps, t, b);
    return;
  }
  if (streq(name, "td") || streq(name, "th")) {
    static const char *const t[] = {"td", "th", 0};
    static const char *const b[] = {"tr", "table", 0};
    close_to(ps, t, b);
    return;
  }
  if (streq(name, "thead") || streq(name, "tbody") || streq(name, "tfoot")) {
    static const char *const t[] = {"tr", "thead", "tbody", "tfoot", 0};
    static const char *const b[] = {"table", 0};
    close_to(ps, t, b);
    return;
  }
  if (is_block(name)) {
    static const char *const t[] = {"p", 0};
    static const char *const b[] = {"td", "th", "li", "button", 0};
    close_to(ps, t, b);
  }
}

/* s[0..n) == pre (case-insensitive) and followed by a tag-name terminator. */
static bool ci_close_match(const char *s, const char *end, const char *pre, int n)
{
  if (s + n > end)
    return false;
  for (int i = 0; i < n; i++)
    if (lower(s[i]) != lower(pre[i]))
      return false;
  char nx = (s + n < end) ? s[n] : '>';
  return nx == '>' || nx == '/' || is_space(nx);
}

static void handle_text(struct parser *ps)
{
  const char *start = ps->p;
  while (ps->p < ps->end && *ps->p != '<')
    ps->p++;
  int len = (int)(ps->p - start);
  if (len <= 0)
    return;
  struct dom_node *t = node_new(ps, DOM_TEXT);
  if (!t)
    return;
  t->text = dupn(start, len);
  if (!t->text) {
    free(t);
    ps->oom = true;
    return;
  }
  node_append(ps->cur, t);
}

static void emit_literal(struct parser *ps, char c)
{
  struct dom_node *t = node_new(ps, DOM_TEXT);
  if (!t)
    return;
  t->text = dupn(&c, 1);
  if (!t->text) {
    free(t);
    ps->oom = true;
    return;
  }
  node_append(ps->cur, t);
}

static void handle_tag(struct parser *ps)
{
  const char *p = ps->p, *end = ps->end;

  if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') { /* comment */
    const char *c = p + 4;
    while (c + 2 < end && !(c[0] == '-' && c[1] == '-' && c[2] == '>'))
      c++;
    ps->p = (c + 2 < end) ? c + 3 : end;
    return;
  }
  if (p + 1 < end && (p[1] == '!' || p[1] == '?')) { /* doctype/PI */
    const char *c = p + 2;
    while (c < end && *c != '>')
      c++;
    ps->p = (c < end) ? c + 1 : end;
    return;
  }

  bool closing = false;
  const char *q = p + 1;
  if (q < end && *q == '/') {
    closing = true;
    q++;
  }

  if (q >= end || !is_alpha(*q)) { /* '<' not starting a tag */
    emit_literal(ps, '<');
    ps->p = p + 1;
    return;
  }

  const char *name_start = q;
  while (q < end && is_name_char(*q))
    q++;
  int nl = (int)(q - name_start);

  if (closing) {
    while (q < end && *q != '>')
      q++;
    ps->p = (q < end) ? q + 1 : end;
    char *lname = dupn_lower(name_start, nl);
    const char *t[2] = {lname, 0};
    if (!lname) {
      ps->oom = true;
      return;
    }
    close_to(ps, t, 0);
    free(lname);
    return;
  }

  struct dom_node *el = node_new(ps, DOM_ELEMENT);
  if (!el) {
    while (q < end && *q != '>')
      q++;
    ps->p = (q < end) ? q + 1 : end;
    return;
  }
  if (!set_tag(el, name_start, nl)) {
    ps->oom = true;
    free(el);
    return;
  }

  bool self_close = false;
  while (q < end && *q != '>') {
    const char *guard = q;
    while (q < end && is_space(*q))
      q++;
    if (q < end && *q == '/') {
      self_close = true;
      q++;
      continue;
    }
    if (q >= end || *q == '>')
      break;

    const char *ns = q;
    while (q < end && *q != '=' && *q != '>' && *q != '/' && !is_space(*q))
      q++;
    int anl = (int)(q - ns);
    char *aname = (anl > 0) ? dupn_lower(ns, anl) : 0;

    while (q < end && is_space(*q))
      q++;
    char *aval = 0;
    if (q < end && *q == '=') {
      q++;
      while (q < end && is_space(*q))
        q++;
      if (q < end && (*q == '"' || *q == '\'')) {
        char quote = *q++;
        const char *vs = q;
        while (q < end && *q != quote)
          q++;
        aval = dupn(vs, (int)(q - vs));
        if (q < end)
          q++;
      } else {
        const char *vs = q;
        while (q < end && *q != '>' && !is_space(*q))
          q++;
        aval = dupn(vs, (int)(q - vs));
      }
    } else {
      aval = dupn("", 0);
    }

    if (aname && aval) {
      if (attr_append(el, aname, aval) && memcmp(aname, ns, (unsigned)anl))
        el->last_attr->qname = dupn(ns, anl);
    } else {
      if (aname)
        free(aname);
      if (aval)
        free(aval);
    }

    if (q == guard) {
      if (q < end)
        q++;
      else
        break;
    } /* never stall */
  }
  ps->p = (q < end) ? q + 1 : end;

  auto_close(ps, el->tag);
  node_append(ps->cur, el);

  if (is_void(el->tag) || self_close)
    return;

  if (is_rawtext(el->tag)) {
    const char *rs = ps->p, *r = ps->p;
    int tl = strlen(el->tag);
    while (r < end) {
      if (*r == '<' && r + 1 < end && r[1] == '/' && ci_close_match(r + 2, end, el->tag, tl))
        break;
      r++;
    }
    int rl = (int)(r - rs);
    if (rl > 0) {
      struct dom_node *t = node_new(ps, DOM_TEXT);
      if (t) {
        t->text = dupn(rs, rl);
        if (t->text)
          node_append(el, t);
        else {
          free(t);
          ps->oom = true;
        }
      }
    }
    if (r < end) {
      while (r < end && *r != '>')
        r++;
      if (r < end)
        r++;
    }
    ps->p = r;
    return;
  }

  if (ps->depth < MAX_DEPTH) { /* over the cap: keep el a leaf */
    ps->cur = el;
    ps->depth++;
  }
}

struct dom_node *dom_parse(const char *html, int len)
{
  if (!html || len < 0)
    len = 0;

  struct parser ps;
  ps.p = html;
  ps.end = html + len;
  ps.depth = 0;
  ps.oom = false;

  ps.root = malloc(sizeof *ps.root);
  if (!ps.root)
    return 0;
  node_init(ps.root, DOM_ELEMENT);
  if (!set_tag(ps.root, "#document", 9)) {
    free(ps.root);
    return 0;
  }
  ps.cur = ps.root;

  while (ps.p < ps.end && !ps.oom) {
    if (*ps.p == '<')
      handle_tag(&ps);
    else
      handle_text(&ps);
  }
  return ps.root;
}

/* Set by browser/js/bindings.c: detaches the JS wrapper of a freed node. NULL when no JS. */
void (*dom_on_free_wrapper)(void *wrapper);
void (*dom_on_free_style_cache)(void *cache);
void (*dom_on_free_anim)(struct dom_node *n);
void (*dom_on_mutate)(const struct dom_mutation *m);
bool dom_structural_styles;

/* Bumped by every tree-mutating API; the browser re-flattens only when it changes. */
unsigned dom_mutation_seq;

static const char *const known_ns[] = {DOM_NS_HTML,  DOM_NS_SVG, DOM_NS_MATHML, DOM_NS_XML, DOM_NS_XMLNS,
                                       DOM_NS_XLINK, ""};
static const char xml_doc_mark[] = "";

/* known URIs are shared literals; anything else is a heap copy */
static char *ns_dup(const char *ns)
{
  if (!ns)
    return 0;
  for (unsigned i = 0; i < sizeof known_ns / sizeof known_ns[0]; i++)
    if (!strcmp(ns, known_ns[i]))
      return (char *)known_ns[i];
  return dupn(ns, (int)strlen(ns));
}
static void ns_free(char *ns)
{
  if (!ns || ns == xml_doc_mark)
    return;
  for (unsigned i = 0; i < sizeof known_ns / sizeof known_ns[0]; i++)
    if (ns == known_ns[i])
      return;
  free(ns);
}

void (*dom_on_free_sheet)(void *sheet);

const struct dom_node *dom_flat_parent(const struct dom_node *n) { return n->parent ? n->parent : n->host; }

struct dom_node *dom_new_shadow_root(struct dom_node *host, bool open)
{
  if (host->shadow)
    return 0;
  struct dom_node *root = dom_new_element("#shadow-root");
  if (!root)
    return 0;
  root->host = host;
  root->shadow_open = open;
  host->shadow = root;
  dom_mutation_seq++;
  return root;
}

static const char *attr_or_empty(const struct dom_node *n, const char *name)
{
  const char *v = n->type == DOM_ELEMENT ? dom_get_attr(n, name) : 0;
  return v ? v : "";
}

const struct dom_node *dom_slot_host(const struct dom_node *slot)
{
  const struct dom_node *r = slot;
  while (r->parent)
    r = r->parent;
  return r->host;
}

/* First <slot name=`name`> in the shadow tree of `host`, in tree order. */
static struct dom_node *first_slot_named(const struct dom_node *host, const char *name)
{
  struct dom_node *n = host->shadow->first_child;
  while (n) {
    if (n->type == DOM_ELEMENT && streq(n->tag, "slot") && streq(attr_or_empty(n, "name"), name))
      return n;
    if (n->first_child) {
      n = n->first_child;
      continue;
    }
    while (n && !n->next)
      n = n->parent == host->shadow ? 0 : n->parent;
    if (n)
      n = n->next;
  }
  return 0;
}

bool dom_slot_accepts(const struct dom_node *slot, const struct dom_node *c)
{
  const struct dom_node *host = dom_slot_host(slot);
  if (!host || !host->shadow || c->parent != host)
    return false;
  const char *want = attr_or_empty(slot, "name");
  return streq(c->type == DOM_TEXT ? "" : attr_or_empty(c, "slot"), want) && first_slot_named(host, want) == slot;
}

struct dom_node *dom_assigned_slot(const struct dom_node *c)
{
  const struct dom_node *host = c->parent;
  if (!host || !host->shadow)
    return 0;
  return first_slot_named(host, c->type == DOM_TEXT ? "" : attr_or_empty(c, "slot"));
}

static void declarative_walk(struct dom_node *host, int depth)
{
  struct dom_node *next;
  for (struct dom_node *c = host->first_child; c; c = next) {
    const char *mode = c->type == DOM_ELEMENT && streq(c->tag, "template") ? dom_get_attr(c, "shadowrootmode") : 0;
    next = c->next;
    if (mode && (streq_ci(mode, "open") || streq_ci(mode, "closed")) && host->tag[0] != '#' && !host->shadow) {
      struct dom_node *sr = dom_new_shadow_root(host, streq_ci(mode, "open"));
      if (sr) {
        while (c->first_child) {
          struct dom_node *k = c->first_child;
          dom_remove_child(c, k);
          dom_append_child(sr, k);
        }
        dom_remove_child(host, c);
        dom_free(c);
        declarative_walk(sr, depth + 1);
        continue;
      }
    }
    if (c->type == DOM_ELEMENT && depth < 512)
      declarative_walk(c, depth + 1);
  }
}

void dom_attach_declarative_shadows(struct dom_node *root) { declarative_walk(root, 0); }

static void node_free_one(struct dom_node *n)
{
  if (n->shadow) {
    n->shadow->host = 0;
    dom_free(n->shadow);
  }
  if (n->shadow_sheet && dom_on_free_sheet)
    dom_on_free_sheet(n->shadow_sheet);
  if (n->wrapper && dom_on_free_wrapper)
    dom_on_free_wrapper(n->wrapper);
  if (dom_on_free_style_cache) {
    if (n->computed_style)
      dom_on_free_style_cache(n->computed_style);
    for (int i = 0; i < 2; i++)
      if (n->pseudo_style[i])
        dom_on_free_style_cache(n->pseudo_style[i]);
  }
  if (n->anim_rec && dom_on_free_anim)
    dom_on_free_anim(n);
  dom_files_free(n->files);
  struct dom_attr *a = n->attrs;
  while (a) {
    struct dom_attr *nx = a->next;
    free(a->name);
    free(a->value);
    ns_free(a->ns);
    free(a->qname);
    free(a);
    a = nx;
  }
  ns_free(n->ns);
  if (n->tag != empty_tag)
    free(n->tag);
  free(n->qname);
  if (n->text)
    free(n->text);
  free(n);
}

/* Iterative post-order teardown (no recursion). */
void dom_files_free(struct dom_files *f)
{
  if (!f)
    return;
  for (int i = 0; i < f->n; i++) {
    free(f->path[i]);
    free(f->rel[i]);
  }
  free(f->path);
  free(f->rel);
  free(f);
}

void dom_free(struct dom_node *root)
{
  struct dom_node *n = root;
  while (n) {
    if (n->first_child) {
      n = n->first_child;
      continue;
    }
    struct dom_node *next = n->next;
    struct dom_node *parent = n->parent;
    node_free_one(n);
    if (next) {
      n = next;
    } else if (parent) {
      parent->first_child = 0;
      n = parent;
    } else {
      n = 0;
    }
  }
}

const char *dom_get_attr(const struct dom_node *node, const char *name)
{
  static const char *const common[] = {"id", "class", "style", "type", "href", "src", "value", "name"};
  int slot = -1;
  if (!node || node->type != DOM_ELEMENT)
    return 0;
  for (int i = 0; i < (int)(sizeof common / sizeof common[0]); i++)
    if (streq_ci(name, common[i])) {
      slot = i;
      break;
    }
  if (slot >= 0) {
    struct dom_node *mutable = (struct dom_node *)node;
    unsigned bit = 1u << slot;
    if (mutable->attr_cache_seq != mutable->attr_seq) {
      mutable->attr_cache_valid = 0;
      mutable->attr_cache_seq = mutable->attr_seq;
    }
    if (mutable->attr_cache_valid & bit)
      return mutable->attr_cache[slot] ? mutable->attr_cache[slot]->value : 0;
    mutable->attr_cache_valid |= (unsigned char)bit;
  }
  for (struct dom_attr *a = node->attrs; a; a = a->next)
    if (streq_ci(a->name, name)) {
      if (slot >= 0)
        ((struct dom_node *)node)->attr_cache[slot] = a;
      return a->value;
    }
  return 0;
}

/* Attributes that can change a computed style. */
static bool style_attr(const char *name)
{
  static const char *const names[] = {"id",       "class",  "style",   "color",        "bgcolor",
                                      "width",    "height", "checked", "selected",     "disabled",
                                      "required", "href",   "open",    "popover-open", "dialog-modal"};
  for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
    if (streq_ci(name, names[i]))
      return true;
  return false;
}

static void style_stale_subtree(struct dom_node *root)
{
  struct dom_node *n = root;
  while (n) {
    n->computed_style_seq = DOM_STYLE_STALE;
    n->pseudo_seq = DOM_STYLE_STALE;
    if (n->first_child) {
      n = n->first_child;
      continue;
    }
    while (n != root && !n->next)
      n = n->parent;
    if (n == root)
      break;
    n = n->next;
  }
}

/* Mutation API for the JS DOM bindings (browser/js/bindings.c). */

struct dom_node *dom_new_element(const char *tag)
{
  struct dom_node *n = malloc(sizeof *n);
  if (!n)
    return 0;
  node_init(n, DOM_ELEMENT);
  if (!set_tag(n, tag ? tag : "", tag ? (int)strlen(tag) : 0)) {
    free(n);
    return 0;
  }
  return n;
}

struct dom_node *dom_new_text(const char *text)
{
  struct dom_node *n = malloc(sizeof *n);
  if (!n)
    return 0;
  node_init(n, DOM_TEXT);
  n->text = dupn(text ? text : "", text ? (int)strlen(text) : 0);
  return n;
}

static struct dom_node *sib_before(const struct dom_node *c)
{
  struct dom_node *p = c->parent ? c->parent->first_child : 0;
  while (p && p->next != c)
    p = p->next;
  return p;
}

static void report_children(struct dom_node *parent, struct dom_node *added, struct dom_node *removed,
                            struct dom_node *prev, struct dom_node *next)
{
  struct dom_mutation m = {DOM_MUT_CHILDREN, parent, added, removed, prev, next, 0, 0};
  dom_on_mutate(&m);
}

void dom_set_text(struct dom_node *n, const char *text)
{
  char *nt = dupn(text ? text : "", text ? (int)strlen(text) : 0);
  if (!n || !nt) {
    free(nt);
    return;
  }
  if (dom_on_mutate) {
    struct dom_mutation m = {DOM_MUT_TEXT, n, 0, 0, 0, 0, 0, n->text};
    dom_on_mutate(&m);
  }
  free(n->text);
  n->text = nt;
  dom_mutation_seq++;
}

void dom_remove_child(struct dom_node *parent, struct dom_node *child)
{
  if (!parent || !child)
    return;
  dom_mutation_seq++;
  struct dom_node *prev = 0, *c = parent->first_child, *after;
  while (c && c != child) {
    prev = c;
    c = c->next;
  }
  if (!c)
    return;
  if (prev)
    prev->next = c->next;
  else
    parent->first_child = c->next;
  if (parent->last_child == c)
    parent->last_child = prev;
  after = c->next;
  c->next = 0;
  c->parent = 0;
  if (dom_on_mutate)
    report_children(parent, 0, c, prev, after);
  if (dom_structural_styles)
    style_stale_subtree(parent);
}

bool dom_is_ancestor(const struct dom_node *anc, const struct dom_node *n)
{
  for (; n; n = n->parent)
    if (n == anc)
      return true;
  return false;
}

static struct dom_node *clone_one(const struct dom_node *src)
{
  struct dom_node *n = malloc(sizeof *n);
  if (!n)
    return 0;
  node_init(n, src->type);
  if (src->tag != empty_tag && !set_tag(n, src->tag, (int)strlen(src->tag))) {
    free(n);
    return 0;
  }
  if (src->qname && !(n->qname = dupn(src->qname, (int)strlen(src->qname)))) {
    dom_free(n);
    return 0;
  }
  n->ns = src->ns == xml_doc_mark ? 0 : ns_dup(src->ns);
  if (src->text && !(n->text = dupn(src->text, (int)strlen(src->text)))) {
    free(n);
    return 0;
  }
  for (const struct dom_attr *a = src->attrs; a; a = a->next) {
    char *nn = dupn(a->name, (int)strlen(a->name)),
         *nv = dupn(a->value ? a->value : "", a->value ? (int)strlen(a->value) : 0);
    if (!nn || !nv) {
      free(nn);
      free(nv);
      dom_free(n);
      return 0;
    }
    if (!attr_append(n, nn, nv)) {
      dom_free(n);
      return 0;
    }
    n->last_attr->ns = ns_dup(a->ns);
    if (a->qname)
      n->last_attr->qname = dupn(a->qname, (int)strlen(a->qname));
    n->attr_seq++;
  }
  return n;
}

struct dom_node *dom_clone(const struct dom_node *src, bool deep)
{
  struct dom_node *root = clone_one(src), *dst = root, *c;
  const struct dom_node *s = src;
  if (!root || !deep)
    return root;
  for (;;) {
    if (s->first_child) {
      if (!(c = clone_one(s->first_child)))
        break;
      node_append(dst, c);
      s = s->first_child;
      dst = c;
      continue;
    }
    while (s != src && !s->next) {
      s = s->parent;
      dst = dst->parent;
    }
    if (s == src)
      return root;
    if (!(c = clone_one(s->next)))
      break;
    node_append(dst->parent, c);
    s = s->next;
    dst = c;
  }
  dom_free(root);
  return 0;
}

void dom_append_child(struct dom_node *parent, struct dom_node *child)
{
  if (!parent || !child || dom_is_ancestor(child, parent))
    return;
  dom_mutation_seq++;
  if (child->parent)
    dom_remove_child(child->parent, child);
  node_append(parent, child);
  if (dom_on_mutate)
    report_children(parent, child, 0, sib_before(child), 0);
  style_stale_subtree(child);
  if (dom_structural_styles)
    style_stale_subtree(parent);
}

/* Insert `child` before `ref`; a NULL or foreign `ref` appends (Node.insertBefore). */
void dom_insert_before(struct dom_node *parent, struct dom_node *child, struct dom_node *ref)
{
  if (!parent || !child || ref == child || dom_is_ancestor(child, parent))
    return;
  if (!ref || ref->parent != parent) {
    dom_append_child(parent, child);
    return;
  }
  dom_mutation_seq++;
  if (child->parent)
    dom_remove_child(child->parent, child);
  child->parent = parent;
  {
    struct dom_node *prev = 0, *c = parent->first_child;
    while (c && c != ref) {
      prev = c;
      c = c->next;
    }
    child->next = ref;
    if (prev)
      prev->next = child;
    else
      parent->first_child = child;
  }
  if (dom_on_mutate)
    report_children(parent, child, 0, sib_before(child), ref);
  style_stale_subtree(child);
  if (dom_structural_styles)
    style_stale_subtree(parent);
}

void dom_set_attr(struct dom_node *el, const char *name, const char *value)
{
  if (!el || el->type != DOM_ELEMENT || !name)
    return;
  dom_mutation_seq++;
  if (dom_on_mutate) {
    struct dom_mutation m = {DOM_MUT_ATTR, el, 0, 0, 0, 0, name, dom_get_attr(el, name)};
    dom_on_mutate(&m);
  }
  if (style_attr(name))
    style_stale_subtree(el);
  for (struct dom_attr *a = el->attrs; a; a = a->next)
    if (streq_ci(a->name, name)) {
      char *nv = dupn(value ? value : "", value ? (int)strlen(value) : 0);
      if (nv) {
        free(a->value);
        a->value = nv;
        el->attr_seq++;
      }
      return;
    }
  {
    char *nn = dupn_lower(name, (int)strlen(name));
    char *nv = dupn(value ? value : "", value ? (int)strlen(value) : 0);
    if (nn && nv) {
      if (attr_append(el, nn, nv)) {
        if (memcmp(nn, name, strlen(name)) && strcmp(dom_ns(el), DOM_NS_HTML))
          el->last_attr->qname = dupn(name, (int)strlen(name));
        el->attr_seq++;
      }
    } else {
      if (nn)
        free(nn);
      if (nv)
        free(nv);
    }
  }
}

void dom_remove_attr(struct dom_node *el, const char *name)
{
  if (!el)
    return;
  dom_mutation_seq++;
  if (dom_on_mutate && dom_get_attr(el, name)) {
    struct dom_mutation m = {DOM_MUT_ATTR, el, 0, 0, 0, 0, name, dom_get_attr(el, name)};
    dom_on_mutate(&m);
  }
  if (style_attr(name))
    style_stale_subtree(el);
  struct dom_attr *prev = 0, *a = el->attrs;
  while (a) {
    if (streq_ci(a->name, name)) {
      if (prev)
        prev->next = a->next;
      else
        el->attrs = a->next;
      if (el->last_attr == a)
        el->last_attr = prev;
      free(a->name);
      free(a->value);
      free(a);
      el->attr_seq++;
      return;
    }
    prev = a;
    a = a->next;
  }
}

/* Detach and free every child subtree, unlinking each first. */
void dom_clear_children(struct dom_node *el)
{
  if (!el)
    return;
  dom_mutation_seq++;
  if (dom_on_mutate && el->first_child) {
    struct dom_mutation m = {DOM_MUT_CLEAR, el, 0, el->first_child, 0, 0, 0, 0};
    dom_on_mutate(&m);
  }
  if (dom_structural_styles) {
    el->computed_style_seq = DOM_STYLE_STALE;
    el->pseudo_seq = DOM_STYLE_STALE;
  }
  struct dom_node *c = el->first_child;
  el->first_child = el->last_child = 0;
  while (c) {
    struct dom_node *nx = c->next;
    c->next = 0;
    c->parent = 0;
    dom_free(c);
    c = nx;
  }
}

/* ------------------------------------------------------- namespaces ------- */

const char *dom_qname(const struct dom_node *n)
{
  return n->qname && strcmp(dom_ns(n), DOM_NS_HTML) ? n->qname : n->tag;
}
const char *dom_attr_qname(const struct dom_node *el, const struct dom_attr *a)
{
  return a->qname && strcmp(dom_ns(el), DOM_NS_HTML) ? a->qname : a->name;
}

int dom_prefix_len(const char *q)
{
  const char *c = strchr(q, ':');
  return c ? (int)(c - q) : 0;
}
const char *dom_local_name(const char *q)
{
  int n = dom_prefix_len(q);
  return n ? q + n + 1 : q;
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
static bool is_xml_doc(const struct dom_node *n)
{
  while (n->parent)
    n = n->parent;
  return n->ns == xml_doc_mark;
}
void dom_mark_xml_doc(struct dom_node *root)
{
  ns_free(root->ns);
  root->ns = (char *)xml_doc_mark;
}

/* The namespace `prefix` (plen bytes, 0 = default) is bound to at `el`, from xmlns attributes */
const char *dom_lookup_ns(const struct dom_node *el, const char *prefix, int plen)
{
  for (const struct dom_node *e = el; e && e->type == DOM_ELEMENT && e->tag[0] != '#'; e = e->parent)
    for (const struct dom_attr *a = e->attrs; a; a = a->next) {
      if (plen ? (!ncmp(a->name, "xmlns:", 6) && (int)strlen(a->name + 6) == plen && !ncmp(a->name + 6, prefix, plen))
               : !strcmp(a->name, "xmlns"))
        return a->value ? a->value : "";
    }
  return "";
}

const char *dom_ns(const struct dom_node *n)
{
  const struct dom_node *p = n;
  int xml = is_xml_doc(n);
  for (;;) {
    int pl = dom_prefix_len(p->tag);
    if (p->ns && p->ns != xml_doc_mark)
      return p->ns;
    if (pl) {
      const char *u = dom_lookup_ns(p, p->tag, pl);
      if (*u)
        return u;
    } else if (!strcmp(p->tag, "svg"))
      return DOM_NS_SVG;
    else if (!strcmp(p->tag, "math"))
      return DOM_NS_MATHML;
    else if (xml) {
      const char *u = dom_lookup_ns(p, "", 0);
      if (*u)
        return u;
    }
    p = p->parent;
    if (!p || p->tag[0] == '#' || (!xml && !strcmp(p->tag, "foreignobject")))
      return xml ? "" : DOM_NS_HTML;
  }
}

const char *dom_attr_ns(const struct dom_attr *a)
{
  int pl;
  if (a->ns)
    return a->ns;
  if (!strcmp(a->name, "xmlns"))
    return DOM_NS_XMLNS;
  pl = dom_prefix_len(a->name);
  if (pl == 5 && !ncmp(a->name, "xmlns", 5))
    return DOM_NS_XMLNS;
  if (pl == 3 && !ncmp(a->name, "xml", 3))
    return DOM_NS_XML;
  if (pl == 5 && !ncmp(a->name, "xlink", 5))
    return DOM_NS_XLINK;
  return "";
}

const char *dom_scope_ns(const struct dom_node *el, const char *prefix, int plen)
{
  for (const struct dom_node *e = el; e && e->type == DOM_ELEMENT && e->tag[0] != '#'; e = e->parent) {
    if (dom_prefix_len(e->tag) == plen && !ncmp(e->tag, prefix, plen) && *dom_ns(e))
      return dom_ns(e);
    for (const struct dom_attr *a = e->attrs; a; a = a->next)
      if (plen ? (!ncmp(a->name, "xmlns:", 6) && !ncmp(a->name + 6, prefix, plen) && !a->name[6 + plen])
               : !strcmp(a->name, "xmlns"))
        return a->value ? a->value : "";
  }
  return "";
}

void dom_set_ns(struct dom_node *el, const char *ns)
{
  char *c = ns_dup(ns);
  if (ns && !c)
    return;
  ns_free(el->ns);
  el->ns = c;
}

struct dom_attr *dom_find_attr_ns(const struct dom_node *el, const char *ns, const char *local)
{
  for (struct dom_attr *a = el->attrs; a; a = a->next)
    if (!strcmp(dom_attr_ns(a), ns ? ns : "") && !strcmp(dom_local_name(a->name), local))
      return a;
  return 0;
}

void dom_set_attr_ns(struct dom_node *el, const char *ns, const char *qname, const char *value)
{
  struct dom_attr *a = dom_find_attr_ns(el, ns, dom_local_name(qname));
  char *c;
  if (!a) {
    dom_set_attr(el, qname, value);
    for (a = el->attrs; a && !streq_ci(a->name, qname); a = a->next)
      ;
    if (a && !a->qname && memcmp(a->name, qname, strlen(qname)))
      a->qname = dupn(qname, (int)strlen(qname));
  } else {
    dom_set_attr(el, a->name, value);
  }
  if (!a)
    return;
  c = ns_dup(ns ? ns : "");
  if (c) {
    ns_free(a->ns);
    a->ns = c;
  }
}
