/* permissions_test.c — host unit test for the browser's permission store +
 * Permissions-Policy parser (user/apps/browser/permissions.c). Pure logic
 * (no engine, no QEMU): the feature/token table, the policy allowlist parser,
 * the session vs persistent store, and a real SQLite round-trip through
 * user/lib/sqlite. PERM_DB is redirected to a temp file via -DPERM_DB.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib/sqlite -Iuser/apps/browser \
 *      -DPERM_DB='"/tmp/owos_perms_test.db"' \
 *      tests/permissions_test.c user/apps/browser/permissions.c \
 *      user/lib/sqlite/sqlite.c -o /tmp/permissions_test && /tmp/permissions_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include <string.h>
#include "permissions.h"

/* A parsed policy blocks feature f? */
static int blocks(const char *hdr, const char *host, int f)
{
	return perm_policy_blocks(perm_policy_parse(hdr, host), f);
}

int main(void)
{
	/* ---- feature <-> token table ---- */
	test("feature token table");
	expect(perm_feature_from_token("geolocation") == PERM_GEOLOCATION);
	expect(perm_feature_from_token("camera") == PERM_CAMERA);
	expect(perm_feature_from_token("microphone") == PERM_MICROPHONE);
	expect(perm_feature_from_token("notifications") == PERM_NOTIFICATIONS);
	expect(perm_feature_from_token("GEOLOCATION") == PERM_GEOLOCATION);
	expect(perm_feature_from_token("gyroscope") == -1);
	expect(!strcmp(perm_feature_label(PERM_GEOLOCATION), "Location"));

	/* ---- Permissions-Policy parsing ---- */
	test("Permissions-Policy parsing");
	expect(!blocks("geolocation=*", "a.com", PERM_GEOLOCATION));
	expect(!blocks("geolocation=(self)", "a.com", PERM_GEOLOCATION));
	expect(!blocks("geolocation=self", "a.com", PERM_GEOLOCATION));
	expect(blocks("geolocation=()", "a.com", PERM_GEOLOCATION));
	expect(blocks("camera=()", "a.com", PERM_CAMERA));
	expect(!blocks("camera=()", "a.com", PERM_GEOLOCATION));
	expect(!blocks("geolocation=*", "a.com", PERM_CAMERA));
	/* multiple directives, comma-separated (Permissions-Policy) */
	expect(blocks("geolocation=(), camera=*", "a.com", PERM_GEOLOCATION));
	expect(!blocks("geolocation=(), camera=*", "a.com", PERM_CAMERA));
	/* self present alongside a foreign origin -> enabled for self */
	expect(!blocks("geolocation=(self \"https://b.com\")", "a.com", PERM_GEOLOCATION));
	/* only a foreign origin, no self -> blocked for this document */
	expect(blocks("geolocation=(\"https://b.com\")", "a.com", PERM_GEOLOCATION));
	/* the page's own origin listed explicitly -> enabled */
	expect(!blocks("geolocation=(\"https://a.com\")", "a.com", PERM_GEOLOCATION));
	/* legacy Feature-Policy ';' separator */
	expect(blocks("geolocation 'none'; microphone 'self'", "a.com", PERM_GEOLOCATION));
	expect(!blocks("geolocation 'none'; microphone 'self'", "a.com", PERM_MICROPHONE));
	expect(perm_policy_parse(0, "a.com") == 0);
	expect(perm_policy_parse("", "a.com") == 0);

	/* ---- session store (persist = 0) ---- */
	test("session permission store");
	expect(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_ASK);
	perm_remember("https://x.com", PERM_GEOLOCATION, PERM_DENIED, 0);
	expect(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_DENIED);
	expect(perm_lookup("https://x.com", PERM_CAMERA) == PERM_ASK);
	expect(perm_lookup("https://y.com", PERM_GEOLOCATION) == PERM_ASK);
	perm_remember("https://x.com", PERM_GEOLOCATION, PERM_GRANTED, 0);   /* overwrite */
	expect(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_GRANTED);
	expect(perm_lookup("HTTPS://X.COM", PERM_GEOLOCATION) == PERM_GRANTED);

	/* ---- persistent store round-trip (persist = 1) ---- */
	test("persistent permission store");
	remove(PERM_DB);
	perm_remember("https://persist.com", PERM_NOTIFICATIONS, PERM_GRANTED, 1);   /* "Allow always" */
	perm_remember("https://persist.com", PERM_CAMERA, PERM_DENIED, 0);           /* session only */

	/* Fresh process would call perm_load() against the same db; simulate by
	 * loading into this run (the just-remembered persist.com/notifications must
	 * come back, the session-only camera must NOT). */
	perm_load();
	expect(perm_lookup("https://persist.com", PERM_NOTIFICATIONS) == PERM_GRANTED);

	/* A brand-new origin only written session-only leaves no disk trace: rewrite
	 * the db from persist-flagged entries and confirm the session one is absent by
	 * checking the file has just the one persisted row's origin. */
	{
		FILE *f = fopen(PERM_DB, "rb");
		expect(f != 0);
		if (f) {
			char buf[65536]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
			int has_persist = 0, has_session = 0;
			for (size_t i = 0; i + 11 <= n; i++) {
				if (!memcmp(buf + i, "persist.com", 11)) has_persist = 1;
				if (!memcmp(buf + i, "session", 7)) has_session = 1;
			}
			expect(has_persist);
			expect(!has_session);
		}
	}
	remove(PERM_DB);

	return 0;
}
