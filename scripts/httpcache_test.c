/* httpcache_test.c — host-side unit test for user/lib/curl/httpcache.c.
 *
 * The RFC 7234 caching *policy* (date parsing, Cache-Control parsing, freshness
 * and current-age arithmetic, storability) is pure computation, so it is tested
 * on the host with the real source — no QEMU, no network (the guest can't reach
 * one here anyway). Build + run:
 *
 *     cc -std=c11 -Iscripts -Iuser/lib/curl \
 *        scripts/httpcache_test.c user/lib/curl/httpcache.c user/lib/curl/headers.c \
 *        -o /tmp/httpcache_test && /tmp/httpcache_test
 *
 * scripts/ carries a tiny shim owos.h giving httpcache.c/headers.c the few libc
 * decls they pull from the real one (strlen, memset, malloc, lower, starts_ci). */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "httpcache.h"

static int fails;
#define CHECK(cond) do { \
	if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); \
	if (_a != _b) { printf("FAIL %s:%d  %s == %s  (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b); fails++; } \
} while (0)

/* Parse a header block and return its cc facts. */
static void parse_cc(const char *block, struct http_cc *cc, struct http_headers *H)
{
	http_headers_parse(H, block);
	http_cc_parse(H, cc);
}

int main(void)
{
	/* --- http_date_parse: the three grammars all map to the same instant. */
	long imf     = http_date_parse("Sun, 06 Nov 1994 08:49:37 GMT");
	long rfc850  = http_date_parse("Sunday, 06-Nov-94 08:49:37 GMT");
	long asctime = http_date_parse("Sun Nov  6 08:49:37 1994");
	CHECK_EQ(imf, 784111777L);          /* known epoch for that date */
	CHECK_EQ(rfc850, imf);
	CHECK_EQ(asctime, imf);
	CHECK_EQ(http_date_parse("Thu, 01 Jan 1970 00:00:00 GMT"), 0L);
	CHECK_EQ(http_date_parse("garbage"), 0L);
	CHECK_EQ(http_date_parse(NULL), 0L);
	/* epoch of a round 2021 date, cross-checkable with `date -u -d`. */
	CHECK_EQ(http_date_parse("Fri, 01 Jan 2021 00:00:00 GMT"), 1609459200L);

	struct http_headers H;
	struct http_cc cc;
	long now = 1000000;   /* arbitrary "now" in epoch seconds */

	/* --- max-age wins; no-store / no-cache / must-revalidate flagged. */
	parse_cc("HTTP/1.1 200 OK\r\nCache-Control: max-age=600, must-revalidate\r\n\r\n", &cc, &H);
	CHECK(cc.has_max_age);
	CHECK_EQ(cc.max_age, 600L);
	CHECK(cc.must_revalidate);
	CHECK(!cc.no_store);
	CHECK_EQ(http_cc_freshness(&cc, now), 600L);
	CHECK(http_cc_storable(&cc, 200));
	http_headers_free(&H);

	parse_cc("HTTP/1.1 200 OK\r\nCache-Control: no-store\r\n\r\n", &cc, &H);
	CHECK(cc.no_store);
	CHECK(!http_cc_storable(&cc, 200));
	http_headers_free(&H);

	parse_cc("HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\n\r\n", &cc, &H);
	CHECK(cc.no_cache);
	CHECK(http_cc_storable(&cc, 200));   /* storable, but must revalidate before use */
	http_headers_free(&H);

	/* Pragma: no-cache (HTTP/1.0) folds into no_cache. */
	parse_cc("HTTP/1.1 200 OK\r\nPragma: no-cache\r\n\r\n", &cc, &H);
	CHECK(cc.no_cache);
	http_headers_free(&H);

	/* Only 200 is storable here. */
	parse_cc("HTTP/1.1 404 Not Found\r\nCache-Control: max-age=60\r\n\r\n", &cc, &H);
	CHECK(!http_cc_storable(&cc, 404));
	http_headers_free(&H);

	/* --- Expires - Date freshness (max-age absent). */
	parse_cc("HTTP/1.1 200 OK\r\n"
	         "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
	         "Expires: Fri, 01 Jan 2021 01:00:00 GMT\r\n\r\n", &cc, &H);
	CHECK(cc.has_expires);
	CHECK_EQ(http_cc_freshness(&cc, now), 3600L);
	http_headers_free(&H);

	/* An Expires in the past = already stale (freshness 0). */
	parse_cc("HTTP/1.1 200 OK\r\n"
	         "Date: Fri, 01 Jan 2021 02:00:00 GMT\r\n"
	         "Expires: Fri, 01 Jan 2021 01:00:00 GMT\r\n\r\n", &cc, &H);
	CHECK_EQ(http_cc_freshness(&cc, now), 0L);
	http_headers_free(&H);

	/* An invalid Expires is treated as expired even without a Date. */
	parse_cc("HTTP/1.1 200 OK\r\nExpires: 0\r\n\r\n", &cc, &H);
	CHECK(cc.has_expires);
	CHECK_EQ(http_cc_freshness(&cc, now), 0L);
	http_headers_free(&H);

	/* --- Last-Modified heuristic: 10% of the resource's age, capped at a day. */
	parse_cc("HTTP/1.1 200 OK\r\n"
	         "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
	         "Last-Modified: Thu, 01 Jan 2021 00:00:00 GMT\r\n\r\n", &cc, &H);
	/* Last-Modified == Date -> age 0 -> freshness 0. */
	CHECK_EQ(http_cc_freshness(&cc, now), 0L);
	http_headers_free(&H);

	parse_cc("HTTP/1.1 200 OK\r\n"
	         "Date: Fri, 08 Jan 2021 00:00:00 GMT\r\n"        /* one week after LM */
	         "Last-Modified: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n", &cc, &H);
	CHECK_EQ(http_cc_freshness(&cc, now), 604800L / 10);      /* 10% of a week */
	http_headers_free(&H);

	parse_cc("HTTP/1.1 200 OK\r\n"
	         "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
	         "Last-Modified: Fri, 01 Jan 2010 00:00:00 GMT\r\n\r\n", &cc, &H);
	CHECK_EQ(http_cc_freshness(&cc, now), 86400L);            /* capped at one day */
	http_headers_free(&H);

	/* --- current age (RFC 7234 §4.2.3): Age header + residence time. */
	parse_cc("HTTP/1.1 200 OK\r\nAge: 100\r\n\r\n", &cc, &H);
	/* stored 50s ago, was already 100s old on arrival -> 150s now. */
	CHECK_EQ(http_cc_current_age(&cc, 1000, 1050), 150L);
	http_headers_free(&H);

	/* apparent age from Date (no Age header): received 200s after Date. */
	parse_cc("HTTP/1.1 200 OK\r\nDate: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n", &cc, &H);
	{
		long date = 1609459200L;
		long stored = date + 200;      /* received 200s after the Date */
		CHECK_EQ(http_cc_current_age(&cc, stored, stored), 200L);
		CHECK_EQ(http_cc_current_age(&cc, stored, stored + 30), 230L);
	}
	http_headers_free(&H);

	/* --- a fresh-vs-stale end to end: max-age=300, stored now. */
	parse_cc("HTTP/1.1 200 OK\r\nCache-Control: max-age=300\r\n\r\n", &cc, &H);
	{
		long lifetime = http_cc_freshness(&cc, now);
		CHECK(http_cc_current_age(&cc, now, now + 100) < lifetime);   /* fresh  */
		CHECK(http_cc_current_age(&cc, now, now + 400) >= lifetime);  /* stale  */
	}
	http_headers_free(&H);

	/* --- validators surface for conditional requests. */
	parse_cc("HTTP/1.1 200 OK\r\nETag: \"abc123\"\r\n"
	         "Last-Modified: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n", &cc, &H);
	CHECK(cc.etag && strcmp(cc.etag, "\"abc123\"") == 0);
	CHECK(cc.last_mod_str && strcmp(cc.last_mod_str, "Fri, 01 Jan 2021 00:00:00 GMT") == 0);
	http_headers_free(&H);

	if (fails == 0) printf("httpcache_test: all checks passed\n");
	else            printf("httpcache_test: %d FAILED\n", fails);
	return fails ? 1 : 0;
}
