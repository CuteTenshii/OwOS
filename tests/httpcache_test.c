/* httpcache_test.c - host-side unit test for user/lib/curl/httpcache.c.
 *
 * The RFC 7234 caching *policy* (date parsing, Cache-Control parsing, freshness
 * and current-age arithmetic, storability) is pure computation, so it is tested
 * on the host with the real source - no QEMU, no network (the guest can't reach
 * one here anyway). Build + run:
 *
 *     cc -std=c11 -Iscripts -Iuser/lib/curl \
 *        tests/httpcache_test.c user/lib/curl/httpcache.c user/lib/curl/headers.c \
 *        -o /tmp/httpcache_test && /tmp/httpcache_test
 *
 * scripts/ carries a tiny shim owos.h giving httpcache.c/headers.c the few libc
 * decls they pull from the real one (strlen, memset, malloc, lower, starts_ci). */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "httpcache.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Parse a header block and return its cc facts. */
static void parse_cc(const char *block, struct http_cc *cc, struct http_headers *H)
{
  http_headers_parse(H, block);
  http_cc_parse(H, cc);
}

int main(void)
{
  /* --- http_date_parse: the three grammars all map to the same instant. */
  test("HTTP date parsing");
  long imf = http_date_parse("Sun, 06 Nov 1994 08:49:37 GMT");
  long rfc850 = http_date_parse("Sunday, 06-Nov-94 08:49:37 GMT");
  long asctime = http_date_parse("Sun Nov  6 08:49:37 1994");
  expect((imf) == (784111777L)); /* known epoch for that date */
  expect((rfc850) == (imf));
  expect((asctime) == (imf));
  expect(http_date_parse("Thu, 01 Jan 1970 00:00:00 GMT") == 0L);
  expect((http_date_parse("garbage")) == (0L));
  expect((http_date_parse(NULL)) == (0L));
  /* epoch of a round 2021 date, cross-checkable with `date -u -d`. */
  expect(http_date_parse("Fri, 01 Jan 2021 00:00:00 GMT") == 1609459200L);

  struct http_headers H;
  struct http_cc cc;
  long now = 1000000; /* arbitrary "now" in epoch seconds */

  /* --- max-age wins; no-store / no-cache / must-revalidate flagged. */
  test("Cache-Control directives");
  parse_cc("HTTP/1.1 200 OK\r\nCache-Control: max-age=600, must-revalidate\r\n\r\n", &cc, &H);
  expect(cc.has_max_age);
  expect((cc.max_age) == (600L));
  expect(cc.must_revalidate);
  expect(!cc.no_store);
  expect(http_cc_freshness(&cc, now) == 600L);
  expect(http_cc_storable(&cc, 200));
  http_headers_free(&H);

  parse_cc("HTTP/1.1 200 OK\r\nCache-Control: no-store\r\n\r\n", &cc, &H);
  expect(cc.no_store);
  expect(!http_cc_storable(&cc, 200));
  http_headers_free(&H);

  parse_cc("HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\n\r\n", &cc, &H);
  expect(cc.no_cache);
  expect(http_cc_storable(&cc, 200)); /* storable, but must revalidate before use */
  http_headers_free(&H);

  /* Pragma: no-cache (HTTP/1.0) folds into no_cache. */
  parse_cc("HTTP/1.1 200 OK\r\nPragma: no-cache\r\n\r\n", &cc, &H);
  expect(cc.no_cache);
  http_headers_free(&H);

  /* Only 200 is storable here. */
  parse_cc("HTTP/1.1 404 Not Found\r\nCache-Control: max-age=60\r\n\r\n", &cc, &H);
  expect(!http_cc_storable(&cc, 404));
  http_headers_free(&H);

  /* --- Expires - Date freshness (max-age absent). */
  test("Expires freshness");
  parse_cc("HTTP/1.1 200 OK\r\n"
           "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
           "Expires: Fri, 01 Jan 2021 01:00:00 GMT\r\n\r\n",
           &cc, &H);
  expect(cc.has_expires);
  expect(http_cc_freshness(&cc, now) == 3600L);
  http_headers_free(&H);

  /* An Expires in the past = already stale (freshness 0). */
  parse_cc("HTTP/1.1 200 OK\r\n"
           "Date: Fri, 01 Jan 2021 02:00:00 GMT\r\n"
           "Expires: Fri, 01 Jan 2021 01:00:00 GMT\r\n\r\n",
           &cc, &H);
  expect(http_cc_freshness(&cc, now) == 0L);
  http_headers_free(&H);

  /* An invalid Expires is treated as expired even without a Date. */
  parse_cc("HTTP/1.1 200 OK\r\nExpires: 0\r\n\r\n", &cc, &H);
  expect(cc.has_expires);
  expect(http_cc_freshness(&cc, now) == 0L);
  http_headers_free(&H);

  /* --- Last-Modified heuristic: 10% of the resource's age, capped at a day. */
  test("Last-Modified heuristic");
  parse_cc("HTTP/1.1 200 OK\r\n"
           "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
           "Last-Modified: Thu, 01 Jan 2021 00:00:00 GMT\r\n\r\n",
           &cc, &H);
  /* Last-Modified == Date -> age 0 -> freshness 0. */
  expect(http_cc_freshness(&cc, now) == 0L);
  http_headers_free(&H);

  parse_cc("HTTP/1.1 200 OK\r\n"
           "Date: Fri, 08 Jan 2021 00:00:00 GMT\r\n" /* one week after LM */
           "Last-Modified: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n",
           &cc, &H);
  expect(http_cc_freshness(&cc, now) == 604800L / 10); /* 10% of a week */
  http_headers_free(&H);

  parse_cc("HTTP/1.1 200 OK\r\n"
           "Date: Fri, 01 Jan 2021 00:00:00 GMT\r\n"
           "Last-Modified: Fri, 01 Jan 2010 00:00:00 GMT\r\n\r\n",
           &cc, &H);
  expect(http_cc_freshness(&cc, now) == 86400L); /* capped at one day */
  http_headers_free(&H);

  /* --- current age (RFC 7234 §4.2.3): Age header + residence time. */
  test("current age calculation");
  parse_cc("HTTP/1.1 200 OK\r\nAge: 100\r\n\r\n", &cc, &H);
  /* stored 50s ago, was already 100s old on arrival -> 150s now. */
  expect(http_cc_current_age(&cc, 1000, 1050) == 150L);
  http_headers_free(&H);

  /* apparent age from Date (no Age header): received 200s after Date. */
  parse_cc("HTTP/1.1 200 OK\r\nDate: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n", &cc, &H);
  {
    long date = 1609459200L;
    long stored = date + 200; /* received 200s after the Date */
    expect(http_cc_current_age(&cc, stored, stored) == 200L);
    expect(http_cc_current_age(&cc, stored, stored + 30) == 230L);
  }
  http_headers_free(&H);

  /* --- a fresh-vs-stale end to end: max-age=300, stored now. */
  test("fresh and stale responses");
  parse_cc("HTTP/1.1 200 OK\r\nCache-Control: max-age=300\r\n\r\n", &cc, &H);
  {
    long lifetime = http_cc_freshness(&cc, now);
    expect(http_cc_current_age(&cc, now, now + 100) < lifetime);  /* fresh  */
    expect(http_cc_current_age(&cc, now, now + 400) >= lifetime); /* stale  */
  }
  http_headers_free(&H);

  /* --- validators surface for conditional requests. */
  test("cache validators");
  parse_cc("HTTP/1.1 200 OK\r\nETag: \"abc123\"\r\n"
           "Last-Modified: Fri, 01 Jan 2021 00:00:00 GMT\r\n\r\n",
           &cc, &H);
  expect(cc.etag && strcmp(cc.etag, "\"abc123\"") == 0);
  expect(cc.last_mod_str && strcmp(cc.last_mod_str, "Fri, 01 Jan 2021 00:00:00 GMT") == 0);
  http_headers_free(&H);

  /* --- Vary names and secondary keys. */
  test("Vary key from a response and request");
  {
    struct http_vary v;
    http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: Accept-Encoding, User-Agent\r\n\r\n");
    expect(http_vary_parse(&v, &H) == 0);
    char *a = http_vary_key(&v, "User-Agent: x  y\r\nAccept-Encoding: GZIP\r\n");
    char *b = http_vary_key(&v, "accept-encoding: GZIP\r\nuser-agent:  x y \r\n");
    char *c = http_vary_key(&v, "accept-encoding: GZIP\r\n");
    expect(http_vary_key_match(a, b));
    expect(!http_vary_key_match(a, c));
    free(a);
    free(b);
    free(c);
    http_vary_free(&v);
    http_headers_free(&H);
  }

  return 0;
}
