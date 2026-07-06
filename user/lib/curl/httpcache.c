/* httpcache.c — the RFC 7234 caching policy declared in httpcache.h.
 *
 * Pure computation over a parsed response: no malloc, no I/O, no clock (the
 * caller passes `now`). Every fiddly bit of the spec — the three date grammars,
 * the freshness-lifetime precedence, the current-age formula — lives here once
 * so a cache just stores bytes and asks these functions fresh-or-stale. */
#include "owos.h"
#include "httpcache.h"

static int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_dig(int c)   { return c >= '0' && c <= '9'; }

static long rd_num(const char **p)
{
	long v = 0;
	while (is_dig((unsigned char)**p)) { v = v * 10 + (**p - '0'); (*p)++; }
	return v;
}

/* First three letters (case-insensitive) name a month -> 1..12, else 0. */
static int month_num(const char *s)
{
	static const char *const m[12] = { "jan","feb","mar","apr","may","jun",
	                                    "jul","aug","sep","oct","nov","dec" };
	if (!s[0] || !s[1] || !s[2]) return 0;   /* don't read past a truncated header */
	for (int i = 0; i < 12; i++)
		if (lower(s[0]) == m[i][0] && lower(s[1]) == m[i][1] && lower(s[2]) == m[i][2])
			return i + 1;
	return 0;
}

/* Days from 1970-01-01 to civil y-m-d (Howard Hinnant's algorithm). */
static long days_from_civil(long y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

long http_date_parse(const char *s)
{
	if (!s) return 0;
	const char *p = s;
	while (*p == ' ') p++;
	while (is_alpha((unsigned char)*p)) p++;          /* weekday name */
	while (*p == ',' || *p == ' ') p++;

	int day = 0, mon = 0, hh = 0, mm = 0, ss = 0;
	long year = 0;

	if (is_alpha((unsigned char)*p)) {                /* asctime: Nov  6 08:49:37 1994 */
		mon = month_num(p);
		while (is_alpha((unsigned char)*p)) p++;
		while (*p == ' ') p++;
		day = (int)rd_num(&p);
		while (*p == ' ') p++;
		hh = (int)rd_num(&p);
		if (*p == ':') { p++; mm = (int)rd_num(&p); }
		if (*p == ':') { p++; ss = (int)rd_num(&p); }
		while (*p == ' ') p++;
		year = rd_num(&p);
	} else {                                          /* 06 Nov 1994 / 06-Nov-94 */
		day = (int)rd_num(&p);
		while (*p == '-' || *p == ' ') p++;
		mon = month_num(p);
		while (is_alpha((unsigned char)*p)) p++;
		while (*p == '-' || *p == ' ') p++;
		year = rd_num(&p);
		while (*p == ' ') p++;
		hh = (int)rd_num(&p);
		if (*p == ':') { p++; mm = (int)rd_num(&p); }
		if (*p == ':') { p++; ss = (int)rd_num(&p); }
	}

	if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
	if (hh > 23 || mm > 59 || ss > 60) return 0;      /* 60 = leap second */
	if (year < 100) year += (year < 70) ? 2000 : 1900;  /* RFC 850 two-digit year */

	return days_from_civil(year, mon, day) * 86400L + hh * 3600 + mm * 60 + ss;
}

/* Value of a Cache-Control directive that carries "=<digits>" (max-age,
 * s-maxage). Returns 1 + *out on success, 0 if the directive is absent. Matched
 * only at a comma/space/start boundary so "max-age" doesn't match inside another
 * token. */
static int cc_num_directive(const char *cc, const char *name, long *out)
{
	int nl = strlen(name);
	for (const char *p = cc; *p; p++) {
		if (p != cc && p[-1] != ',' && p[-1] != ' ' && p[-1] != '\t') continue;
		if (!starts_ci(p, name)) continue;
		const char *q = p + nl;
		while (*q == ' ' || *q == '\t') q++;
		if (*q != '=') continue;
		q++;
		while (*q == ' ' || *q == '\t' || *q == '"') q++;
		if (!is_dig((unsigned char)*q)) continue;
		*out = rd_num(&q);
		return 1;
	}
	return 0;
}

void http_cc_parse(const struct http_headers *H, struct http_cc *cc)
{
	memset(cc, 0, sizeof *cc);

	const char *ccv = http_headers_get(H, "cache-control");
	if (ccv) {
		cc->no_store        = http_header_has_token(ccv, "no-store");
		cc->no_cache        = http_header_has_token(ccv, "no-cache");
		cc->must_revalidate = http_header_has_token(ccv, "must-revalidate");
		cc->is_private      = http_header_has_token(ccv, "private");
		long ma;
		if (cc_num_directive(ccv, "max-age", &ma)) { cc->has_max_age = 1; cc->max_age = ma; }
	}
	/* HTTP/1.0 "Pragma: no-cache" is equivalent to Cache-Control: no-cache. */
	const char *pragma = http_headers_get(H, "pragma");
	if (pragma && http_header_has_token(pragma, "no-cache")) cc->no_cache = 1;

	cc->date = http_date_parse(http_headers_get(H, "date"));

	const char *exp = http_headers_get(H, "expires");
	if (exp) { cc->has_expires = 1; cc->expires = http_date_parse(exp); }

	bool ok;
	cc->age = (long)http_parse_uint(http_headers_get(H, "age"), &ok);
	if (!ok) cc->age = 0;

	cc->last_mod_str  = http_headers_get(H, "last-modified");
	cc->last_modified = http_date_parse(cc->last_mod_str);
	cc->etag = http_headers_get(H, "etag");
}

int http_cc_storable(const struct http_cc *cc, int status)
{
	if (cc->no_store) return 0;
	return status == 200;
}

long http_cc_freshness(const struct http_cc *cc, long now)
{
	if (cc->has_max_age) return cc->max_age > 0 ? cc->max_age : 0;
	if (cc->has_expires) {
		if (!cc->expires) return 0;               /* present but invalid = expired */
		long base = cc->date ? cc->date : now;
		long f = cc->expires - base;
		return f > 0 ? f : 0;
	}
	/* Heuristic (RFC 7234 §4.2.2): 10% of the time since Last-Modified. */
	if (cc->last_modified && cc->date) {
		long f = (cc->date - cc->last_modified) / 10;
		if (f < 0) f = 0;
		if (f > 86400) f = 86400;                 /* cap the guess at one day */
		return f;
	}
	return 0;
}

long http_cc_current_age(const struct http_cc *cc, long stored_at, long now)
{
	long apparent = stored_at - (cc->date ? cc->date : stored_at);
	if (apparent < 0) apparent = 0;
	long corrected = apparent > cc->age ? apparent : cc->age;
	long resident = now - stored_at;
	if (resident < 0) resident = 0;
	return corrected + resident;
}
