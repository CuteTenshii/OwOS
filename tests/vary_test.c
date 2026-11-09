/* Host unit test for HTTP Vary support (RFC 9111 4.1): the secondary-key helpers in
 * user/lib/curl/httpcache.c and the variant-aware cache in
 * user/apps/browser/features/cache/cache.c. Run through tests/run-tests.sh, which builds cache.c
 * against tests/vary_fs.c (a host stand-in for the owofs calls) with CACHE_DIR in a temp dir. */
#include <dirent.h>
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "features/cache/cache.h"
#include "httpcache.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define URL "https://v.test/res"
#define BIG (4 * 1024 * 1024)

static char *key_for(const char *resp_hdrs, const char *req_hdrs)
{
  struct http_headers H;
  struct http_vary v;
  char blk[512];
  snprintf(blk, sizeof blk, "HTTP/1.1 200 OK\r\n%s\r\n", resp_hdrs);
  http_headers_parse(&H, blk);
  http_vary_parse(&v, &H);
  char *k = http_vary_key(&v, req_hdrs);
  http_vary_free(&v);
  http_headers_free(&H);
  return k;
}

static bool same_variant(const char *resp_hdrs, const char *req_a, const char *req_b)
{
  char *a = key_for(resp_hdrs, req_a);
  char *b = key_for(resp_hdrs, req_b);
  bool m = http_vary_key_match(a, b);
  free(a);
  free(b);
  return m;
}

static void put(const char *url, const char *req, const char *rhdr, const char *body, long now)
{
  char blk[1024];
  snprintf(blk, sizeof blk, "HTTP/1.1 200 OK\r\n%s\r\n", rhdr);
  cache_store("GET", url, req, 200, now, blk, "text/plain", (const unsigned char *)body, (int)strlen(body));
}

/* Lookup; body prefix copied into out[64] (NUL-terminated), conditional lines into cond (if non-NULL). */
static int look(const char *url, const char *req, long now, char *out, char *cond)
{
  unsigned char *b;
  int n;
  int v = cache_lookup("GET", url, req, now, &b, &n, 0, 0, 0, 0, cond, cond ? 512 : 0);
  out[0] = '\0';
  if (b) {
    n = n < 63 ? n : 63;
    memcpy(out, b, (size_t)n);
    out[n] = '\0';
    free(b);
  }
  return v;
}

static int count_files(void)
{
  int n = 0;
  DIR *d = opendir(CACHE_DIR);
  for (struct dirent *e; d && (e = readdir(d));)
    n += e->d_name[0] != '.';
  if (d)
    closedir(d);
  return n;
}

static void put_big(int i, long now)
{
  char *body = malloc(BIG + 1);
  memset(body, 'x' + (i & 1), BIG);
  body[BIG] = '\0';
  char url[64];
  snprintf(url, sizeof url, "https://big.test/%d", i);
  put(url, "", "Cache-Control: max-age=10\r\n", body, now);
  free(body);
}

struct snap {
  char name[64];
  char *data;
  int len;
};

static char rq_e[5][32];

static char out[64], cond[512];

static void vary_parsing(void)
{
  struct http_headers H;
  struct http_vary v;
  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary:  Accept-Encoding ,\taccept-language,ACCEPT-ENCODING\r\n"
                         "Vary: Cookie\r\n\r\n");
  expect(http_vary_parse(&v, &H) == 0);
  expect(v.n == 3 && !v.star);
  expect_string(v.names[0], "accept-encoding");
  expect_string(v.names[1], "accept-language");
  expect_string(v.names[2], "cookie");
  http_vary_free(&v);
  http_headers_free(&H);

  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: Accept, *\r\n\r\n");
  http_vary_parse(&v, &H);
  expect(v.star);
  expect(http_vary_key(&v, "accept: a\r\n") == NULL);
  http_vary_free(&v);
  http_headers_free(&H);

  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: \r\n\r\n");
  http_vary_parse(&v, &H);
  expect(v.n == 0 && !v.star);
  http_vary_free(&v);
  http_headers_free(&H);

  struct http_vary a, b;
  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: A, B\r\n\r\n");
  http_vary_parse(&a, &H);
  http_headers_free(&H);
  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: b,a\r\n\r\n");
  http_vary_parse(&b, &H);
  http_headers_free(&H);
  expect(http_vary_same(&a, &b));
  http_vary_free(&b);
  http_headers_parse(&H, "HTTP/1.1 200 OK\r\nVary: A\r\n\r\n");
  http_vary_parse(&b, &H);
  http_headers_free(&H);
  expect(!http_vary_same(&a, &b));
  http_vary_free(&a);
  http_vary_free(&b);
}

