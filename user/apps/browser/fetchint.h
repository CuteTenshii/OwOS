/* fetchint.h: internal interface between fetch.c and resource.c (render/UI files use browser.h). */
#ifndef OWOS_BROWSER_FETCHINT_H
#define OWOS_BROWSER_FETCHINT_H

#include "browser.h"

/* Request-header values shared by page and sub-resource requests. */
#define BR_ACCEPT "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
#define BR_ACCLNG "en-US,en;q=0.9"

/* Accept of sub-resource requests: images, and everything else the batch fetcher loads (css, fonts, scripts). */
#define IMG_ACCEPT "image/png,image/webp,image/gif,image/svg+xml,*/*"
#define SUBRES_ACCEPT "text/css,*/*"

/* resource.c: sub-resources over their own sockets, several in parallel. */
void fetch_inline_images(const char *page_url); /* eager inline <img>, in parallel */
void fetch_favicon(const char *page_url);       /* page favicon -> B->favicon      */
/* Batch-fetch external stylesheets (html.c hook); bodies[i] malloc'd, caller frees. */
void css_batch(char *const *urls, int n, unsigned char **bodies, int *lens);

/* Origins that advertised HTTP/3 through Alt-Svc, tried over h3 first. h3_forget benches an origin for a few minutes
 * after QUIC failed, so a network that drops UDP is not retried on every request. */
bool h3_known(const char *host);
void h3_remember(const char *host);
void h3_forget(const char *host);
/* Keep what a server pushes over h3 in the HTTP cache; call before opening an h3 session. */
void h3_push_init(void);

/* An h2 session to u (idle, or a new connection whose ALPN selected h2); else NULL with the HTTP/1.1 connection in
 * *plain, or NULL there too if the connection failed. `fresh` skips the idle pool. */
h2_session *h2_open(const struct purl *u, bool fresh, netconn **plain);

/* Decode a data: URL into a malloc'd buffer (caller frees); media type goes to `ctype`. NULL on error. */
unsigned char *decode_data_url(const char *url, int *outlen, char *ctype, int cap);

#endif /* OWOS_BROWSER_FETCHINT_H */
