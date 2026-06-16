/* dom.h — HTML DOM tree for the browser. dom_parse() builds a node tree on the
 * heap; html.c walks it to render. */
#ifndef OWOS_DOM_H
#define OWOS_DOM_H

/* Shared case-insensitive ASCII matchers, used by both dom.c and html.c (kept
 * here so dom.h has no dependencies). browser.h re-exports them via this header. */
static inline char lower(char c)
{ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int starts_ci(const char *s, const char *pre)
{ while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; } return 1; }
static inline int streq_ci(const char *a, const char *b)
{ while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) == lower(*b); }

enum dom_type { DOM_ELEMENT, DOM_TEXT };

struct dom_attr {
	char            *name;           /* lowercased                           */
	char            *value;          /* raw (entities not decoded), "" if none */
	struct dom_attr *next;
};

struct dom_node {
	enum dom_type    type;
	char             tag[16];        /* lowercased tag name (element)        */
	char            *text;           /* character data (text node)           */
	struct dom_attr *attrs;
	struct dom_node *parent;
	struct dom_node *first_child;
	struct dom_node *last_child;
	struct dom_node *next;           /* sibling                              */
};

/* Parse `len` bytes into a tree under a synthetic "#document" root. Returns NULL
 * only on immediate OOM; a mid-parse OOM returns the partial tree. */
struct dom_node *dom_parse(const char *html, int len);
void dom_free(struct dom_node *root);
const char *dom_get_attr(const struct dom_node *node, const char *name);

#endif /* OWOS_DOM_H */
