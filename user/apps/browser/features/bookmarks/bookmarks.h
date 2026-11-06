/* bookmarks.h — the browser's persistent bookmarks tree.
 *
 * Bookmarks and folders form one tree under an implicit root. A bookmark
 * carries a url, a display title and an optional 16x16 favicon (the page's,
 * snapshotted when it was starred); a folder carries a name, an
 * expanded/collapsed UI flag and its children (bookmarks and sub-folders,
 * nested arbitrarily deep). The tree is persisted to one real SQLite3 file (the
 * same engine + whole-file rewrite as the cookie jar) as a flat id/parent node
 * table.
 *
 * Mutations run on the UI thread; the deferred save runs on the worker thread
 * (bookmarks_flush), both under browser_buf_lock so the worker never walks a
 * half-linked tree. Every operation is best-effort — a failed alloc/read/write
 * leaves the in-memory tree intact, so the browser still runs without a
 * writable home dir. */
#ifndef BROWSER_BOOKMARKS_H
#define BROWSER_BOOKMARKS_H

struct bm_node {
  int is_folder;
  char *title;            /* bookmark title or folder name              */
  char *url;              /* bookmark: target URL; folder: NULL         */
  unsigned *icon;         /* bookmark: FAVICON_W*FAVICON_H ARGB, or NULL */
  int expanded;           /* folder: UI open/closed (persisted)         */
  struct bm_node *parent; /* owning folder (root for top level)         */
  struct bm_node *child;  /* folder: first child                        */
  struct bm_node *next;   /* next sibling                               */
};

/* Restore the tree from the SQLite db at startup (before the worker starts). */
void bookmarks_load(void);
/* Persist the tree to disk only if it changed since the last flush.
 * Worker-thread only (it briefly holds browser_buf_lock while copying the tree). */
void bookmarks_flush(void);

/* The implicit root folder; its `child` list is the top level. Never NULL. */
struct bm_node *bookmarks_root(void);

/* The bookmark node for `url` anywhere in the tree, or NULL. */
struct bm_node *bookmark_find(const char *url);

/* Add a bookmark under `folder` (NULL = root). Dedups: if `url` is already
 * bookmarked, the existing node is returned unchanged. `icon` (may be NULL) is
 * a FAVICON_W*FAVICON_H ARGB bitmap that is copied. Returns the node, or NULL.
 */
struct bm_node *bookmark_add(struct bm_node *folder, const char *url, const char *title, const unsigned *icon);

/* Replace `url`'s bookmark favicon with a copy of `icon` if it differs (e.g.
 * the page's favicon arrived after it was starred). No-op if `url` isn't
 * bookmarked. */
void bookmark_set_icon(const char *url, const unsigned *icon);

/* Create an empty folder named `name` under `folder` (NULL = root). Returns it.
 */
struct bm_node *folder_add(struct bm_node *folder, const char *name);

/* Remove the bookmark for `url` (used by the star toggle). No-op if absent. */
void bookmark_remove(const char *url);

/* Delete `node` and, for a folder, its whole subtree. Ignores the root. */
void bm_delete(struct bm_node *node);

/* Toggle a folder's expanded flag (persisted). No-op on a bookmark. */
void folder_toggle(struct bm_node *node);

#endif /* BROWSER_BOOKMARKS_H */
