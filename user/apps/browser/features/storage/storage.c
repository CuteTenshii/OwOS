/* storage.c: localStorage (SQLite file STORAGE_DB, whole-file rewrite) and per-tab sessionStorage. */
#include "features/storage/storage.h"
#include "browser.h"
#include "sqlite.h"
#include "url.h"

#ifndef STORAGE_DB /* overridable so the host test can redirect it */
#define STORAGE_DB "/home/root/localstorage.db"
#endif
#define STORAGE_SQL "CREATE TABLE storage(origin TEXT, key TEXT, value TEXT)"
#define STORAGE_COLS 3
#define FLUSH_MS 2000 /* minimum gap between writes triggered by setItem */

struct item {
  char *origin, *key, *val;
  unsigned sid; /* 0 = localStorage (persisted), else the owning tab's session id */
  struct item *next;
};
static struct item *g_items;
static bool g_dirty;
static unsigned g_last_flush, g_next_sid;

static char *dupz(const char *s)
{
  int n = strlen(s);
  char *d = (char *)malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}

/* Scheme://host:port; an opaque page (no host) gets "null" and is never persisted. */
static void origin_of(const char *url, char *out, int cap)
{
  struct purl u;
  if (!url || !parse_url(url, &u) || !u.host[0]) {
    strlcpy(out, "null", cap);
    return;
  }
  snprintf(out, (unsigned)cap, "%s://%s:%d", u.tls ? "https" : "http", u.host, u.port);
}

/* The scope id of an entry: 0 for a persisted localStorage origin, else the tab's session id. */
static unsigned scope_id(int session, const char *origin)
{
  if (!session && strcmp(origin, "null") != 0)
    return 0;
  if (!B->pg->sid)
    B->pg->sid = ++g_next_sid;
  return B->pg->sid;
}

static struct item **item_find(unsigned sid, const char *origin, const char *key)
{
  struct item **pp = &g_items;
  for (; *pp; pp = &(*pp)->next)
    if ((*pp)->sid == sid && !strcmp((*pp)->origin, origin) && !strcmp((*pp)->key, key))
      return pp;
  return 0;
}

static void item_free(struct item *it)
{
  free(it->origin);
  free(it->key);
  free(it->val);
  free(it);
}

static void append(struct item *it)
{
  struct item **pp = &g_items;
  while (*pp)
    pp = &(*pp)->next;
  *pp = it;
}

static void save(void)
{
  struct sql_db *db = sql_new();
  if (!db)
    return;
  struct sql_table *t = sql_create_table(db, "storage", STORAGE_SQL, STORAGE_COLS);
  if (t) {
    for (struct item *it = g_items; it; it = it->next) {
      if (it->sid)
        continue;
      struct sql_value v[STORAGE_COLS];
      v[0] = sqlv_text(it->origin);
      v[1] = sqlv_text(it->key);
      v[2] = sqlv_text(it->val);
      sql_insert(t, v, STORAGE_COLS);
    }
  }
  sql_save(db, STORAGE_DB);
  sql_close(db);
  g_dirty = false;
  g_last_flush = msec();
}

static void touch(unsigned sid)
{
  if (sid)
    return;
  g_dirty = true;
  if (msec() - g_last_flush >= FLUSH_MS)
    save();
}

const char *storage_get(const char *page_url, int session, const char *key)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  struct item **pp = item_find(scope_id(session, origin), origin, key);
  return pp ? (*pp)->val : 0;
}

static unsigned long used_bytes(unsigned sid, const char *origin)
{
  unsigned long n = 0;
  for (struct item *it = g_items; it; it = it->next)
    if (it->sid == sid && !strcmp(it->origin, origin))
      n += strlen(it->key) + strlen(it->val);
  return n;
}

int storage_set(const char *page_url, int session, const char *key, const char *val)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  unsigned sid = scope_id(session, origin);
  struct item **pp = item_find(sid, origin, key);
  unsigned long used = used_bytes(sid, origin) + strlen(val);
  used = pp ? used - strlen((*pp)->val) : used + strlen(key);
  if (used > STORAGE_QUOTA)
    return -1;
  if (pp) {
    char *v = dupz(val);
    if (!v)
      return -1;
    free((*pp)->val);
    (*pp)->val = v;
  } else {
    struct item *it = (struct item *)malloc(sizeof *it);
    if (!it)
      return -1;
    it->origin = dupz(origin);
    it->key = dupz(key);
    it->val = dupz(val);
    it->sid = sid;
    it->next = 0;
    if (!it->origin || !it->key || !it->val) {
      item_free(it);
      return -1;
    }
    append(it);
  }
  touch(sid);
  return 0;
}

void storage_remove(const char *page_url, int session, const char *key)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  unsigned sid = scope_id(session, origin);
  struct item **pp = item_find(sid, origin, key);
  if (!pp)
    return;
  struct item *it = *pp;
  *pp = it->next;
  item_free(it);
  touch(sid);
}

void storage_clear(const char *page_url, int session)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  unsigned sid = scope_id(session, origin);
  bool any = false;
  for (struct item **pp = &g_items; *pp;) {
    struct item *it = *pp;
    if (it->sid == sid && !strcmp(it->origin, origin)) {
      *pp = it->next;
      item_free(it);
      any = true;
    } else {
      pp = &it->next;
    }
  }
  if (any)
    touch(sid);
}

int storage_length(const char *page_url, int session)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  unsigned sid = scope_id(session, origin);
  int n = 0;
  for (struct item *it = g_items; it; it = it->next)
    n += it->sid == sid && !strcmp(it->origin, origin);
  return n;
}

const char *storage_key(const char *page_url, int session, int index)
{
  char origin[URL_HOST_MAX + 24];
  origin_of(page_url, origin, sizeof origin);
  unsigned sid = scope_id(session, origin);
  for (struct item *it = g_items; it; it = it->next)
    if (it->sid == sid && !strcmp(it->origin, origin) && index-- == 0)
      return it->key;
  return 0;
}

void storage_drop_session(unsigned sid)
{
  if (!sid)
    return;
  for (struct item **pp = &g_items; *pp;) {
    struct item *it = *pp;
    if (it->sid == sid) {
      *pp = it->next;
      item_free(it);
    } else {
      pp = &it->next;
    }
  }
}

void storage_load(void)
{
  struct sql_db *db = sql_open(STORAGE_DB);
  if (!db)
    return;
  struct sql_table *t = sql_table_find(db, "storage");
  if (t) {
    for (struct sql_row *r = t->rows; r; r = r->next) {
      if (r->ncol < STORAGE_COLS || r->col[0].type != SQL_TEXT || r->col[1].type != SQL_TEXT ||
          r->col[2].type != SQL_TEXT)
        continue;
      if (item_find(0, r->col[0].text, r->col[1].text))
        continue;
      struct item *it = (struct item *)malloc(sizeof *it);
      if (!it)
        break;
      it->origin = dupz(r->col[0].text);
      it->key = dupz(r->col[1].text);
      it->val = dupz(r->col[2].text);
      it->sid = 0;
      it->next = 0;
      if (!it->origin || !it->key || !it->val) {
        item_free(it);
        continue;
      }
      append(it);
    }
  }
  sql_close(db);
}

void storage_flush(void)
{
  if (g_dirty)
    save();
}
