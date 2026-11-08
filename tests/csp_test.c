/* Host unit test for CSP external <script src> nonce / 'strict-dynamic' admission.
 * Build+run:
 *
 *   cc -std=c11 -Iscripts -Iuser/lib/curl -Iuser/apps/browser -Iuser/lib/crypto \
 *      -Iuser/lib/base64 tests/csp_test.c user/apps/browser/features/csp/csp.c \
 *      user/lib/curl/url.c user/lib/crypto/crypto.c user/lib/base64/base64.c \
 *      -o /tmp/csp_test && /tmp/csp_test
 */
#include "features/csp/csp.h"
#define OWOS_SUITE_TEST
#include "test.h"
#include <stdio.h>
#include <string.h>

/* Run an external <script src> through the policy set as a parser-inserted element. */
static int script_ok(const char *policy, const char *url, const char *page, const char *nonce, int parser_inserted)
{
  csp *c = csp_new();
  csp_add(c, policy);
  char out[1024], reason[64];
  int r = csp_check_script_el(c, url, page, nonce, parser_inserted, out, sizeof out, reason, sizeof reason);
  csp_free(c);
  return r;
}

static const char *page = "https://cleverpush.com/";
static const char *cf =
    "https://cleverpush.com/cdn-cgi/challenge-platform/h/b/orchestrate/chl_page/v1?ray=a169e1cd8846986b";

/* A nonce'd script under a nonce-only script-src must be admitted. */
static void check_nonce(void)
{
  expect(script_ok("script-src 'nonce-abc123'", cf, page, "abc123", 1));

  /* Wrong / missing nonce is still refused (no host-source matches). */
  expect(!script_ok("script-src 'nonce-abc123'", cf, page, "zzz", 1));
  expect(!script_ok("script-src 'nonce-abc123'", cf, page, 0, 1));
}

/* strict-dynamic: nonce'd parser scripts and injected scripts admitted; nonce-less parser ones not. */
static void check_strict_dynamic(void)
{
  expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, "n1", 1));
  expect(!script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, 0, 1));
  expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", "https://cdn.example.com/x.js", page, 0, 0));

  /* Leniency: a matching host-source admits the URL without consulting strict-dynamic. */
  expect(script_ok("script-src 'self' 'strict-dynamic'", cf, page, 0, 1));
}

/* Plain host-source path is unchanged: 'self' admits same-origin, blocks cross. */
static void check_host_source(void)
{
  expect(script_ok("script-src 'self'", cf, page, 0, 1));
  expect(!script_ok("script-src 'self'", "https://evil.example.com/x.js", page, 0, 1));
}

/* Multi-policy: every policy must pass; host-only 'self' blocks the cross-origin URL. */
static void check_multiple_policies(void)
{
  expect(!script_ok("script-src 'nonce-abc123'; script-src 'self'", "https://cdn.example.com/x.js", page, "abc123", 1));
}

/* No CSP / empty policy => allowed. */
static void check_empty_policy(void) { expect(script_ok("", cf, page, 0, 1)); }

int main(void)
{
  test("nonce authorization", check_nonce);
  test("strict-dynamic authorization", check_strict_dynamic);
  test("host-source authorization", check_host_source);
  test("multiple policies", check_multiple_policies);
  test("empty policy", check_empty_policy);
  return 0;
}
