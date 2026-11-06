#include "dom_query.h"

enum query_kind { QUERY_TAG, QUERY_ID, QUERY_CLASS };

static int match_id(struct dom_node *n, const char *v)
{
  const char *a = dom_get_attr(n, "id");
  return a && streq(a, v);
}

static int match_tag(struct dom_node *n, const char *v) { return streq(v, "*") || streq(n->tag, v); }

static int match_class(struct dom_node *n, const char *v)
{
  const char *a = dom_get_attr(n, "class");
  int vl = strlen(v);
  if (!a)
    return 0;
  while (*a) {
    const char *s;
    while (*a == ' ' || *a == '\t' || *a == '\n')
      a++;
    s = a;
    while (*a && *a != ' ' && *a != '\t' && *a != '\n')
      a++;
    if (a - s == vl && !memcmp(s, v, (unsigned)vl))
      return 1;
  }
  return 0;
}

typedef int (*match_fn)(struct dom_node *, const char *);

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

static match_fn parse_selector(const char *selector, char *value, int cap)
{
  char kind = 0;
  int i = 0;
  while (*selector == ' ')
    selector++;
  if (*selector == '#' || *selector == '.')
    kind = *selector++;
  for (; selector[i] && selector[i] != ' ' && selector[i] != '>' && i < cap - 1; i++)
    value[i] = kind ? selector[i] : lower(selector[i]);
  value[i] = '\0';
  return kind == '#' ? match_id : kind == '.' ? match_class : match_tag;
}

struct dom_node *js_dom_find_selector(struct dom_node *root, const char *selector)
{
  char value[64];
  match_fn match = parse_selector(selector, value, sizeof value);
  return root ? find_first(root, match, value) : 0;
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
  char value[64];
  match_fn match = parse_selector(selector, value, sizeof value);
  if (root)
    find_all(root, match, value, out);
}

int js_dom_matches(struct dom_node *node, const char *selector)
{
  char value[64];
  match_fn match;
  if (!node || node->type != DOM_ELEMENT)
    return 0;
  match = parse_selector(selector, value, sizeof value);
  return match(node, value);
}
