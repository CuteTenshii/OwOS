/* clienthints.h: HTTP Client Hints (Accept-CH, Critical-CH, Permissions-Policy delegation). */
#ifndef BROWSER_CLIENTHINTS_H
#define BROWSER_CLIENTHINTS_H

#include <stdbool.h>

#define CH_NBRANDS 2

struct ch_brand {
  const char *brand, *version;
};

/* The single source of truth for UA values, shared by the headers and navigator.userAgentData. */
struct ch_ua_data {
  struct ch_brand brands[CH_NBRANDS];      /* significant versions */
  struct ch_brand full_brands[CH_NBRANDS]; /* full versions */
  const char *platform, *platform_version, *architecture, *bitness, *model, *form_factor, *full_version;
  bool mobile, wow64;
};
extern const struct ch_ua_data ch_ua;

/* Record the hints a host requested via its Accept-CH value; unknown tokens are ignored. */
void ch_accept(const char *host, const char *accept_ch_value);

/* True for https URLs and localhost: the only origins that receive or expose hints. */
bool ch_secure_url(const char *url);

/* Write the "Name: value\r\n" hint headers for a same-origin request to `host` (assumed secure). */
int ch_request_headers(const char *host, char *out, int cap);

/* As above for a request to `url` made by a page at `page_origin` (NULL = top-level navigation).
 * Cross-origin requests get a hint only if `permissions_policy` (the page's header, may be NULL)
 * delegates it; the page origin's Accept-CH opt-in applies. Returns the length. */
int ch_request_headers_for(const char *page_origin, const char *permissions_policy, const char *url, char *out,
                           int cap);

/* True if a Critical-CH token names a hint present in `now_headers` but missing from `sent_headers`. */
bool ch_critical_retry(const char *critical_ch, const char *sent_headers, const char *now_headers);

#endif /* BROWSER_CLIENTHINTS_H */
