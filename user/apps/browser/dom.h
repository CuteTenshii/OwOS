/* dom.h: HTML DOM tree built by dom_parse() and walked by html.c. */
#ifndef OWOS_DOM_H
#define OWOS_DOM_H

#include "owos.h" /* lower/starts_ci/streq_ci/contains_ci case-insensitive helpers */
#include <stdbool.h>

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
  unsigned attr_seq;
  unsigned char attr_cache_valid;
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
  unsigned computed_style_seq; /* style-cache key, DOM_STYLE_STALE after a relevant mutation */
  void *pseudo_style[2];       /* cached ::before/::after style (NULL = no content) */
  void *pseudo_sheet;
  unsigned pseudo_seq;
  void *anim_rec;        /* animation/transition record (features/anim), or NULL */
  unsigned anc_bloom[4]; /* hashes of the ancestors' tag/id/classes (css/engine.c) */
  unsigned anc_bloom_seq;
  void *wrapper; /* cached JS wrapper (browser/js/bindings.c); NULL until exposed to script */
};

/* Parse `len` bytes under a synthetic "#document" root. NULL only on immediate OOM. */
struct dom_node *dom_parse(const char *html, int len);
void dom_free(struct dom_node *root);
const char *dom_get_attr(const struct dom_node *node, const char *name);

/* Mutation API for the JS DOM bindings; appended nodes are owned by the tree. */
struct dom_node *dom_new_element(const char *tag);
struct dom_node *dom_new_text(const char *text);
void dom_append_child(struct dom_node *parent, struct dom_node *child);
void dom_insert_before(struct dom_node *parent, struct dom_node *child, struct dom_node *ref);
void dom_remove_child(struct dom_node *parent, struct dom_node *child);
void dom_set_attr(struct dom_node *el, const char *name, const char *value);
void dom_remove_attr(struct dom_node *el, const char *name);
void dom_clear_children(struct dom_node *el);

/* computed_style_seq / pseudo_seq value after a mutation made the cache unusable. */
#define DOM_STYLE_STALE (~0u)

/* Set once a loaded rule uses a structural pseudo-class (:nth-*, :empty, ...). */
extern bool dom_structural_styles;

/* Installed by features/anim: drops the record of a node that is being freed. */
extern void (*dom_on_free_anim)(struct dom_node *n);

/* Hook the JS layer installs so a freed node detaches its wrapper. */
extern void (*dom_on_free_wrapper)(void *wrapper);
extern void (*dom_on_free_style_cache)(void *cache);

/* Bumped on every tree mutation. */
extern unsigned dom_mutation_seq;

#endif /* OWOS_DOM_H */
