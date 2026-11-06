/* permissions.c — per-origin permission store + Permissions-Policy parser.
 * See permissions.h. The store is one in-memory list of (origin, feature ->
 * state) entries; those flagged `persist` are also written to a real SQLite db
 * (user/lib/sqlite) at PERM_DB, so "Allow always" survives a reboot while
 * "Allow once" / "Refuse" (session-only) do not. Whole-file rewrite on each
 * persistent change — the list is tiny, like the cookie jar (jar.c). */
#include "permissions.h"
#include "owos.h" /* malloc/free/strlen + streq_ci/starts_ci helpers */
#include "sqlite.h"

#ifndef PERM_DB /* overridable so the host test can redirect it */
#define PERM_DB "/home/root/permissions.db"
#endif
#define PERM_SQL "CREATE TABLE perm(origin TEXT, feature INTEGER, state INTEGER)"
#define PERM_COLS 3

struct perm_entry {
  char *origin;
  int feature;
  int state;   /* PERM_GRANTED / PERM_DENIED                */
  int persist; /* 1 = also lives on disk (Allow always)     */
  struct perm_entry *next;
};
static struct perm_entry *g_perms;

/* Per-feature metadata: the on-disk index is the array index, so only append.
 */
static const struct {
  const char *label, *verb, *token;
} k_feat[PERM_FEATURE_COUNT] = {
    {"Notifications", "show notifications", "notifications"},
    {"Location", "know your location", "geolocation"},
    {"Camera", "use your camera", "camera"},
    {"Microphone", "use your microphone", "microphone"},
};

const char *perm_feature_label(int f) { return (f >= 0 && f < PERM_FEATURE_COUNT) ? k_feat[f].label : "this feature"; }
const char *perm_feature_verb(int f)
{
  return (f >= 0 && f < PERM_FEATURE_COUNT) ? k_feat[f].verb : "use this feature";
}
const char *perm_feature_token(int f) { return (f >= 0 && f < PERM_FEATURE_COUNT) ? k_feat[f].token : ""; }

int perm_feature_from_token(const char *token)
{
  if (!token)
    return -1;
  for (int f = 0; f < PERM_FEATURE_COUNT; f++)
    if (streq_ci(token, k_feat[f].token))
      return f;
  return -1;
}

static char *pdup(const char *s)
{
  int n = s ? strlen(s) : 0;
  char *d = (char *)malloc((unsigned)n + 1);
  if (!d)
    return 0;
  for (int i = 0; i < n; i++)
    d[i] = s[i];
  d[n] = '\0';
  return d;
}

static struct perm_entry *perm_find(const char *origin, int feature)
{
  if (!origin)
    return 0;
  for (struct perm_entry *e = g_perms; e; e = e->next)
    if (e->feature == feature && e->origin && streq_ci(e->origin, origin))
      return e;
  return 0;
}

int perm_lookup(const char *origin, int feature)
{
  struct perm_entry *e = perm_find(origin, feature);
  return e ? e->state : PERM_ASK;
}

/* Rewrite the persistent db from every persist-flagged entry (best-effort). */
static void perm_save(void)
{
  struct sql_db *db = sql_new();
  if (!db)
    return;
  struct sql_table *t = sql_create_table(db, "perm", PERM_SQL, PERM_COLS);
  if (t) {
    for (struct perm_entry *e = g_perms; e; e = e->next) {
      if (!e->persist || !e->origin)
        continue;
      struct sql_value v[PERM_COLS];
      v[0] = sqlv_text(e->origin);
      v[1] = sqlv_int(e->feature);
      v[2] = sqlv_int(e->state);
      sql_insert(t, v, PERM_COLS);
    }
  }
  sql_save(db, PERM_DB);
  sql_close(db);
}

void perm_remember(const char *origin, int feature, int state, int persist)
{
  if (!origin || !origin[0] || feature < 0 || feature >= PERM_FEATURE_COUNT)
    return;
  struct perm_entry *e = perm_find(origin, feature);
  int was_persist = e ? e->persist : 0;
  if (!e) {
    e = (struct perm_entry *)malloc(sizeof *e);
    if (!e)
      return;
    e->origin = pdup(origin);
    if (!e->origin) {
      free(e);
      return;
    }
    e->feature = feature;
    e->next = g_perms;
    g_perms = e;
  }
  e->state = state;
  e->persist = persist ? 1 : 0;
  if (persist || was_persist)
    perm_save(); /* disk changed either way */
}

