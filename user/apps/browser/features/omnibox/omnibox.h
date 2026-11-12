/* omnibox.h: turn address-bar text into a navigation target, searching when it isn't a URL. */
#ifndef BROWSER_OMNIBOX_H
#define BROWSER_OMNIBOX_H

#include <stdbool.h>

/* Query URL prefix; the form-encoded search terms are appended to it. */
#define OMNIBOX_SEARCH_PREFIX "https://www.google.com/search?q="

/* True when the n bytes at s (already trimmed) name a URL or host rather than search terms. */
bool omnibox_is_url(const char *s, int n);

/* Write the URL to load for the typed text into out[cap]: the trimmed text itself when it is a URL,
 * otherwise a search for it. Returns the length written; 0 when the text is blank. */
int omnibox_target(const char *typed, char *out, int cap);

#endif