static void vary_secondary_key(void)
{
  {
    const char *ae = "Vary: Accept-Encoding\r\n";
    expect(same_variant(ae, "Accept-Encoding: gzip, br\r\n", "accept-encoding:   GZIP,BR\r\n"));
    expect(same_variant(ae, "ACCEPT-ENCODING: gzip\r\nX: 1\r\n", "x: 2\r\naccept-encoding: gzip\r\n"));
    expect(!same_variant(ae, "Accept-Encoding: gzip\r\n", "Accept-Encoding: br\r\n"));
    expect(!same_variant(ae, "X-Accept-Encoding: gzip\r\n", "Accept-Encoding: gzip\r\n"));
    expect(same_variant(ae, "Accept-Encoding: a\r\nAccept-Encoding: b\r\n", "Accept-Encoding: a, b\r\n"));

    const char *ck = "Vary: Cookie\r\n";
    expect(!same_variant(ck, "Cookie: s=Abc\r\n", "Cookie: s=abc\r\n"));
    expect(same_variant(ck, "Cookie:   s=abc   d=1 \r\n", "Cookie: s=abc d=1\r\n"));

    expect(!same_variant("Vary: X-A\r\n", "Y: 1\r\n", "X-A:\r\n"));
    expect(same_variant("Vary: X-A\r\n", "Y: 1\r\n", "Z: 2\r\n"));
    expect(same_variant("Vary: X-A\r\n", "X-A:\r\n", "X-A:   \r\n"));
    expect(!same_variant("Vary: X-A\r\n", "X-A: 1\r\n", "X-A:\r\n"));

    expect(same_variant("Vary: A, B\r\n", "a: 1\r\nb: 2\r\n", "b: 2\r\na: 1\r\n"));
    expect(!same_variant("Vary: A, B\r\n", "a: 1\r\nb: 2\r\n", "a: 1\r\nb: 3\r\n"));
    expect(!same_variant("Vary: A, B\r\n", "a: 12\r\nb: 3\r\n", "a: 1\r\nb: 23\r\n"));

    expect(!same_variant("Vary: *\r\n", "a: 1\r\n", "a: 1\r\n"));
    expect(same_variant("X: 1\r\n", "a: 1\r\n", "a: 2\r\n"));
    char *none = key_for("X: 1\r\n", "a: 1\r\n");
    expect_string(none, "");
    free(none);
    expect(!http_vary_key_match(NULL, NULL));
    expect(!http_vary_key_match("a", NULL));
  }

  cache_load();
}