void perm_load(void)
{
  struct sql_db *db = sql_open(PERM_DB);
  if (!db)
    return;
  struct sql_table *t = sql_table_find(db, "perm");
  if (t) {
    for (struct sql_row *r = t->rows; r; r = r->next) {
      if (r->ncol < 3)
        continue;
      if (r->col[0].type != SQL_TEXT || r->col[1].type != SQL_INT || r->col[2].type != SQL_INT)
        continue;
      int feature = (int)r->col[1].i;
      int state = (int)r->col[2].i;
      if (feature < 0 || feature >= PERM_FEATURE_COUNT)
        continue;
      if (state != PERM_GRANTED && state != PERM_DENIED)
        continue;
      if (perm_find(r->col[0].text, feature))
        continue; /* de-dupe */
      struct perm_entry *e = (struct perm_entry *)malloc(sizeof *e);
      if (!e)
        break;
      e->origin = pdup(r->col[0].text);
      if (!e->origin) {
        free(e);
        continue;
      }
      e->feature = feature;
      e->state = state;
      e->persist = 1;
      e->next = g_perms;
      g_perms = e;
    }
  }
  sql_close(db);
}

/* ---- Permissions-Policy (HTTP response header) -------------------------- */

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Does an allowlist enable this document (whose host is `host`)? The forms are
 * `*` (all), a bare `self`, or a parenthesised list of `self`/`*`/quoted
 * origins.
 * `()` (empty) disables. We enable when the list carries `*`, `self`, or the
 * page's own host — the top-level document's origin. */
static int allowlist_enables(const char *a, int alen, const char *host)
{
  int has = 0;
  /* scan tokens: '*', bare/quoted words; ignore parens/quotes/commas/spaces */
  for (int i = 0; i < alen; i++) {
    char c = a[i];
    if (c == '*')
      return 1;
    if (is_ws(c) || c == '(' || c == ')' || c == '"' || c == '\'' || c == ',')
      continue;
    /* word start: capture [a-zA-Z] run and URL-ish runs */
    int s = i;
    while (i < alen && a[i] != ' ' && a[i] != '"' && a[i] != '\'' && a[i] != '(' && a[i] != ')' && a[i] != ',')
      i++;
    int wl = i - s;
    if (wl == 4 && starts_ci(a + s, "self"))
      return 1;
    if (host && host[0] && wl > 0) {
      /* enabled if the page's own host appears in a listed origin */
      int hl = strlen(host);
      for (int k = s; k + hl <= s + wl; k++)
        if (starts_ci(a + k, host)) {
          has = 1;
          break;
        }
      if (has)
        return 1;
    }
  }
  return 0;
}

unsigned perm_policy_parse(const char *hv, const char *host)
{
  unsigned blocked = 0;
  if (!hv)
    return 0;
  int n = strlen(hv), i = 0;
  while (i < n) {
    /* one directive up to ',' (Permissions-Policy) or ';' (Feature-Policy) */
    int s = i;
    while (i < n && hv[i] != ',' && hv[i] != ';')
      i++;
    int e = i;
    if (i < n)
      i++; /* skip the separator */
    while (s < e && is_ws(hv[s]))
      s++;
    while (e > s && is_ws(hv[e - 1]))
      e--;
    if (s >= e)
      continue;
    /* split token[=allowlist]; a bare token (Feature-Policy "geolocation
     * 'self'") also works — the allowlist is whatever follows the token. */
    int eq = s;
    while (eq < e && hv[eq] != '=' && !is_ws(hv[eq]))
      eq++;
    int f = -1;
    {
      char tok[32];
      int k = 0;
      for (int j = s; j < eq && k < (int)sizeof tok - 1; j++)
        tok[k++] = hv[j];
      tok[k] = 0;
      f = perm_feature_from_token(tok);
    }
    if (f < 0)
      continue;
    int as = eq;
    if (as < e && hv[as] == '=')
      as++;
    while (as < e && is_ws(hv[as]))
      as++;
    if (!allowlist_enables(hv + as, e - as, host))
      blocked |= (1u << f);
  }
  return blocked;
}

int perm_policy_blocks(unsigned policy, int feature)
{
  if (feature < 0 || feature >= PERM_FEATURE_COUNT)
    return 0;
  return (policy & (1u << feature)) != 0;
}
