#include "sibling.h"

static void sibling_meta(const struct dom_node *node)
{
  if (!node || !node->parent || node->sibling_meta_seq == dom_mutation_seq)
    return;

  struct {
    char tag[16];
    int count;
  } types[64];
  int ntypes = 0, elements = 0;
  for (struct dom_node *c = node->parent->first_child; c; c = c->next) {
    c->sibling_meta_seq = dom_mutation_seq;
    c->elem_index = c->elem_count = c->type_index = c->type_count = 0;
    if (c->type != DOM_ELEMENT)
      continue;
    c->elem_index = ++elements;
    int ti = -1;
    for (int i = 0; i < ntypes; i++)
      if (streq(types[i].tag, c->tag)) {
        ti = i;
        break;
      }
    if (ti < 0 && ntypes < (int)(sizeof types / sizeof types[0])) {
      ti = ntypes++;
      int i = 0;
      while (c->tag[i] && i < (int)sizeof types[ti].tag - 1) {
        types[ti].tag[i] = c->tag[i];
        i++;
      }
      types[ti].tag[i] = '\0';
      types[ti].count = 0;
    }
    if (ti >= 0)
      c->type_index = ++types[ti].count;
  }
  for (struct dom_node *c = node->parent->first_child; c; c = c->next) {
    c->elem_count = elements;
    if (c->type != DOM_ELEMENT)
      continue;
    for (int i = 0; i < ntypes; i++)
      if (streq(types[i].tag, c->tag)) {
        c->type_count = types[i].count;
        break;
      }
  }
}

int css_elem_index(const struct dom_node *node)
{
  sibling_meta(node);
  return node && node->parent ? node->elem_index : 1;
}

int css_elem_count(const struct dom_node *node)
{
  sibling_meta(node);
  return node && node->parent ? node->elem_count : 1;
}

int css_type_index(const struct dom_node *node)
{
  sibling_meta(node);
  return node && node->parent ? node->type_index : 1;
}

int css_type_count(const struct dom_node *node)
{
  sibling_meta(node);
  return node && node->parent ? node->type_count : 1;
}