static void cache_keeps_one_variant_per_request_key(void)
{
  const char *ae = "Cache-Control: max-age=1000\r\nVary: Accept-Encoding\r\n";
  put(URL, "Accept-Encoding: gzip\r\n", ae, "GZIP-BODY", 100);
  put(URL, "Accept-Encoding: br\r\n", ae, "BR-BODY", 100);
  expect(look(URL, "Accept-Encoding: gzip\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "GZIP-BODY");
  expect(look(URL, "accept-encoding:  BR \r\nUser-Agent: z\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "BR-BODY");
  expect(look(URL, "Accept-Encoding: identity\r\n", 200, out, NULL) == CACHE_MISS);
  expect(look(URL, "", 200, out, NULL) == CACHE_MISS);

  put(URL, "Accept-Encoding: gzip\r\n", ae, "GZIP-NEW", 150);
  expect(look(URL, "Accept-Encoding: gzip\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "GZIP-NEW");
  expect(look(URL, "Accept-Encoding: br\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "BR-BODY");
}

static void vary_is_never_served(void)
{
  put("https://v.test/star", "a: 1\r\n", "Cache-Control: max-age=1000\r\nVary: *\r\n", "STAR", 100);
  expect(look("https://v.test/star", "a: 1\r\n", 101, out, NULL) == CACHE_MISS);
  expect(look("https://v.test/star", "", 101, out, NULL) == CACHE_MISS);
}

static void absent_request_header_differs_from_empty(void)
{
  const char *h = "Cache-Control: max-age=1000\r\nVary: X-A\r\n";
  put("https://v.test/absent", "Y: 1\r\n", h, "ABSENT", 100);
  expect(look("https://v.test/absent", "Z: 1\r\n", 101, out, NULL) == CACHE_FRESH);
  expect(look("https://v.test/absent", "X-A:\r\n", 101, out, NULL) == CACHE_MISS);
  expect(look("https://v.test/absent", "X-A: 1\r\n", 101, out, NULL) == CACHE_MISS);
}

static void revalidation_targets_the_matching_variant(void)
{
  const char *u = "https://v.test/reval";
  put(u, "Accept-Language: en\r\n", "Cache-Control: max-age=10\r\nETag: \"en1\"\r\nVary: Accept-Language\r\n", "EN",
      100);
  put(u, "Accept-Language: fr\r\n", "Cache-Control: max-age=10\r\nETag: \"fr1\"\r\nVary: Accept-Language\r\n", "FR",
      100);
  expect(look(u, "Accept-Language: fr\r\n", 500, out, cond) == CACHE_REVALIDATE);
  expect(strstr(cond, "\"fr1\"") && !strstr(cond, "\"en1\""));
  expect(look(u, "Accept-Language: en\r\n", 500, out, cond) == CACHE_REVALIDATE);
  expect(strstr(cond, "\"en1\"") && !strstr(cond, "\"fr1\""));

  unsigned char *b;
  int n;
  const char *r304 = "HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=1000\r\n\r\n";
  expect(cache_revalidated(u, "Accept-Language: fr\r\n", 500, r304, &b, &n, 0, 0, 0, 0));
  expect(n == 2 && !memcmp(b, "FR", 2));
  free(b);
  expect(look(u, "Accept-Language: fr\r\n", 600, out, cond) == CACHE_FRESH);
  expect_string(out, "FR");
  expect(look(u, "Accept-Language: en\r\n", 600, out, cond) == CACHE_REVALIDATE);
  expect(!cache_revalidated(u, "Accept-Language: de\r\n", 600, r304, &b, &n, 0, 0, 0, 0));
  expect(b == NULL);
}

static void a_changed_vary_field_list_drops_older_variants(void)
{
  const char *u = "https://v.test/shift";
  put(u, "Accept-Encoding: gzip\r\n", "Cache-Control: max-age=1000\r\nVary: Accept-Encoding\r\n", "OLD", 100);
  expect(look(u, "Accept-Encoding: gzip\r\n", 101, out, NULL) == CACHE_FRESH);
  put(u, "Accept-Language: en\r\n", "Cache-Control: max-age=1000\r\nVary: Accept-Language\r\n", "NEW", 102);
  expect(look(u, "Accept-Encoding: gzip\r\n", 103, out, NULL) == CACHE_MISS);
  expect(look(u, "Accept-Language: en\r\n", 103, out, NULL) == CACHE_FRESH);
  expect_string(out, "NEW");
  put(u, "", "Cache-Control: max-age=1000\r\n", "PLAIN", 104);
  expect(look(u, "Accept-Language: en\r\n", 105, out, NULL) == CACHE_FRESH);
  expect_string(out, "PLAIN");
}

static void byte_accounting_survives_variant_replacement(void)
{
  put("https://v.test/keep", "", "Cache-Control: max-age=100000\r\n", "KEEP", 100);
  char *body = malloc(1024 * 1024 + 1);
  memset(body, 'r', 1024 * 1024);
  body[1024 * 1024] = '\0';
  for (int i = 0; i < 40; i++)
    put("https://v.test/churn", "Accept: a\r\n", "Cache-Control: max-age=100000\r\nVary: Accept\r\n", body, 100 + i);
  free(body);
  expect(look("https://v.test/keep", "", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "KEEP");
}

static void eviction_drops_the_least_recently_used_variant(void)
{
  const char *h = "Cache-Control: max-age=100000\r\nVary: Accept\r\n";
  char *body = malloc(BIG + 1);
  for (int i = 0; i < 5; i++)
    snprintf(rq_e[i], sizeof rq_e[i], "Accept: e%d\r\n", i);
  for (int i = 0; i < 3; i++) {
    memset(body, 'a' + i, BIG);
    body[BIG] = '\0';
    put("https://v.test/lru", rq_e[i], h, body, 1000 + i);
  }
  expect(look("https://v.test/lru", rq_e[0], 1100, out, NULL) == CACHE_FRESH);
  for (int i = 3; i < 5; i++) {
    memset(body, 'a' + i, BIG);
    body[BIG] = '\0';
    put("https://v.test/lru", rq_e[i], h, body, 1000 + i);
  }
  free(body);
  expect(look("https://v.test/lru", rq_e[1], 1100, out, NULL) == CACHE_MISS);
  expect(look("https://v.test/lru", rq_e[0], 1100, out, NULL) == CACHE_FRESH);
  expect(look("https://v.test/lru", rq_e[2], 1100, out, NULL) == CACHE_FRESH);
  expect(look("https://v.test/lru", rq_e[4], 1100, out, NULL) == CACHE_FRESH);
}

static void variants_persist_with_unique_files_across_a_reload(void)
{
  for (int i = 0; i < 5; i++) /* stale and validator-less: dropped on lookup */
    look("https://v.test/lru", rq_e[i], 10000000, out, NULL);
  cache_flush();
  int before = count_files();
  const char *u = "https://p.test/r";
  const char *h = "Cache-Control: max-age=1000\r\nVary: Accept-Encoding\r\n";
  put(u, "Accept-Encoding: gzip\r\n", "Cache-Control: max-age=1000\r\nETag: \"g1\"\r\nVary: Accept-Encoding\r\n", "GZ",
      100);
  put(u, "Accept-Encoding: br\r\n", h, "BR", 100);
  put("https://p.test/plain", "", "Cache-Control: max-age=1000\r\nETag: \"pl\"\r\n", "PL", 100);
  cache_flush();
  expect(count_files() == before + 3);

  struct snap *snaps = 0;
  int ns = 0;
  DIR *d = opendir(CACHE_DIR);
  for (struct dirent *e; d && (e = readdir(d));) {
    if (e->d_name[0] == '.')
      continue;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", CACHE_DIR, e->d_name);
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    snaps = realloc(snaps, (size_t)(ns + 1) * sizeof *snaps);
    snprintf(snaps[ns].name, sizeof snaps[ns].name, "%s", e->d_name);
    snaps[ns].data = malloc((size_t)len + 1);
    snaps[ns].len = (int)fread(snaps[ns].data, 1, (size_t)len, f);
    fclose(f);
    ns++;
  }
  if (d)
    closedir(d);

  for (int i = 0; i < 4; i++) /* evict every small entry from memory and disk */
    put_big(i, 100);
  cache_flush();
  expect(count_files() == 4);

  for (int i = 0; i < ns; i++) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", CACHE_DIR, snaps[i].name);
    FILE *f = fopen(path, "wb");
    fwrite(snaps[i].data, 1, (size_t)snaps[i].len, f);
    fclose(f);
    free(snaps[i].data);
  }
  free(snaps);
  char stale[512];
  snprintf(stale, sizeof stale, "%s/0123456789abcdef", CACHE_DIR);
  FILE *f = fopen(stale, "wb");
  fputs("OWCACHE1\nhttps://old.test/\n200 100 0 3\ntext/plain\nabc", f);
  fclose(f);

  for (int i = 0; i < 4; i++) { /* expire the bigs to free their bytes */
    char bu[64];
    snprintf(bu, sizeof bu, "https://big.test/%d", i);
    expect(look(bu, "", 5000, out, NULL) == CACHE_MISS);
  }
  cache_flush();
  cache_load();

  expect(access(stale, 0) != 0);
  expect(look(u, "Accept-Encoding: gzip\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "GZ");
  expect(look(u, "Accept-Encoding: br\r\n", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "BR");
  expect(look(u, "Accept-Encoding: deflate\r\n", 200, out, NULL) == CACHE_MISS);
  expect(look(u, "Accept-Encoding: gzip\r\n", 1500, out, cond) == CACHE_REVALIDATE);
  expect(strstr(cond, "\"g1\"") != NULL);
  expect(look(u, "Accept-Encoding: br\r\n", 1500, out, cond) == CACHE_MISS);
  expect(look("https://p.test/plain", "", 200, out, NULL) == CACHE_FRESH);
  expect_string(out, "PL");
  expect(look("https://p.test/plain", "Accept-Encoding: gzip\r\n", 200, out, NULL) == CACHE_FRESH);
  expect(look("https://p.test/plain", "", 1500, out, cond) == CACHE_REVALIDATE);
  expect(strstr(cond, "\"pl\"") != NULL);
}

int main(void)
{
  test("Vary parsing", vary_parsing);
  test("Vary secondary key", vary_secondary_key);
  test("cache keeps one variant per request key", cache_keeps_one_variant_per_request_key);
  test("Vary: * is never served", vary_is_never_served);
  test("absent request header differs from empty", absent_request_header_differs_from_empty);
  test("revalidation targets the matching variant", revalidation_targets_the_matching_variant);
  test("a changed Vary field list drops older variants", a_changed_vary_field_list_drops_older_variants);
  test("byte accounting survives variant replacement", byte_accounting_survives_variant_replacement);
  test("eviction drops the least recently used variant", eviction_drops_the_least_recently_used_variant);
  test("variants persist with unique files across a reload", variants_persist_with_unique_files_across_a_reload);
  return 0;
}
