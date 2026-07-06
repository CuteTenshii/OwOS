/* bookmarks.c — the browser's persistent bookmarks tree.
 *
 * The in-memory model is a tree of `struct bm_node` (folders + bookmarks) under a
 * static implicit root. A bookmark node may carry a heap copy of the page's 16x16
 * ARGB favicon. On disk the tree lives in one real SQLite3 database (user/lib/
 * sqlite), the file BM_DB, as a single flat node table with an id/parent linkage
 * — the same engine and whole-file rewrite model as the cookie jar (jar.c). It is
 * a genuine SQLite file: `sqlite3 bookmarks.db` on the host opens and queries it.
 *
 * Mutations (add/remove/delete/toggle) come from the UI thread; the deferred save
 * runs on the worker thread. Both take blk_lock (main.c's realloc/render
 * interlock) around the tree so the worker never walks a half-linked node, and a
 * delete frees its subtree under the same lock. The save copies the tree into the
 * SQLite model under the lock, then releases it before the (blocking) disk write,
 * so a save never stalls the UI for the length of a writefile.
 */
#include "browser.h"
#include "sqlite.h"
#include "bookmarks.h"

/* Mirrors the browser's /home/root convention (see jar.c's JAR_DB). */
#define BM_DB    "/home/root/bookmarks.db"
/* nodes(id, parent, kind, title, url, icon, expanded): kind 0=bookmark 1=folder;
 * parent 0 = the root. icon is a FAVICON_W*FAVICON_H ARGB BLOB (NULL if none). */
#define BM_SQL   "CREATE TABLE nodes(id INTEGER, parent INTEGER, kind INTEGER, " \
                 "title TEXT, url TEXT, icon BLOB, expanded INTEGER)"
#define BM_COLS  7
#define ICON_BYTES ((int)(FAVICON_W * FAVICON_H * sizeof(unsigned)))

/* The implicit root: a folder whose `child` list is the top level. */
static struct bm_node bm_root = { 1, 0, 0, 0, 1, 0, 0, 0 };
/* Set on any change; bookmarks_flush() rewrites the db once, later, on the worker
 * thread — keeping the whole-file write off the click that made the change. */
static bool bm_dirty;

static char *bm_dup(const char *s)
{
	if (!s) return 0;
	int n = (int)strlen(s);
	char *d = (char *)malloc((unsigned)n + 1);
	if (!d) return 0;
	memcpy(d, s, (unsigned)n + 1);
	return d;
}

/* Heap copy of a favicon bitmap (NULL -> NULL). */
static unsigned *icon_dup(const unsigned *ic)
{
	if (!ic) return 0;
	unsigned *d = (unsigned *)malloc((unsigned)ICON_BYTES);
	if (d) memcpy(d, ic, (unsigned)ICON_BYTES);
	return d;
}

struct bm_node *bookmarks_root(void) { return &bm_root; }

/* Append `n` to `folder`'s child list (order = creation order). */
static void link_child(struct bm_node *folder, struct bm_node *n)
{
	n->parent = folder;
	n->next = 0;
	if (!folder->child) { folder->child = n; return; }
	struct bm_node *t = folder->child;
	while (t->next) t = t->next;
	t->next = n;
}

/* Depth-first search for the bookmark whose url matches. */
static struct bm_node *find_url(struct bm_node *folder, const char *url)
{
	for (struct bm_node *n = folder->child; n; n = n->next) {
		if (!n->is_folder && n->url && streq(n->url, url)) return n;
		if (n->is_folder) {
			struct bm_node *hit = find_url(n, url);
			if (hit) return hit;
		}
	}
	return 0;
}

struct bm_node *bookmark_find(const char *url)
{
	if (!url || !url[0]) return 0;
	return find_url(&bm_root, url);
}

struct bm_node *bookmark_add(struct bm_node *folder, const char *url,
                             const char *title, const unsigned *icon)
{
	if (!url || !url[0]) return 0;
	struct bm_node *ex = bookmark_find(url);
	if (ex) return ex;
	if (!folder) folder = &bm_root;
	struct bm_node *n = (struct bm_node *)malloc(sizeof *n);
	if (!n) return 0;
	memset(n, 0, sizeof *n);
	n->url   = bm_dup(url);
	n->title = bm_dup(title && title[0] ? title : url);
	n->icon  = icon_dup(icon);
	if (!n->url || !n->title) { free(n->url); free(n->title); free(n->icon); free(n); return 0; }
	blk_lock();
	link_child(folder, n);
	blk_unlock();
	bm_dirty = true;
	return n;
}

void bookmark_set_icon(const char *url, const unsigned *icon)
{
	if (!icon) return;
	struct bm_node *n = bookmark_find(url);
	if (!n) return;
	if (n->icon && !memcmp(n->icon, icon, (unsigned)ICON_BYTES)) return;  /* unchanged */
	unsigned *cp = icon_dup(icon);
	if (!cp) return;
	blk_lock();
	free(n->icon);
	n->icon = cp;
	blk_unlock();
	bm_dirty = true;
}

struct bm_node *folder_add(struct bm_node *folder, const char *name)
{
	if (!folder) folder = &bm_root;
	struct bm_node *n = (struct bm_node *)malloc(sizeof *n);
	if (!n) return 0;
	memset(n, 0, sizeof *n);
	n->is_folder = 1;
	n->expanded = 1;
	n->title = bm_dup(name && name[0] ? name : "New Folder");
	if (!n->title) { free(n); return 0; }
	blk_lock();
	link_child(folder, n);
	blk_unlock();
	bm_dirty = true;
	return n;
}

