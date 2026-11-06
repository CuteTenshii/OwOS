/* bookmarks.h: persistent bookmarks tree (SQLite file). UI-thread mutations, worker-thread flush, under
 * browser_buf_lock. */
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

/* Restore the tree from the SQLite db at startup. */
void bookmarks_load(void);
/* Persist the tree if it changed. Worker thread only. */
void bookmarks_flush(void);

/* The implicit root folder; never NULL. */
struct bm_node *bookmarks_root(void);

/* The bookmark node for `url`, or NULL. */
struct bm_node *bookmark_find(const char *url);

/* Add a bookmark under `folder` (NULL = root); returns the existing node if `url` is present. */
struct bm_node *bookmark_add(struct bm_node *folder, const char *url, const char *title, const unsigned *icon);

/* Replace the favicon of `url`'s bookmark with a copy of `icon` if it differs. */
void bookmark_set_icon(const char *url, const unsigned *icon);

/* Create an empty folder under `folder` (NULL = root). */
struct bm_node *folder_add(struct bm_node *folder, const char *name);

/* Remove the bookmark for `url`; no-op if absent. */
void bookmark_remove(const char *url);

/* Delete `node` and its subtree; ignores the root. */
void bm_delete(struct bm_node *node);

/* Toggle a folder's expanded flag (persisted). */
void folder_toggle(struct bm_node *node);

#endif /* BROWSER_BOOKMARKS_H */
