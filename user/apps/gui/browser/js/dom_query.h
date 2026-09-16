#ifndef BROWSER_JS_DOM_QUERY_H
#define BROWSER_JS_DOM_QUERY_H

#include "../jsint.h"

struct dom_node *js_dom_find_tag(struct dom_node *root, const char *tag);
struct dom_node *js_dom_find_id(struct dom_node *root, const char *id);
void js_dom_find_all_tag(struct dom_node *root, const char *tag, js_object *out);
struct dom_node *js_dom_query(struct dom_node *root, const char *selector);
void js_dom_query_all(struct dom_node *root, const char *selector, js_object *out);
int js_dom_matches(struct dom_node *node, const char *selector);

#endif
