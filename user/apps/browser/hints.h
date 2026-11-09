/* hints.h: preconnect and preload resource hints (Link headers, 103 Early Hints, <link> tags), run on background
 * threads. */
#ifndef OWOS_BROWSER_HINTS_H
#define OWOS_BROWSER_HINTS_H

#include <stdbool.h>

/* True if `tok` is a whitespace-delimited token of the rel value in [q,end). */
bool hint_rel_has(const char *q, const char *end, const char *tok);

/* Open a connection to the origin of `abs_url` and park it for the next request to that origin. */
void hint_preconnect(const char *abs_url);

/* Fetch `abs_url` into the HTTP cache ahead of use. `as` is the Link/`<link>` destination (style, font, script,
 * image); other values are ignored. Checked against the page CSP when one is set. */
void hint_preload(const char *abs_url, const char *as, const char *page_url);

/* Drop hints not yet started and wait for the running ones. Worker thread. */
void hints_join(void);

#endif /* OWOS_BROWSER_HINTS_H */
