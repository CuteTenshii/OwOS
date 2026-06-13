/* url.c — shared URL parsing and resolution (see url.h). Self-contained string
 * helpers, so it links into any user program without dragging in other deps. */
#include "url.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int starts_ci(const char *s, const char *pre)
{
	while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; }
	return 1;
}

static void sapp(char *d, int *n, int cap, const char *s)
{ while (*s && *n < cap - 1) d[(*n)++] = *s++; d[*n] = '\0'; }

static void uapp(char *d, int *n, int cap, unsigned v)
{
	char t[12]; int k = 0;
	if (v == 0) { if (*n < cap - 1) d[(*n)++] = '0'; d[*n] = '\0'; return; }
	while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
	while (k && *n < cap - 1) d[(*n)++] = t[--k];
	d[*n] = '\0';
}

int parse_url(const char *url, struct purl *u)
{
	const char *p = url;
	u->tls = 0; u->port = 0;
	if      (starts_ci(p, "http://"))  p += 7;
	else if (starts_ci(p, "https://")) { p += 8; u->tls = 1; }

	int i = 0;
	while (*p && *p != '/' && *p != ':' && i < (int)sizeof(u->host) - 1)
		u->host[i++] = *p++;
	u->host[i] = '\0';
	if (i == 0) return 0;

	if (*p == ':') {
		p++;
		int port = 0;
		while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
		u->port = port;
	}
	if (u->port == 0) u->port = u->tls ? 443 : 80;

	int j = 0;
	if (*p != '/') u->path[j++] = '/';
	while (*p && j < (int)sizeof(u->path) - 1) u->path[j++] = *p++;
	u->path[j] = '\0';
	if (u->path[0] == '\0') { u->path[0] = '/'; u->path[1] = '\0'; }
	return 1;
}

void resolve_url(const char *base, const char *loc, char *out, int cap)
{
	int n = 0;
	if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) {
		sapp(out, &n, cap, loc);
		return;
	}
	struct purl b;
	if (!parse_url(base, &b)) { sapp(out, &n, cap, loc); return; }
	const char *scheme = b.tls ? "https://" : "http://";

	/* "host[:port]" of the base — the port must ride onto relative/absolute-path
	 * references, or a :8000-style origin would resolve back to :80. */
	char hostport[URL_HOST_MAX + 8]; int hp = 0;
	sapp(hostport, &hp, sizeof hostport, b.host);
	if (b.port != (b.tls ? 443 : 80)) {
		sapp(hostport, &hp, sizeof hostport, ":");
		uapp(hostport, &hp, sizeof hostport, (unsigned)b.port);
	}

	if (loc[0] == '/' && loc[1] == '/') {            /* scheme-relative "//host" */
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, loc + 2);
	} else if (loc[0] == '/') {                      /* absolute path            */
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, hostport);
		sapp(out, &n, cap, loc);
	} else {                                         /* relative to base's dir   */
		sapp(out, &n, cap, scheme);
		sapp(out, &n, cap, hostport);
		int slash = 0;
		for (int i = 0; b.path[i]; i++) if (b.path[i] == '/') slash = i;
		char dir[URL_PATH_MAX]; int d = 0;
		for (int i = 0; i <= slash && d < (int)sizeof(dir) - 1; i++)
			dir[d++] = b.path[i];
		dir[d] = '\0';
		sapp(out, &n, cap, dir);
		sapp(out, &n, cap, loc);
	}
}
