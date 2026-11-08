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

static void node_init(struct dom_node *n, enum dom_type type)
{
  n->type = type;
  n->tag[0] = '\0';
  n->text = 0;
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
  n->pseudo_sheet = 0;
  n->pseudo_seq = DOM_STYLE_STALE;
  n->anim_rec = 0;
  n->anc_bloom_seq = 0;
  n->wrapper = 0;
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

  char name[16];
  int nl = 0;
  while (q < end && is_name_char(*q)) {
    if (nl < 15)
      name[nl++] = lower(*q);
    q++;
  }
  name[nl] = '\0';

  if (closing) {
    while (q < end && *q != '>')
      q++;
    ps->p = (q < end) ? q + 1 : end;
    const char *t[2] = {name, 0};
    close_to(ps, t, 0);
    return;
  }

  struct dom_node *el = node_new(ps, DOM_ELEMENT);
  if (!el) {
    while (q < end && *q != '>')
      q++;
    ps->p = (q < end) ? q + 1 : end;
    return;
  }
  {
    int i = 0;
    for (; name[i]; i++)
      el->tag[i] = name[i];
    el->tag[i] = '\0';
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

    if (aname && aval)
      attr_append(el, aname, aval);
    else {
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
  {
    const char *r = "#document";
    int i = 0;
    for (; r[i] && i < 15; i++)
      ps.root->tag[i] = r[i];
    ps.root->tag[i] = '\0';
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
bool dom_structural_styles;

/* Bumped by every tree-mutating API; the browser re-flattens only when it changes. */
unsigned dom_mutation_seq;

static void node_free_one(struct dom_node *n)
{
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
  struct dom_attr *a = n->attrs;
  while (a) {
    struct dom_attr *nx = a->next;
    free(a->name);
    free(a->value);
    free(a);
    a = nx;
  }
  if (n->text)
    free(n->text);
  free(n);
}

/* Iterative post-order teardown (no recursion). */
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
  {
    int i = 0;
    for (; tag && tag[i] && i < 15; i++)
      n->tag[i] = lower(tag[i]);
    n->tag[i] = '\0';
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

void dom_remove_child(struct dom_node *parent, struct dom_node *child)
{
  if (!parent || !child)
    return;
  dom_mutation_seq++;
  struct dom_node *prev = 0, *c = parent->first_child;
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
  c->next = 0;
  c->parent = 0;
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
  memcpy(n->tag, src->tag, sizeof n->tag);
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
  style_stale_subtree(child);
  if (dom_structural_styles)
    style_stale_subtree(parent);
}

void dom_set_attr(struct dom_node *el, const char *name, const char *value)
{
  if (!el || el->type != DOM_ELEMENT || !name)
    return;
  dom_mutation_seq++;
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
      if (attr_append(el, nn, nv))
        el->attr_seq++;
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
