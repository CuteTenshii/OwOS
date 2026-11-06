/* permissions.h: per-origin permission store and Permissions-Policy parsing. */
#ifndef OWOS_BROWSER_PERMISSIONS_H
#define OWOS_BROWSER_PERMISSIONS_H

/* Gated features. Values are persisted and used as policy bits, so only append. */
enum perm_feature { PERM_NOTIFICATIONS = 0, PERM_GEOLOCATION, PERM_CAMERA, PERM_MICROPHONE, PERM_FEATURE_COUNT };

/* A remembered decision. PERM_ASK = none yet (the caller should prompt). */
enum perm_state { PERM_ASK = 0, PERM_GRANTED, PERM_DENIED };

const char *perm_feature_label(int feature);    /* prompt UI label ("Location") */
const char *perm_feature_verb(int feature);     /* "know your location" for the msg */
const char *perm_feature_token(int feature);    /* Permissions API / policy name */
int perm_feature_from_token(const char *token); /* token -> feature, -1 if unknown */

/* Look up a remembered decision (session store first, then persistent). */
int perm_lookup(const char *origin, int feature);
/* Record a decision; persist != 0 writes the persistent store (Allow always). */
void perm_remember(const char *origin, int feature, int state, int persist);
/* Restore the persistent store at browser startup (best-effort). */
void perm_load(void);

/* --- Permissions-Policy (HTTP response header, formerly Feature-Policy) --- */
/* Parse a header value; returns a bitmap of features disabled (bit 1u<<feature). */
unsigned perm_policy_parse(const char *header_value, const char *host);
/* True if `feature` is blocked by the parsed `policy` bitmap. */
int perm_policy_blocks(unsigned policy, int feature);

#endif /* OWOS_BROWSER_PERMISSIONS_H */
