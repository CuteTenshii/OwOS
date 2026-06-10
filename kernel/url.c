/* url.c — URL parser/builder (see url.h).
 *
 * Self-contained: no allocation, no libc. The parser walks the string once,
 * carving out each component into the caller's struct; the builder stitches
 * them back together, emitting the optional delimiters only when their part is
 * present so a parse→build round-trip is faithful.
 */

#include "url.h"

/* ------------------------------------------------------------------ helpers */

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Copy at most len bytes of src into a dst[cap] buffer, NUL-terminating.
 * Returns 1 if everything fit, 0 if it was truncated. */
static int copyn(char *dst, int cap, const char *src, int len)
{
	int n = 0;
	while (n < len && n < cap - 1) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
	return n == len;
}

/* Append s to dst[cap] starting at *pn; advances *pn. Returns 0 on overflow. */
static int sappend(char *dst, int cap, int *pn, const char *s)
{
	int n = *pn;
	while (*s) {
		if (n >= cap - 1)
			return 0;
		dst[n++] = *s++;
	}
	dst[n] = '\0';
	*pn = n;
	return 1;
}

static int cappend(char *dst, int cap, int *pn, char c)
{
	char s[2] = { c, '\0' };
	return sappend(dst, cap, pn, s);
}

/* ------------------------------------------------------------------- parse */

