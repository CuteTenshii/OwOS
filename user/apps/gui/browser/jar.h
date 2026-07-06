/* jar.h — cookie/HSTS jar entry points that don't fit browser.h's flat API.
 *
 * browser.h declares the original jar functions (cookie_store, cookie_header,
 * the hsts and jar_ helpers). Its cookie_header(host, out, cap) can't see the
 * request scheme or path, so it can't honour Secure or a cookie's Path. This
 * adds a variant that takes both; the legacy cookie_header stays for callers
 * (resource.c's sub-resource paths) that don't thread that context through. */
#ifndef BROWSER_JAR_H
#define BROWSER_JAR_H

/* Like cookie_header, but scheme/path-aware: omits Secure cookies unless `tls`,
 * and Path-scoped cookies unless `path` is under their Path (NULL = no Path
 * filter). Domain-scoped cookies are suffix-matched against `host` in both
 * variants. Returns the header value length written to `out`. */
int cookie_header_tls(const char *host, const char *path, int tls,
                      char *out, int cap);

#endif /* BROWSER_JAR_H */
