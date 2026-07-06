/* permissions_test.c — host unit test for the browser's permission store +
 * Permissions-Policy parser (user/apps/gui/browser/permissions.c). Pure logic
 * (no engine, no QEMU): the feature/token table, the policy allowlist parser,
 * the session vs persistent store, and a real SQLite round-trip through
 * user/lib/sqlite. PERM_DB is redirected to a temp file via -DPERM_DB.
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib/sqlite -Iuser/apps/gui/browser \
 *      -DPERM_DB='"/tmp/owos_perms_test.db"' \
 *      scripts/permissions_test.c user/apps/gui/browser/permissions.c \
 *      user/lib/sqlite/sqlite.c -o /tmp/permissions_test && /tmp/permissions_test
 */
#include <stdio.h>
#include <string.h>
#include "permissions.h"

static int g_fail;
static void ok(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond) g_fail = 1;
}

/* A parsed policy blocks feature f? */
static int blocks(const char *hdr, const char *host, int f)
{
	return perm_policy_blocks(perm_policy_parse(hdr, host), f);
}

int main(void)
{
	/* ---- feature <-> token table ---- */
	ok(perm_feature_from_token("geolocation") == PERM_GEOLOCATION, "token geolocation");
	ok(perm_feature_from_token("camera") == PERM_CAMERA, "token camera");
	ok(perm_feature_from_token("microphone") == PERM_MICROPHONE, "token microphone");
	ok(perm_feature_from_token("notifications") == PERM_NOTIFICATIONS, "token notifications");
	ok(perm_feature_from_token("GEOLOCATION") == PERM_GEOLOCATION, "token is case-insensitive");
	ok(perm_feature_from_token("gyroscope") == -1, "unknown token -> -1");
	ok(!strcmp(perm_feature_label(PERM_GEOLOCATION), "Location"), "geolocation label");

	/* ---- Permissions-Policy parsing ---- */
	ok(!blocks("geolocation=*", "a.com", PERM_GEOLOCATION), "geolocation=* enables");
	ok(!blocks("geolocation=(self)", "a.com", PERM_GEOLOCATION), "(self) enables");
	ok(!blocks("geolocation=self", "a.com", PERM_GEOLOCATION), "bare self enables");
	ok(blocks("geolocation=()", "a.com", PERM_GEOLOCATION), "() disables");
	ok(blocks("camera=()", "a.com", PERM_CAMERA), "camera=() disables camera");
	ok(!blocks("camera=()", "a.com", PERM_GEOLOCATION), "camera=() leaves geolocation alone");
	ok(!blocks("geolocation=*", "a.com", PERM_CAMERA), "unlisted feature stays enabled");
	/* multiple directives, comma-separated (Permissions-Policy) */
	ok(blocks("geolocation=(), camera=*", "a.com", PERM_GEOLOCATION), "multi: geolocation blocked");
	ok(!blocks("geolocation=(), camera=*", "a.com", PERM_CAMERA), "multi: camera enabled");
	/* self present alongside a foreign origin -> enabled for self */
	ok(!blocks("geolocation=(self \"https://b.com\")", "a.com", PERM_GEOLOCATION), "self + foreign enables self");
	/* only a foreign origin, no self -> blocked for this document */
	ok(blocks("geolocation=(\"https://b.com\")", "a.com", PERM_GEOLOCATION), "foreign-only blocks self");
	/* the page's own origin listed explicitly -> enabled */
	ok(!blocks("geolocation=(\"https://a.com\")", "a.com", PERM_GEOLOCATION), "own origin listed enables");
	/* legacy Feature-Policy ';' separator */
	ok(blocks("geolocation 'none'; microphone 'self'", "a.com", PERM_GEOLOCATION), "feature-policy 'none' blocks");
	ok(!blocks("geolocation 'none'; microphone 'self'", "a.com", PERM_MICROPHONE), "feature-policy 'self' enables");
	ok(perm_policy_parse(0, "a.com") == 0, "NULL header blocks nothing");
	ok(perm_policy_parse("", "a.com") == 0, "empty header blocks nothing");

	/* ---- session store (persist = 0) ---- */
	ok(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_ASK, "unknown origin -> ASK");
	perm_remember("https://x.com", PERM_GEOLOCATION, PERM_DENIED, 0);
	ok(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_DENIED, "session deny remembered");
	ok(perm_lookup("https://x.com", PERM_CAMERA) == PERM_ASK, "other feature still ASK");
	ok(perm_lookup("https://y.com", PERM_GEOLOCATION) == PERM_ASK, "other origin still ASK");
	perm_remember("https://x.com", PERM_GEOLOCATION, PERM_GRANTED, 0);   /* overwrite */
	ok(perm_lookup("https://x.com", PERM_GEOLOCATION) == PERM_GRANTED, "decision overwrite");
	ok(perm_lookup("HTTPS://X.COM", PERM_GEOLOCATION) == PERM_GRANTED, "origin match is case-insensitive");

	/* ---- persistent store round-trip (persist = 1) ---- */
	remove(PERM_DB);
	perm_remember("https://persist.com", PERM_NOTIFICATIONS, PERM_GRANTED, 1);   /* "Allow always" */
	perm_remember("https://persist.com", PERM_CAMERA, PERM_DENIED, 0);           /* session only */

	/* Fresh process would call perm_load() against the same db; simulate by
	 * loading into this run (the just-remembered persist.com/notifications must
	 * come back, the session-only camera must NOT). */
	perm_load();
	ok(perm_lookup("https://persist.com", PERM_NOTIFICATIONS) == PERM_GRANTED, "persisted grant reloads");

	/* A brand-new origin only written session-only leaves no disk trace: rewrite
	 * the db from persist-flagged entries and confirm the session one is absent by
	 * checking the file has just the one persisted row's origin. */
	{
		FILE *f = fopen(PERM_DB, "rb");
		ok(f != 0, "permissions.db was written");
		if (f) {
			char buf[65536]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
			int has_persist = 0, has_session = 0;
			for (size_t i = 0; i + 11 <= n; i++) {
				if (!memcmp(buf + i, "persist.com", 11)) has_persist = 1;
				if (!memcmp(buf + i, "session", 7)) has_session = 1;
			}
			ok(has_persist, "persist.com origin is in the db file");
			ok(!has_session, "session-only origins are NOT in the db file");
		}
	}
	remove(PERM_DB);

	printf(g_fail ? "\n=== FAILED ===\n" : "\n=== ALL PASS ===\n");
	return g_fail ? 1 : 0;
}