int url_parse(const char *s, struct url *u)
{
	u->scheme[0] = u->userinfo[0] = u->host[0] = '\0';
	u->path[0] = u->query[0] = u->fragment[0] = '\0';
	u->port = 0;
	u->has_port = u->has_query = u->has_fragment = u->is_ipv6 = u->is_tls = 0;

	const char *p = s;

	/* scheme:// — only when "://" actually follows a run of scheme chars, so a
	 * bare "host:port" isn't mistaken for a scheme. */
	{
		int i = 0;
		while (p[i] && p[i] != ':' && p[i] != '/' && p[i] != '?' && p[i] != '#')
			i++;
		if (p[i] == ':' && p[i + 1] == '/' && p[i + 2] == '/' && i > 0) {
			if (!copyn(u->scheme, URL_SCHEME_MAX, p, i))
				return 0;
			for (int j = 0; u->scheme[j]; j++)
				u->scheme[j] = lower(u->scheme[j]);
			p += i + 3;
		}
	}

	/* Defaults implied by the scheme. */
	if (u->scheme[0]) {
		if (u->scheme[0] == 'h' && u->scheme[1] == 't' && u->scheme[2] == 't' &&
		    u->scheme[3] == 'p' && u->scheme[4] == 's' && u->scheme[5] == '\0') {
			u->port = 443; u->is_tls = 1;
		} else if (u->scheme[0] == 'h' && u->scheme[1] == 't' &&
		           u->scheme[2] == 't' && u->scheme[3] == 'p' &&
		           u->scheme[4] == '\0') {
			u->port = 80;
		}
	}

	/* authority = up to the first '/', '?', or '#'. */
	int alen = 0;
	while (p[alen] && p[alen] != '/' && p[alen] != '?' && p[alen] != '#')
		alen++;
	const char *auth = p;
	const char *rest = p + alen;

	/* userinfo@ — split on the last '@' inside the authority. */
	int at = -1;
	for (int i = 0; i < alen; i++)
		if (auth[i] == '@')
			at = i;
	const char *hp = auth;            /* host:port slice start */
	int hplen = alen;
	if (at >= 0) {
		if (!copyn(u->userinfo, URL_USERINFO_MAX, auth, at))
			return 0;
		hp = auth + at + 1;
		hplen = alen - at - 1;
	}

	/* host[:port], with [..] for an IPv6 literal. */
	int hostlen;
	const char *portstr = 0;
	int portlen = 0;
	if (hplen > 0 && hp[0] == '[') {
		int close = -1;
		for (int i = 1; i < hplen; i++)
			if (hp[i] == ']') { close = i; break; }
		if (close < 0)
			return 0;
		u->is_ipv6 = 1;
		hostlen = close - 1;
		if (!copyn(u->host, URL_HOST_MAX, hp + 1, hostlen))
			return 0;
		if (close + 1 < hplen && hp[close + 1] == ':') {
			portstr = hp + close + 2;
			portlen = hplen - close - 2;
		}
	} else {
		int colon = -1;
		for (int i = 0; i < hplen; i++)
			if (hp[i] == ':') { colon = i; break; }
		hostlen = (colon >= 0) ? colon : hplen;
		if (!copyn(u->host, URL_HOST_MAX, hp, hostlen))
			return 0;
		if (colon >= 0) {
			portstr = hp + colon + 1;
			portlen = hplen - colon - 1;
		}
	}
	if (u->host[0] == '\0')
		return 0;

	if (portstr) {
		uint32_t v = 0;
		if (portlen == 0)
			return 0;
		for (int i = 0; i < portlen; i++) {
			if (portstr[i] < '0' || portstr[i] > '9')
				return 0;
			v = v * 10 + (uint32_t)(portstr[i] - '0');
			if (v > 65535)
				return 0;
		}
		if (v == 0)
			return 0;
		u->port = (uint16_t)v;
		u->has_port = 1;
	}

	/* path, then ?query, then #fragment. */
	p = rest;
	int plen = 0;
	while (p[plen] && p[plen] != '?' && p[plen] != '#')
		plen++;
	if (!copyn(u->path, URL_PATH_MAX, p, plen))
		return 0;
	p += plen;

	if (*p == '?') {
		p++;
		u->has_query = 1;
		int qlen = 0;
		while (p[qlen] && p[qlen] != '#')
			qlen++;
		if (!copyn(u->query, URL_QUERY_MAX, p, qlen))
			return 0;
		p += qlen;
	}

	if (*p == '#') {
		p++;
		u->has_fragment = 1;
		int flen = 0;
		while (p[flen])
			flen++;
		if (!copyn(u->fragment, URL_FRAGMENT_MAX, p, flen))
			return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------- build */

int url_build(const struct url *u, char *out, int outsz)
{
	int n = 0;
	out[0] = '\0';

	if (u->scheme[0]) {
		if (!sappend(out, outsz, &n, u->scheme)) return -1;
		if (!sappend(out, outsz, &n, "://")) return -1;
	}
	if (u->userinfo[0]) {
		if (!sappend(out, outsz, &n, u->userinfo)) return -1;
		if (!cappend(out, outsz, &n, '@')) return -1;
	}
	if (u->is_ipv6) {
		if (!cappend(out, outsz, &n, '[')) return -1;
		if (!sappend(out, outsz, &n, u->host)) return -1;
		if (!cappend(out, outsz, &n, ']')) return -1;
	} else {
		if (!sappend(out, outsz, &n, u->host)) return -1;
	}
	if (u->has_port) {
		char num[6];
		int d = 0;
		uint16_t v = u->port;
		char tmp[6];
		int t = 0;
		if (v == 0)
			tmp[t++] = '0';
		while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
		while (t)
			num[d++] = tmp[--t];
		num[d] = '\0';
		if (!cappend(out, outsz, &n, ':')) return -1;
		if (!sappend(out, outsz, &n, num)) return -1;
	}
	if (u->path[0])
		if (!sappend(out, outsz, &n, u->path)) return -1;
	if (u->has_query) {
		if (!cappend(out, outsz, &n, '?')) return -1;
		if (!sappend(out, outsz, &n, u->query)) return -1;
	}
	if (u->has_fragment) {
		if (!cappend(out, outsz, &n, '#')) return -1;
		if (!sappend(out, outsz, &n, u->fragment)) return -1;
	}
	return n;
}

/* ----------------------------------------------------- percent encode/decode */

static int unreserved(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') ||
	       c == '-' || c == '_' || c == '.' || c == '~';
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int url_encode(const char *s, char *out, int outsz)
{
	static const char hex[] = "0123456789ABCDEF";
	int n = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (unreserved((char)c)) {
			if (n >= outsz - 1) return -1;
			out[n++] = (char)c;
		} else {
			if (n >= outsz - 3) return -1;
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 0xf];
		}
	}
	out[n] = '\0';
	return n;
}

int url_decode(const char *s, char *out, int outsz)
{
	int n = 0;
	while (*s) {
		if (n >= outsz - 1)
			return -1;
		if (*s == '%') {
			int hi = hexval(s[1]);
			int lo = hi < 0 ? -1 : hexval(s[2]);
			if (lo < 0)
				return -1;
			out[n++] = (char)((hi << 4) | lo);
			s += 3;
		} else if (*s == '+') {
			out[n++] = ' ';
			s++;
		} else {
			out[n++] = *s++;
		}
	}
	out[n] = '\0';
	return n;
}
