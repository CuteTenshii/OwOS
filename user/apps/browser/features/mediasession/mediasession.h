#ifndef BROWSER_MEDIASESSION_H
#define BROWSER_MEDIASESSION_H

#include "jsint.h"

/* Define MediaMetadata and navigator.mediaSession; artwork URLs resolve vs page_url. */
void mediasession_install(js_ctx *ctx, js_object *navigator, const char *page_url);
/* Worker loop: publish changes to the system media service and run polled commands. */
void mediasession_tick(const char *doc_title, const char *page_url);
/* Page unload: close the system session and unroot the page's objects. */
void mediasession_teardown(void);

#endif
