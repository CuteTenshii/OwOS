/* dom.h — HTML DOM tree for the browser. dom_parse() builds a node tree on the
 * heap; html.c walks it to render. */
#ifndef OWOS_DOM_H
#define OWOS_DOM_H

#include "owos.h" /* lower/starts_ci/streq_ci/contains_ci case-insensitive helpers */

enum dom_type { DOM_ELEMENT, DOM_TEXT };

struct dom_attr {
  char *name;  /* lowercased                           */
  char *value; /* raw (entities not decoded), "" if none */
  struct dom_attr *next;
};

struct dom_node {
  enum dom_type type;
  char tag[16]; /* lowercased tag name (element)        */
  char *text;   /* character data (text node)           */
  struct dom_attr *attrs;
  struct dom_attr *last_attr; /* tail of attrs, for O(1) append       */
  struct dom_attr *attr_cache[8];
  unsigned attr_cache_seq;
  struct dom_node *parent;
  struct dom_node *first_child;
  struct dom_node *last_child;
  struct dom_node *next; /* sibling                              */
  unsigned sibling_meta_seq;
  int elem_index;
  int elem_count;
  int type_index;
  int type_count;
  void *computed_style;
  void *computed_style_sheet;
  unsigned computed_style_seq;
  void *wrapper;         /* cached JS wrapper object (browser/js.c),
                          * NULL until first exposed to script — gives
                          * a node stable identity so listeners stick. */
};

/* Parse `len` bytes into a tree under a synthetic "#document" root. Returns
 * NULL only on immediate OOM; a mid-parse OOM returns the partial tree. */
struct dom_node *dom_parse(const char *html, int len);
void dom_free(struct dom_node *root);
const char *dom_get_attr(const struct dom_node *node, const char *name);

/* Mutation API used by the JS DOM bindings (browser/js.c). Nodes are plain
 * malloc'd cells; appended nodes are owned by the tree and freed by dom_free.
 */
struct dom_node *dom_new_element(const char *tag);
struct dom_node *dom_new_text(const char *text);
void dom_append_child(struct dom_node *parent, struct dom_node *child);
void dom_insert_before(struct dom_node *parent, struct dom_node *child, struct dom_node *ref);
void dom_remove_child(struct dom_node *parent, struct dom_node *child);
void dom_set_attr(struct dom_node *el, const char *name, const char *value);
void dom_remove_attr(struct dom_node *el, const char *name);
void dom_clear_children(struct dom_node *el);

/* Hook the JS layer installs so a freed node detaches its wrapper (see dom.c).
 */
extern void (*dom_on_free_wrapper)(void *wrapper);
extern void (*dom_on_free_style_cache)(void *cache);

/* Bumped on every tree mutation; the browser re-flattens only when it changes.
 */
extern unsigned dom_mutation_seq;

#endif /* OWOS_DOM_H */
