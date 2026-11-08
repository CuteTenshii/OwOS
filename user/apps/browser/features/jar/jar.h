/* jar.h: scheme/path-aware cookie header, beyond browser.h's flat jar API. */
#ifndef BROWSER_JAR_H
#define BROWSER_JAR_H

/* Like cookie_header, but skips Secure cookies unless `tls` and honours Path (NULL = no filter). */
int cookie_header_tls(const char *host, const char *path, int tls, char *out, int cap);

/* document.cookie view: HttpOnly cookies hidden and the setter refuses to replace them. */
int cookie_script_list(const char *host, const char *path, int tls, char *out, int cap);
void cookie_store_script(const char *host, const char *setcookie_value);

#endif /* BROWSER_JAR_H */
