/* jar.c — the browser's persistent cookie + HSTS store.
 *
 * Keeps the cookie/HSTS data model AND its on-disk persistence out of the HTTP
 * transport (fetch.c), which only ever calls cookie_store/cookie_header/
 * hsts_add/hsts_has. Both in-memory lists are uncapped heap linked lists
 * (B->cookies, B->hsts) — there is no fixed jar size and no fixed field width.
 *
 * On disk both live in ONE real SQLite3 database (user/lib/sqlite), the file
 * JAR_DB, as two tables: `cookies(host, name, value)` and `hsts(host)`. It is a
 * genuine SQLite file — `sqlite3 cookies.db` on the host opens and queries it.
 * The OwOS file API is whole-file, so every change rebuilds and rewrites the
 * whole db; the jar is small, so that is cheap. All persistence is best-effort:
 * a read/write/allocation failure just leaves the in-memory list as-is, so the
 * browser still runs without a writable home directory.
 */
#include "browser.h"
#include "sqlite.h"

/* Mirrors the browser's existing /home/root convention (see DL_DIR for
 * downloads). The file name stays cookies.db even though it now also holds the
 * HSTS table — it is the browser's jar. */
#define JAR_DB     "/home/root/cookies.db"
#define COOKIE_SQL "CREATE TABLE cookies(host TEXT, name TEXT, value TEXT)"
#define HSTS_SQL   "CREATE TABLE hsts(host TEXT)"

/* Heap copy of a byte range as a NUL-terminated string (no length cap). */
static char *dupn(const char *s, int n)
{
	char *d = (char *)malloc((unsigned)(n + 1));
	if (!d) return 0;
	for (int i = 0; i < n; i++) d[i] = s[i];
	d[n] = '\0';
	return d;
}
static char *dupz(const char *s) { return dupn(s, strlen(s)); }

/* Set when the in-memory jar changes; jar_flush() writes the db once, later.
 * This keeps the expensive whole-file rewrite OUT of the per-header parse loop —
 * a response with several Set-Cookie lines now costs one write, not one each. */
static bool jar_dirty;

/* --------------------------------------------------------------- cookie jar */

void cookie_store(const char *host, const char *v)
{
	while (*v == ' ') v++;
	const char *ns = v;
	while (*v && *v != '=' && *v != ';') v++;
	int nlen = (int)(v - ns);
	if (*v != '=' || nlen == 0) return;
	char *name = dupn(ns, nlen);
	if (!name) return;
	v++;
	const char *vs = v;
	while (*v && *v != ';' && *v != '\r' && *v != '\n') v++;
	char *val = dupn(vs, (int)(v - vs));
	if (!val) { free(name); return; }

	/* Update in place if this (host, name) is already stored. */
	for (struct cookie *c = B->cookies; c; c = c->next) {
		if (streq_ci(c->host, host) && streq(c->name, name)) {
			free(c->val);
			c->val = val;
			free(name);
			jar_dirty = true;
			return;
		}
	}
	/* Otherwise prepend a fresh node. */
	struct cookie *c = (struct cookie *)malloc(sizeof *c);
	if (!c) { free(name); free(val); return; }
	c->host = dupz(host);
	c->name = name;
	c->val  = val;
	if (!c->host) { free(c->name); free(c->val); free(c); return; }
	c->next = B->cookies;
	B->cookies = c;
	jar_dirty = true;
}

int cookie_header(const char *host, char *out, int cap)
{
	int n = 0;
	for (struct cookie *c = B->cookies; c; c = c->next) {
		if (!streq_ci(c->host, host)) continue;
		if (n) sapp(out, &n, cap, "; ");
		sapp(out, &n, cap, c->name);
		sapp(out, &n, cap, "=");
		sapp(out, &n, cap, c->val);
	}
	return n;
}

