#ifndef BROWSER_CSS_SIBLING_H
#define BROWSER_CSS_SIBLING_H

#include "../dom.h"

int css_elem_index(const struct dom_node *node);
int css_elem_count(const struct dom_node *node);
int css_type_index(const struct dom_node *node);
int css_type_count(const struct dom_node *node);

#endif
