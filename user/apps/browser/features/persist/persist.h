/* persist.h: origin-keyed byte files that keep IndexedDB databases and Cache API caches across page loads and
 * browser restarts. */
#ifndef OWOS_BROWSER_PERSIST_H
#define OWOS_BROWSER_PERSIST_H

/* The file for `kind` ("idb", "cache") of the origin of `page_url`; -1 for an opaque origin, which never persists */
int persist_path(const char *kind, const char *page_url, char *path, int cap);

/* The file's contents (malloc'd, *len bytes), or NULL when it is missing or damaged */
unsigned char *persist_read(const char *path, int *len);

/* Replaces the file; 0 on success */
int persist_write(const char *path, const void *buf, int len);

#endif /* OWOS_BROWSER_PERSIST_H */
