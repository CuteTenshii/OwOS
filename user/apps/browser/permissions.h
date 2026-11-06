/* permissions.h — the browser's per-origin permission store +
 * Permissions-Policy.
 *
 * Two concerns, kept out of js.c (the request sites) and fetch.c (the header):
 *  - a decision store keyed by (origin, feature): a session list for "Allow
 * once" / "Refuse" plus a persistent SQLite table for "Allow always", so a
 * remembered grant survives reboots and a per-visit choice does not.
 *  - the Permissions-Policy response header, parsed to a per-feature "blocked"
 *    bitmap: a feature the site's own policy disallows is denied WITHOUT a
 * prompt.
 *
 * A "feature" is one powerful capability (notifications, geolocation, camera,
 * microphone). An "origin" is the "scheme://host" the request came from.
 */
#ifndef OWOS_BROWSER_PERMISSIONS_H
#define OWOS_BROWSER_PERMISSIONS_H

/* Gated features. The value is a stable index (persisted on disk + a policy
 * bit), so only append. */
enum perm_feature { PERM_NOTIFICATIONS = 0, PERM_GEOLOCATION, PERM_CAMERA, PERM_MICROPHONE, PERM_FEATURE_COUNT };

/* A remembered decision. PERM_ASK = none yet (the caller should prompt). */
enum perm_state { PERM_ASK = 0, PERM_GRANTED, PERM_DENIED };

const char *perm_feature_label(int feature);    /* prompt UI label ("Location") */
const char *perm_feature_verb(int feature);     /* "know your location" for the msg */
const char *perm_feature_token(int feature);    /* Permissions API / policy name */
int perm_feature_from_token(const char *token); /* token -> feature, -1 if unknown */

/* Look up a remembered decision (session store first, then persistent). */
int perm_lookup(const char *origin, int feature);
/* Record a decision. persist != 0 writes the persistent store (Allow always);
 * otherwise it is session-only (Allow once / Refuse). */
void perm_remember(const char *origin, int feature, int state, int persist);
/* Restore the persistent store at browser startup (best-effort). */
void perm_load(void);

/* --- Permissions-Policy (HTTP response header, formerly Feature-Policy) --- */
/* Parse a header value against the page's own host, returning a bitmap of
 * features DISABLED for this document (bit (1u<<feature) set = blocked). An
 * unlisted feature, or an allowlist containing `*`/`self`/the page's own
 * origin, stays enabled. Combine multiple headers by OR-ing their results. */
unsigned perm_policy_parse(const char *header_value, const char *host);
/* True if `feature` is blocked by the parsed `policy` bitmap. */
int perm_policy_blocks(unsigned policy, int feature);

#endif /* OWOS_BROWSER_PERMISSIONS_H */
