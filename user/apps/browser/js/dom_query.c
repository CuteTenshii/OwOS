#include <stdbool.h>

#include "css.h"
#include "dom_query.h"

/* Pre-order successor of `n` that stays inside `root`'s subtree */
static struct dom_node *next_after(struct dom_node *n, struct dom_node *root)
{
  while (n && n != root && !n->next)
    n = n->parent;
  return n && n != root ? n->next : 0;
}

static bool match_id(struct dom_node *n, const char *v)
{
  const char *a = dom_get_attr(n, "id");
  return a && streq(a, v);
}

static bool match_tag(struct dom_node *n, const char *v) { return streq(v, "*") || streq(n->tag, v); }

typedef bool (*match_fn)(struct dom_node *, const char *);

static struct dom_node *find_first(struct dom_node *n, match_fn match, const char *value)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT && match(c, value))
      return c;
    struct dom_node *found = find_first(c, match, value);
    if (found)
      return found;
  }
  return 0;
}

static void find_all(struct dom_node *n, match_fn match, const char *value, js_object *out)
{
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    if (c->type == DOM_ELEMENT && match(c, value))
      js_array_push(g_ctx, out, wrap_node(c));
    find_all(c, match, value, out);
  }
}

struct dom_node *js_dom_find_selector(struct dom_node *root, const char *selector)
{
  struct sel_list *l = css_sel_parse(selector);
  struct dom_node *found = 0;
  for (struct dom_node *n = root ? root->first_child : 0; n && !found;
       n = n->first_child ? n->first_child : next_after(n, root))
    if (n->type == DOM_ELEMENT && css_sel_match(l, n))
      found = n;
  css_sel_free(l);
  return found;
}

struct dom_node *js_dom_find_tag(struct dom_node *root, const char *tag)
{
  return root ? find_first(root, match_tag, tag) : 0;
}

struct dom_node *js_dom_find_id(struct dom_node *root, const char *id)
{
  return root ? find_first(root, match_id, id) : 0;
}

void js_dom_find_all_tag(struct dom_node *root, const char *tag, js_object *out)
{
  if (root)
    find_all(root, match_tag, tag, out);
}

void js_dom_query_all(struct dom_node *root, const char *selector, js_object *out)
{
  struct sel_list *l = css_sel_parse(selector);
  for (struct dom_node *n = root ? root->first_child : 0; n; n = n->first_child ? n->first_child : next_after(n, root))
    if (n->type == DOM_ELEMENT && css_sel_match(l, n))
      js_array_push(g_ctx, out, wrap_node(n));
  css_sel_free(l);
}

int js_dom_matches(struct dom_node *node, const char *selector)
{
  struct sel_list *l;
  int r;
  if (!node || node->type != DOM_ELEMENT)
    return 0;
  l = css_sel_parse(selector);
  r = css_sel_match(l, node);
  css_sel_free(l);
  return r;
}
