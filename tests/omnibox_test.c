/* Host unit test for address-bar URL/search classification (user/apps/browser/features/omnibox/omnibox.c).
 *
 *   cc -std=gnu11 -Itests -Iuser/lib -Iuser/apps/browser tests/omnibox_test.c tests/test_runner.c \
 *      -o /tmp/omnibox_test && /tmp/omnibox_test
 */
#define OWOS_SUITE_TEST
#include "test.h"
#include <string.h>

#include "../user/apps/browser/features/omnibox/omnibox.c"

static char out[2048];

static const char *target(const char *typed)
{
  omnibox_target(typed, out, sizeof out);
  return out;
}

static bool url(const char *s) { return omnibox_is_url(s, (int)strlen(s)); }

static void test_urls_pass_through(void)
{
  expect_string(target("https://example.com/a b"), "https://example.com/a b");
  expect_string(target("  example.com  "), "example.com");
  expect_string(target("example.com/path?q=1#x"), "example.com/path?q=1#x");
  expect(url("http://localhost:8080/"));
  expect(url("ftp://files.example"));
  expect(url("localhost"));
  expect(url("LOCALHOST:3000/api"));
  expect(url("10.0.2.2:8080"));
  expect(url("192.168.1.1"));
  expect(url("sub.example.co.uk"));
  expect(url("xn--bcher-kva.example"));
  expect(url("bücher.example"));
  expect(url("[::1]"));
  expect(url("[::1]:8080/x"));
  expect(url("devbox:8080"));
  expect(url("file:///home/root/index.html"));
  expect(url("data:text/html,<p>hello world</p>"));
  expect(url("blob:https://a.example/1234"));
}

static void test_search_terms(void)
{
  expect(!url("cats"));
  expect(!url("what is node.js"));
  expect(!url("3.14"));
  expect(!url("1.2.3"));
  expect(!url("e.g."));
  expect(!url(".hidden"));
  expect(!url("a..b"));
  expect(!url("me@example.com"));
  expect(!url("c:foo"));
  expect(!url("host:"));
  expect(!url(":8080"));
  expect(!url("host:123456"));
  expect(!url("[::1"));
  expect(!url("[::1]x"));
  expect(!url("why?"));
  expect(!url("a_b.example"));
}

static void test_search_url_encoding(void)
{
  expect_string(target("cats"), OMNIBOX_SEARCH_PREFIX "cats");
  expect_string(target("  hello   world\t"), OMNIBOX_SEARCH_PREFIX "hello+world");
  expect_string(target("c++ & a=b?"), OMNIBOX_SEARCH_PREFIX "c%2B%2B+%26+a%3Db%3F");
  expect_string(target("what is node.js"), OMNIBOX_SEARCH_PREFIX "what+is+node.js");
  expect_string(target("café"), OMNIBOX_SEARCH_PREFIX "caf%C3%A9");
  expect_string(target("50% off #1"), OMNIBOX_SEARCH_PREFIX "50%25+off+%231");
}

static void test_blank_input(void)
{
  strcpy(out, "untouched");
  expect_eq(omnibox_target("", out, sizeof out), 0);
  expect_string(out, "");
  expect_eq(omnibox_target(" \t ", out, sizeof out), 0);
  expect_eq(omnibox_target(0, out, sizeof out), 0);
}

static void test_truncation(void)
{
  char small[sizeof OMNIBOX_SEARCH_PREFIX + 4];
  /* room for prefix + 4 bytes + NUL: "ab" fits, then "%26" would need 3 more and is dropped whole */
  int n = omnibox_target("ab cd", small, sizeof small);
  expect_string(small, OMNIBOX_SEARCH_PREFIX "ab+c");
  expect_eq(n, (int)strlen(small));
  n = omnibox_target("ab&cd", small, sizeof small);
  expect_string(small, OMNIBOX_SEARCH_PREFIX "ab");
  expect_eq(n, (int)strlen(small));
  char tiny[8];
  expect_eq(omnibox_target("cats", tiny, sizeof tiny), 0);
  expect_string(tiny, "");
  n = omnibox_target("example.com", tiny, sizeof tiny);
  expect_string(tiny, "example");
  expect_eq(n, 7);
}

int main(void)
{
  test("URLs pass through trimmed", test_urls_pass_through);
  test("non-URLs are search terms", test_search_terms);
  test("search terms are form-encoded", test_search_url_encoding);
  test("blank input navigates nowhere", test_blank_input);
  test("truncation never splits an escape", test_truncation);
  return 0;
}
