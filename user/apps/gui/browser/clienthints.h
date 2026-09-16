/* clienthints.h — HTTP Client Hints (Accept-CH opt-in).
 *
 * A response's Accept-CH header lists the client-hint request headers the
 * origin wants on later requests. The browser records the requested set per
 * host and echoes back the hints it can answer (the Sec-CH-UA user-agent
 * family, DPR, Viewport-Width). Hints are strictly opt-in: a host that never
 * sends Accept-CH receives none, so the default request footprint stays small.
 * State is in memory for the session (not persisted). */
#ifndef BROWSER_CLIENTHINTS_H
#define BROWSER_CLIENTHINTS_H

/* Record the hints an origin requested via its Accept-CH header value (a
 * comma-separated token list); unknown tokens are ignored. Merges with any set
 * previously recorded for `host`. */
void ch_accept(const char *host, const char *accept_ch_value);

/* Write the client-hint request headers `host` opted into as "Name: value\r\n"
 * lines into `out` (NUL-terminated, up to cap-1 bytes). Returns the length; 0
 * if the host opted into nothing. Names are canonical-cased (h2/h3 lowercase
 * them). */
int ch_request_headers(const char *host, char *out, int cap);

#endif /* BROWSER_CLIENTHINTS_H */
