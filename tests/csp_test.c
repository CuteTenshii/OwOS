/* csp_test.c — host unit test for the browser CSP engine, focused on external
 * <script src> nonce / 'strict-dynamic' admission (the Cloudflare "just a moment"
 * challenge-platform case). Build+run:
 *
 *   cc -std=c11 -Iscripts -Iuser/lib/curl -Iuser/apps/browser -Iuser/lib/crypto \
 *      -Iuser/lib/base64 tests/csp_test.c user/apps/browser/csp.c \
 *      user/lib/curl/url.c user/lib/crypto/crypto.c user/lib/base64/base64.c \
 *      -o /tmp/csp_test && /tmp/csp_test
 */
#include "csp.h"
#define OWOS_SUITE_TEST
#include "test.h"
#include <stdio.h>
#include <string.h>

/* Run an external <script src> through the policy set as a parser-inserted element. */
static int script_ok(const char *policy, const char *url, const char *page,
                     const char *nonce, int parser_inserted)
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
	const char *cf   = "https://cleverpush.com/cdn-cgi/challenge-platform/h/b/orchestrate/chl_page/v1?ray=a169e1cd8846986b";

	/* The reported bug: a nonce'd challenge-platform script under a nonce-only
	 * script-src was refused because the host-source check ignored the nonce. */
	test("nonce authorization");
	expect(script_ok("script-src 'nonce-abc123'", cf, page, "abc123", 1));

	/* Wrong / missing nonce is still refused (no host-source matches). */
	expect(!script_ok("script-src 'nonce-abc123'", cf, page, "zzz", 1));
	expect(!script_ok("script-src 'nonce-abc123'", cf, page, 0, 1));

	/* strict-dynamic: a nonce'd parser-inserted script is admitted; a nonce-less
	 * parser-inserted script is not; a script-injected one (parser_inserted=0) is. */
	test("strict-dynamic authorization");
	expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, "n1", 1));
	expect(!script_ok("script-src 'nonce-n1' 'strict-dynamic'", cf, page, 0, 1));
	expect(script_ok("script-src 'nonce-n1' 'strict-dynamic'", "https://cdn.example.com/x.js", page, 0, 0));

	/* Deliberate leniency: when a host-source already admits the URL we allow it
	 * without consulting strict-dynamic. A spec-strict browser ignores 'self'
	 * under strict-dynamic and would block this nonce-less parser script, but being
	 * stricter than the host-source check only risks breaking pages, never helps a
	 * single-user browser — so a matching host-source wins. */
	expect(script_ok("script-src 'self' 'strict-dynamic'", cf, page, 0, 1));

	/* Plain host-source path is unchanged: 'self' admits same-origin, blocks cross. */
	test("host-source authorization");
	expect(script_ok("script-src 'self'", cf, page, 0, 1));
	expect(!script_ok("script-src 'self'", "https://evil.example.com/x.js", page, 0, 1));

	/* Multi-policy: the element must satisfy every policy. Nonce satisfies the first
	 * policy but the second (host-only 'self') blocks the cross-origin URL. */
	test("multiple policies");
	expect(!script_ok("script-src 'nonce-abc123'; script-src 'self'",
	                  "https://cdn.example.com/x.js", page, "abc123", 1));

	/* No CSP / empty policy => allowed. */
	test("empty policy");
	expect(script_ok("", cf, page, 0, 1));
	return 0;
}
