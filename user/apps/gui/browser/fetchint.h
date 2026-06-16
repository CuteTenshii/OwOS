/* fetchint.h — internal interface shared among the browser's transport files
 * (fetch.c = page transport + orchestration, resource.c = sub-resources). It is
 * NOT for the render/UI files; those use browser.h. */
#ifndef OWOS_BROWSER_FETCHINT_H
#define OWOS_BROWSER_FETCHINT_H

#include "browser.h"

/* Request-header values, shared so the browser presents itself identically on
 * the page request and every sub-resource request. */
#define BR_UA     "Mozilla/5.0 (OwOS; x86) OwOS-Browser/1.0"
#define BR_ACCEPT "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
#define BR_ACCLNG "en-US,en;q=0.9"

/* resource.c — sub-resources over their own sockets (HTTP/1.1, follow redirects,
 * send cookies, HSTS-upgrade). The kernel TLS is multi-flow, so several run at
 * once (the parallel image pool). */
void  fetch_inline_images(const char *page_url);   /* eager inline <img>, in parallel */
void  fetch_favicon(const char *page_url);          /* page favicon -> B->favicon      */
/* Batch-fetch external stylesheets (html.c hook): same-origin HTTPS multiplexed
 * over h2, the rest over h1.1. Fills bodies[i] (malloc'd, caller frees) + lens[i].*/
void  css_batch(char *const *urls, int n, unsigned char **bodies, int *lens);

#endif /* OWOS_BROWSER_FETCHINT_H */
