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

int main(void)
{
  const char *page = "https://cleverpush.com/";
  const char *cf = "https://cleverpush.com/cdn-cgi/challenge-platform/h/b/orchestrate/chl_page/v1?ray=a169e1cd8846986b";

  /* A nonce'd script under a nonce-only script-src must be admitted. */
  test("nonce authorization");
  expect(script_ok("script-src 'nonce-abc123'", cf, page, "abc123", 1));

  /* Wrong / missing nonce is still refused (no host-source matches). */
  expect(!script_ok("script-src 'nonce-abc123'", cf, page, "zzz", 1));
  expect(!script_ok("script-src 'nonce-abc123'", cf, page, 0, 1));

  /* strict-dynamic: nonce'd parser scripts and injected scripts admitted; nonce-less parser ones not. */
  test("strict-dynamic authorization");
  expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, "n1", 1));
  expect(!script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, 0, 1));
  expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", "https://cdn.example.com/x.js", page, 0, 0));

  /* Leniency: a matching host-source admits the URL without consulting strict-dynamic. */
  expect(script_ok("script-src 'self' 'strict-dynamic'", cf, page, 0, 1));

  /* Plain host-source path is unchanged: 'self' admits same-origin, blocks cross. */
  test("host-source authorization");
  expect(script_ok("script-src 'self'", cf, page, 0, 1));
  expect(!script_ok("script-src 'self'", "https://evil.example.com/x.js", page, 0, 1));

  /* Multi-policy: every policy must pass; host-only 'self' blocks the cross-origin URL. */
  test("multiple policies");
  expect(!script_ok("script-src 'nonce-abc123'; script-src 'self'", "https://cdn.example.com/x.js", page, "abc123", 1));

  /* No CSP / empty policy => allowed. */
  test("empty policy");
  expect(script_ok("", cf, page, 0, 1));
  return 0;
}
