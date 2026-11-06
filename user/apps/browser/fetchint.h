/* fetchint.h: internal interface between fetch.c and resource.c (render/UI files use browser.h). */
#ifndef OWOS_BROWSER_FETCHINT_H
#define OWOS_BROWSER_FETCHINT_H

#include "browser.h"

/* Request-header values shared by page and sub-resource requests. */
#define BR_ACCEPT "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
#define BR_ACCLNG "en-US,en;q=0.9"

/* resource.c: sub-resources over their own sockets, several in parallel. */
void fetch_inline_images(const char *page_url); /* eager inline <img>, in parallel */
void fetch_favicon(const char *page_url);       /* page favicon -> B->favicon      */
/* Batch-fetch external stylesheets (html.c hook); bodies[i] malloc'd, caller frees. */
void css_batch(char *const *urls, int n, unsigned char **bodies, int *lens);

/* Decode a data: URL into a malloc'd buffer (caller frees); media type goes to `ctype`. NULL on error. */
unsigned char *decode_data_url(const char *url, int *outlen, char *ctype, int cap);

#endif /* OWOS_BROWSER_FETCHINT_H */
