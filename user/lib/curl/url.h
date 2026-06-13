/* url.h — URL parsing + relative-URL resolution, shared by the user programs
 * that speak HTTP (curl, the browser). Freestanding, no allocation. */
#ifndef OWOS_URL_H
#define OWOS_URL_H

#define URL_HOST_MAX 256
#define URL_PATH_MAX 512

/* A split URL: scheme (tls flag), host, port, and an absolute request path. */
struct purl { int tls; int port; char host[URL_HOST_MAX]; char path[URL_PATH_MAX]; };

/* Split "scheme://host[:port][/path]". Lenient: a missing scheme means http; the
 * port defaults to 443 (https) / 80 (http). No IPv6-bracket support. Returns 1
 * on success, 0 if the host is empty. */
int  parse_url(const char *url, struct purl *u);

/* Resolve `loc` against `base` into out[cap] as an absolute URL. `loc` may be a
 * full URL (returned as-is), "//host/path" (inherits the scheme), "/abs/path"
 * (inherits scheme+host[:port]), or a relative path (joined onto base's dir). */
void resolve_url(const char *base, const char *loc, char *out, int cap);

#endif /* OWOS_URL_H */
