/* dom.h: HTML DOM tree built by dom_parse() and walked by html.c. */
#ifndef OWOS_DOM_H
#define OWOS_DOM_H

#include "owos.h" /* lower/starts_ci/streq_ci/contains_ci case-insensitive helpers */
#include <stdbool.h>

enum dom_type { DOM_ELEMENT, DOM_TEXT };

struct dom_attr {
  char *name;  /* lowercased                           */
  char *value; /* raw (entities not decoded), "" if none */
  char *qname; /* original-case name when it differs from name, else NULL */
  char *ns;    /* explicit namespace URI ("" = none), NULL = derived from the prefix */
  struct dom_attr *next;
};

struct dom_node {
  enum dom_type type;
  char *tag;   /* lowercased tag name (element), "" for text   */
  char *qname; /* original-case name when it differs from tag, else NULL */
  char *text;  /* character data (text node)           */
  char *ns;    /* explicit namespace URI ("" = none), NULL = derived; see dom_ns() */
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
  void *wrapper;           /* cached JS wrapper (browser/js/bindings.c); NULL until exposed to script */
  struct dom_node *shadow; /* element: its attached shadow root (detached tree, tag "#shadow-root") */
  struct dom_node *host;   /* shadow root: the element it is attached to */
  void *shadow_sheet;      /* shadow root: the rules of its <style> elements (css_sheet *) */
  unsigned shadow_sheet_key;
  bool shadow_open; /* shadow root: mode "open" */
};

/* Parse `len` bytes under a synthetic "#document" root. NULL only on immediate OOM. */
struct dom_node *dom_parse(const char *html, int len);
void dom_free(struct dom_node *root);
const char *dom_get_attr(const struct dom_node *node, const char *name);

/* Namespaces. Parsed elements derive theirs (svg/math subtrees, prefixes via xmlns:p); script-made
 * ones carry an explicit URI. "" means no namespace. */
#define DOM_NS_HTML "http://www.w3.org/1999/xhtml"
#define DOM_NS_SVG "http://www.w3.org/2000/svg"
#define DOM_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define DOM_NS_XML "http://www.w3.org/XML/1998/namespace"
#define DOM_NS_XMLNS "http://www.w3.org/2000/xmlns/"
#define DOM_NS_XLINK "http://www.w3.org/1999/xlink"
const char *dom_qname(const struct dom_node *el); /* name to show: tag for HTML, original case otherwise */
const char *dom_attr_qname(const struct dom_node *el, const struct dom_attr *a);
const char *dom_ns(const struct dom_node *el);
const char *dom_attr_ns(const struct dom_attr *a);
const char *dom_local_name(const char *qname); /* part after the prefix */
int dom_prefix_len(const char *qname);         /* length of the prefix, 0 if none */
const char *dom_lookup_ns(const struct dom_node *el, const char *prefix, int plen); /* "" if undeclared */
const char *dom_scope_ns(const struct dom_node *el, const char *prefix, int plen);  /* binding in scope, "" if none */
void dom_set_ns(struct dom_node *el, const char *ns);
void dom_mark_xml_doc(struct dom_node *root); /* unprefixed xmlns attributes apply under this root */
struct dom_attr *dom_find_attr_ns(const struct dom_node *el, const char *ns, const char *local);
void dom_set_attr_ns(struct dom_node *el, const char *ns, const char *qname, const char *value);

/* Mutation API for the JS DOM bindings; appended nodes are owned by the tree. */
struct dom_node *dom_new_element(const char *tag);
struct dom_node *dom_new_text(const char *text);
void dom_append_child(struct dom_node *parent, struct dom_node *child);
void dom_insert_before(struct dom_node *parent, struct dom_node *child, struct dom_node *ref);
void dom_remove_child(struct dom_node *parent, struct dom_node *child);
void dom_set_attr(struct dom_node *el, const char *name, const char *value);
void dom_remove_attr(struct dom_node *el, const char *name);
void dom_clear_children(struct dom_node *el);
struct dom_node *dom_clone(const struct dom_node *src, bool deep);          /* detached copy, NULL on OOM */
bool dom_is_ancestor(const struct dom_node *anc, const struct dom_node *n); /* anc == n counts */

/* computed_style_seq / pseudo_seq value after a mutation made the cache unusable. */
#define DOM_STYLE_STALE (~0u)

/* Set once a loaded rule uses a structural pseudo-class (:nth-*, :empty, ...). */
extern bool dom_structural_styles;

/* Installed by features/anim: drops the record of a node that is being freed. */
extern void (*dom_on_free_anim)(struct dom_node *n);

/* Parent in the shadow-including tree: a shadow root's parent is its host. */
const struct dom_node *dom_flat_parent(const struct dom_node *n);
struct dom_node *dom_new_shadow_root(struct dom_node *host, bool open); /* NULL if `host` already has one */
extern void (*dom_on_free_sheet)(void *sheet);

/* Named slot assignment. A slot lives in a shadow tree; its assigned nodes are children of that tree's host. */
const struct dom_node *dom_slot_host(const struct dom_node *slot); /* host of the shadow tree holding `slot` */
bool dom_slot_accepts(const struct dom_node *slot,
                      const struct dom_node *c);              /* c (a host child) is slotted in `slot` */
struct dom_node *dom_assigned_slot(const struct dom_node *c); /* slot `c` is assigned to, or NULL */
/* Turn every <template shadowrootmode="open|closed"> under `root` into a shadow root on its parent. */
void dom_attach_declarative_shadows(struct dom_node *root);

/* Tree mutation report for MutationObserver; the hook stays NULL until script observes something.
 * CHILDREN: `added` or `removed` sits between `prev` and `next`. CLEAR: every child of `target` is
 * about to be freed, `removed` is the first of them. ATTR: `old` is NULL for a new attribute.
 * TEXT: `old` is the previous data of the text node `target`. */
enum dom_mut { DOM_MUT_ATTR, DOM_MUT_CHILDREN, DOM_MUT_CLEAR, DOM_MUT_TEXT };
struct dom_mutation {
  enum dom_mut kind;
  struct dom_node *target, *added, *removed, *prev, *next;
  const char *name, *old;
};
extern void (*dom_on_mutate)(const struct dom_mutation *m);
void dom_set_text(struct dom_node *n, const char *text);

/* Hook the JS layer installs so a freed node detaches its wrapper. */
extern void (*dom_on_free_wrapper)(void *wrapper);
extern void (*dom_on_free_style_cache)(void *cache);

/* Bumped on every tree mutation. */
extern unsigned dom_mutation_seq;

#endif /* OWOS_DOM_H */
