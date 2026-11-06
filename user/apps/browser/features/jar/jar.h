/* jar.h: scheme/path-aware cookie header, beyond browser.h's flat jar API. */
#ifndef BROWSER_JAR_H
#define BROWSER_JAR_H

/* Like cookie_header, but skips Secure cookies unless `tls` and honours Path (NULL = no filter). */
int cookie_header_tls(const char *host, const char *path, int tls, char *out, int cap);

#endif /* BROWSER_JAR_H */
