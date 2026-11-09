/* url_test.c: host test for user/lib/curl/url.c, covering bracketed IPv6 hosts. Run with `make test`. */
#define OWOS_SUITE_TEST
#include "owos.h"
#include "url.h"

static void test_ipv6_host(void)
{
  struct purl u;
  expect(parse_url("http://[2001:db8::1]:8080/a/b?q=1", &u));
  expect_string(u.host, "2001:db8::1");
  expect(u.port == 8080);
  expect(!u.tls);
  expect_string(u.path, "/a/b?q=1");

  expect(parse_url("https://[::1]/", &u));
  expect_string(u.host, "::1");
  expect(u.port == 443 && u.tls);

  expect(parse_url("http://[fe80::1]", &u));
  expect_string(u.host, "fe80::1");
  expect(u.port == 80);
  expect_string(u.path, "/");
}

static void test_ipv4_and_names_unchanged(void)
{
  struct purl u;
  expect(parse_url("http://example.com:81/x", &u));
  expect_string(u.host, "example.com");
  expect(u.port == 81);
  expect(!parse_url("http://[]/", &u));
}

static void test_resolve_keeps_brackets(void)
{
  char out[128];
  resolve_url("http://[2001:db8::1]:8080/dir/page", "other", out, sizeof out);
  expect_string(out, "http://[2001:db8::1]:8080/dir/other");
  resolve_url("https://[::1]/dir/page", "/root", out, sizeof out);
  expect_string(out, "https://[::1]/root");
}

static void test_bracket_host(void)
{
  char buf[64];
  expect_string(url_bracket_host("2001:db8::1", buf, sizeof buf), "[2001:db8::1]");
  const char *name = "example.com";
  expect(url_bracket_host(name, buf, sizeof buf) == name);
}

int main(void)
{
  test("bracketed IPv6 hosts", test_ipv6_host);
  test("IPv4 and name hosts", test_ipv4_and_names_unchanged);
  test("relative resolution keeps the brackets", test_resolve_keeps_brackets);
  test("host bracketing for Host headers", test_bracket_host);
  return 0;
}