/* Free a node and, for a folder, its whole subtree. Caller holds blk_lock. */
static void free_subtree(struct bm_node *n)
{
	struct bm_node *c = n->child;
	while (c) { struct bm_node *nx = c->next; free_subtree(c); c = nx; }
	free(n->url); free(n->title); free(n->icon); free(n);
}

/* Unlink `n` from its parent's child list. Caller holds blk_lock. */
static void unlink_node(struct bm_node *n)
{
	if (!n->parent) return;
	struct bm_node **pp = &n->parent->child;
	while (*pp) {
		if (*pp == n) { *pp = n->next; return; }
		pp = &(*pp)->next;
	}
}

void bm_delete(struct bm_node *node)
{
	if (!node || node == &bm_root) return;
	blk_lock();
	unlink_node(node);
	free_subtree(node);
	blk_unlock();
	bm_dirty = true;
}

void bookmark_remove(const char *url)
{
	struct bm_node *n = bookmark_find(url);
	if (n) bm_delete(n);
}

void folder_toggle(struct bm_node *node)
{
	if (!node || !node->is_folder) return;
	node->expanded = !node->expanded;
	bm_dirty = true;
}

/* ------------------------------------------------------- SQLite persistence */

/* Emit `n` and its subtree as rows; ids are DFS-assigned from *next. */
static void emit_node(struct sql_table *t, struct bm_node *n, int parent_id, int *next)
{
	int id = (*next)++;
	struct sql_value v[BM_COLS];
	v[0] = sqlv_int(id);
	v[1] = sqlv_int(parent_id);
	v[2] = sqlv_int(n->is_folder ? 1 : 0);
	v[3] = sqlv_text(n->title ? n->title : "");
	v[4] = (n->url) ? sqlv_text(n->url) : sqlv_null();
	if (n->icon) {
		v[5].type = SQL_BLOB; v[5].text = (char *)n->icon;
		v[5].len = ICON_BYTES; v[5].i = 0;
	} else {
		v[5] = sqlv_null();
	}
	v[6] = sqlv_int(n->is_folder ? (n->expanded ? 1 : 0) : 0);
	sql_insert(t, v, BM_COLS);   /* a long title/url spills onto SQLite overflow pages */
	for (struct bm_node *c = n->child; c; c = c->next)
		emit_node(t, c, id, next);
}

void bookmarks_flush(void)
{
	if (!bm_dirty) return;
	struct sql_db *db = sql_new();
	if (!db) return;                                   /* stays dirty: retry next tick */
	struct sql_table *t = sql_create_table(db, "nodes", BM_SQL, BM_COLS);
	if (!t) { sql_close(db); return; }                 /* stays dirty */

	blk_lock();
	int next = 1;
	for (struct bm_node *n = bm_root.child; n; n = n->next)
		emit_node(t, n, 0, &next);
	bm_dirty = false;
	blk_unlock();

	sql_save(db, BM_DB);                               /* writefile outside the lock */
	sql_close(db);
}

void bookmarks_load(void)
{
	struct sql_db *db = sql_open(BM_DB);
	if (!db) return;
	struct sql_table *t = sql_table_find(db, "nodes");
	if (!t) { sql_close(db); return; }

	/* Count rows to size the id->node map; ids were assigned 1..N in DFS order. */
	int maxid = 0;
	for (struct sql_row *r = t->rows; r; r = r->next)
		if (r->ncol >= 1 && r->col[0].type == SQL_INT && (int)r->col[0].i > maxid)
			maxid = (int)r->col[0].i;
	if (maxid <= 0) { sql_close(db); return; }
	struct bm_node **byid = (struct bm_node **)malloc((unsigned)(maxid + 1) * sizeof *byid);
	if (!byid) { sql_close(db); return; }
	for (int i = 0; i <= maxid; i++) byid[i] = 0;

	/* First pass: materialise each node keyed by its id. */
	for (struct sql_row *r = t->rows; r; r = r->next) {
		if (r->ncol < 7 || r->col[0].type != SQL_INT) continue;
		int id = (int)r->col[0].i;
		if (id < 1 || id > maxid) continue;
		struct bm_node *n = (struct bm_node *)malloc(sizeof *n);
		if (!n) continue;
		memset(n, 0, sizeof *n);
		n->is_folder = (r->col[2].type == SQL_INT && r->col[2].i != 0);
		n->title = bm_dup(r->col[3].type == SQL_TEXT ? r->col[3].text : "");
		if (!n->is_folder && r->col[4].type == SQL_TEXT) n->url = bm_dup(r->col[4].text);
		if (!n->is_folder && r->col[5].type == SQL_BLOB && r->col[5].len == ICON_BYTES) {
			n->icon = (unsigned *)malloc((unsigned)ICON_BYTES);
			if (n->icon) memcpy(n->icon, r->col[5].text, (unsigned)ICON_BYTES);
		}
		n->expanded = n->is_folder ? (r->col[6].type == SQL_INT ? r->col[6].i != 0 : 1) : 0;
		byid[id] = n;
	}
	/* Second pass: link each node to its parent (0 = root). Rows are in DFS order,
	 * so appending in id order restores the original sibling order. */
	for (struct sql_row *r = t->rows; r; r = r->next) {
		if (r->ncol < 2 || r->col[0].type != SQL_INT) continue;
		int id = (int)r->col[0].i;
		if (id < 1 || id > maxid || !byid[id]) continue;
		int pid = (r->col[1].type == SQL_INT) ? (int)r->col[1].i : 0;
		struct bm_node *parent = (pid >= 1 && pid <= maxid && byid[pid]) ? byid[pid] : &bm_root;
		link_child(parent, byid[id]);
	}
	free(byid);
	sql_close(db);
}
