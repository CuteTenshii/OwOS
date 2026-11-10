/* xmltree.h: XML decoder and encoder over a heap-allocated node tree. Link $(XMLTREE); libc only.
 *
 * xml_parse reads a well-formed UTF-8 (or US-ASCII) document into a tree: names keep their case, attribute values are
 * normalized, entity and character references are decoded, CDATA sections stay as XML_CDATA nodes, and element and
 * attribute namespaces are resolved from xmlns declarations (the declarations stay in the attribute list). DOCTYPE
 * declarations are skipped; entity declarations in an internal subset are rejected and so are other encodings.
 * UTF-8 byte sequences are not validated. Nesting is limited to 256 levels.
 *
 * xml_write turns a tree back into text. It writes names and attributes as stored, so a built tree must carry its
 * own xmlns attributes; it does not invent declarations. A tree with an invalid name or character, a comment
 * containing "--", or a PI containing "?>" is refused.
 *
 * Ownership: xml_parse and the xml_new* builders return nodes the caller releases with xml_free (which also detaches
 * the node from its parent). xml_append takes ownership of the child, freeing it on failure unless it is attached
 * elsewhere or an ancestor of the parent. xml_write and xml_text_content return malloc'd strings the caller free()s. */
#ifndef OWOS_XMLTREE_H
#define OWOS_XMLTREE_H

enum xml_type { XML_DOC, XML_ELEM, XML_TEXT, XML_CDATA, XML_COMMENT, XML_PI };

struct xml_attr {
  char *name;  /* qualified name as written */
  char *value; /* decoded */
  char *ns;    /* namespace URI; "" for none, the xmlns URI for declarations */
  struct xml_attr *next;
};

struct xml_node {
  enum xml_type type;
  char *name;        /* XML_ELEM: qualified name; XML_PI: target */
  const char *local; /* XML_ELEM: the part of name after any prefix */
  char *ns;          /* XML_ELEM: namespace URI, "" for none */
  char *value;       /* TEXT, CDATA, COMMENT: content; PI: data */
  struct xml_attr *attrs;
  struct xml_node *parent, *first, *last, *next, *prev;
};

struct xml_error {
  int pos, line, col; /* byte offset and 1-based position of the problem */
  char msg[64];
};

/* Parse buf[0..len) into an XML_DOC node. NULL with *err filled (err may be NULL) on bad input. */
struct xml_node *xml_parse(const char *buf, int len, struct xml_error *err);

void xml_free(struct xml_node *n);

/* An element named qname in namespace ns (NULL = none), or NULL if the name is invalid. */
struct xml_node *xml_new_element(const char *qname, const char *ns);

/* An XML_DOC (name and value ignored), XML_TEXT, XML_CDATA, XML_COMMENT (value) or XML_PI (name = target,
 * value = data) node. */
struct xml_node *xml_new(enum xml_type type, const char *name, const char *value);

/* Add child last under parent (an XML_DOC or XML_ELEM). Refuses a child that already has a parent, an ancestor of
 * parent, an XML_DOC, text directly under a document, and a second root element. 0 / -1. */
int xml_append(struct xml_node *parent, struct xml_node *child);

/* Set attribute qname on el, replacing an existing one. ns is its namespace URI (NULL = none); xmlns declarations
 * are recognised by name. 0 / -1. */
int xml_set_attr(struct xml_node *el, const char *qname, const char *ns, const char *value);

/* Value of the attribute named qname, or by namespace URI ("" = none) and local name. NULL if absent. */
const char *xml_attr(const struct xml_node *el, const char *qname);
const char *xml_attr_ns(const struct xml_node *el, const char *ns, const char *local);

/* First child element of parent (or the next sibling element after `after`) with this local name, in namespace ns
 * (NULL = any). NULL if none. */
struct xml_node *xml_find(struct xml_node *parent, const char *ns, const char *local);
struct xml_node *xml_find_next(struct xml_node *after, const char *ns, const char *local);

/* Concatenated TEXT and CDATA content of n and its descendants. */
char *xml_text_content(const struct xml_node *n);

#define XML_PRETTY 1 /* indent element-only content by two spaces and end with a newline */
#define XML_DECL 2   /* start with <?xml version="1.0" encoding="UTF-8"?> */

/* Serialize n and its subtree. NULL if it cannot be represented. *out_len (optional) gets the length. */
char *xml_write(const struct xml_node *n, int flags, int *out_len);

#endif /* OWOS_XMLTREE_H */