/* Free the in-memory cookie jar (used before reloading it from disk). */
static void cookie_clear(void)
{
	struct cookie *c = B->cookies;
	while (c) {
		struct cookie *n = c->next;
		free(c->host); free(c->name); free(c->val); free(c);
		c = n;
	}
	B->cookies = 0;
}

/* ----------------------------------------------- HSTS (force-HTTPS hosts) */

int hsts_has(const char *host)
{
	for (struct hsts_host *h = B->hsts; h; h = h->next)
		if (streq_ci(h->host, host)) return 1;
	return 0;
}

void hsts_add(const char *host)
{
	if (hsts_has(host)) return;
	struct hsts_host *h = (struct hsts_host *)malloc(sizeof *h);
	if (!h) return;
	h->host = dupz(host);
	if (!h->host) { free(h); return; }
	h->next = B->hsts;
	B->hsts = h;
	jar_dirty = true;
}

/* Free the in-memory HSTS list. */
static void hsts_clear(void)
{
	struct hsts_host *h = B->hsts;
	while (h) {
		struct hsts_host *n = h->next;
		free(h->host); free(h);
		h = n;
	}
	B->hsts = 0;
}

/* ----------------------------------------------------- SQLite persistence */

/* Load the persistent jar (cookies + HSTS) from the SQLite db at startup. A
 * missing or corrupt db simply leaves both lists empty. No caps on either. */
void jar_load(void)
{
	struct sql_db *db = sql_open(JAR_DB);
	if (!db) return;
	cookie_clear();
	hsts_clear();

	struct sql_table *t = sql_table_find(db, "cookies");
	if (t) {
		for (struct sql_row *r = t->rows; r; r = r->next) {
			if (r->ncol < 3) continue;
			if (r->col[0].type != SQL_TEXT || r->col[1].type != SQL_TEXT ||
			    r->col[2].type != SQL_TEXT)
				continue;
			struct cookie *c = (struct cookie *)malloc(sizeof *c);
			if (!c) break;
			c->host = dupz(r->col[0].text);
			c->name = dupz(r->col[1].text);
			c->val  = dupz(r->col[2].text);
			if (!c->host || !c->name || !c->val) {
				free(c->host); free(c->name); free(c->val); free(c);
				continue;
			}
			c->next = B->cookies;
			B->cookies = c;
		}
	}

	struct sql_table *ht = sql_table_find(db, "hsts");
	if (ht) {
		for (struct sql_row *r = ht->rows; r; r = r->next) {
			if (r->ncol < 1 || r->col[0].type != SQL_TEXT) continue;
			struct hsts_host *h = (struct hsts_host *)malloc(sizeof *h);
			if (!h) break;
			h->host = dupz(r->col[0].text);
			if (!h->host) { free(h); continue; }
			h->next = B->hsts;
			B->hsts = h;
		}
	}
	sql_close(db);
}

/* Rewrite the jar db (cookies + HSTS tables) from the in-memory lists. */
void jar_save(void)
{
	struct sql_db *db = sql_new();
	if (!db) return;
	struct sql_table *t = sql_create_table(db, "cookies", COOKIE_SQL, 3);
	if (t) {
		for (struct cookie *c = B->cookies; c; c = c->next) {
			struct sql_value v[3];
			v[0] = sqlv_text(c->host);
			v[1] = sqlv_text(c->name);
			v[2] = sqlv_text(c->val);
			sql_insert(t, v, 3);
		}
	}
	struct sql_table *ht = sql_create_table(db, "hsts", HSTS_SQL, 1);
	if (ht) {
		for (struct hsts_host *h = B->hsts; h; h = h->next) {
			struct sql_value v[1];
			v[0] = sqlv_text(h->host);
			sql_insert(ht, v, 1);
		}
	}
	sql_save(db, JAR_DB);
	sql_close(db);
}

/* Persist the jar to disk only if it changed since the last flush. Call once
 * after handling a response, not once per cookie. */
void jar_flush(void)
{
	if (!jar_dirty) return;
	jar_dirty = false;
	jar_save();
}
