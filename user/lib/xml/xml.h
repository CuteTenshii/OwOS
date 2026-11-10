/* xml.h: a small allocation-free XML reader and escaper for protocol payloads (SOAP, device descriptions).
 *
 * Works on spans of an in-memory document. Elements are matched by local name (any namespace) or, with the _ns
 * variants, by namespace URI and local name. Not a validating parser: DTDs, comments, processing
 * instructions and CDATA sections are skipped or passed through as text, never interpreted. Links with $(XML); no
 * dependencies. */
#ifndef OWOS_XML_H
#define OWOS_XML_H

struct xml_span {
  const char *s, *e;
  const char *doc; /* start of the document, for resolving namespace declarations on ancestors; 0 = unknown */
};

/* Span over a whole NUL-terminated document. */
struct xml_span xml_all(const char *doc);

/* Find the next element named `name` in *scan. Sets *inner to its content (an empty span for <a/>) and moves
 * scan->s past the element, so repeated calls walk the siblings. Same-name nesting is balanced. 1 / 0. */
int xml_next(struct xml_span *scan, const char *name, struct xml_span *inner);

/* As xml_next, but only elements whose resolved namespace URI is ns (NULL = any, "" = none) match. Prefixes
 * resolve through xmlns declarations on the element and its ancestors, nested up to 32 levels deep. A span not
 * made by xml_all or xml_next* has no doc, so only declarations inside it are seen. */
int xml_next_ns(struct xml_span *scan, const char *ns, const char *name, struct xml_span *inner);

/* Decoded text of the first element `name` in `in` (entities and CDATA resolved, stopping at the first child
 * element) into out. Returns its length, or -1 if absent or it does not fit in cap. */
int xml_text(struct xml_span in, const char *name, char *out, int cap);

/* xml_text for the first element `name` in namespace ns (see xml_next_ns). */
int xml_text_ns(struct xml_span in, const char *ns, const char *name, char *out, int cap);

/* Append s to out[len..cap) with &, <, >, " escaped, NUL-terminated. Returns the new length, -1 if it does
 * not fit. */
int xml_escape(char *out, int len, int cap, const char *s);

#endif /* OWOS_XML_H */
