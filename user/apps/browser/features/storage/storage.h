/* storage.h: Web Storage backing store. localStorage persists per origin; sessionStorage lives per tab. */
#ifndef OWOS_BROWSER_STORAGE_H
#define OWOS_BROWSER_STORAGE_H

#define STORAGE_QUOTA (5 * 1024 * 1024)

/* `session` selects sessionStorage (this tab's) over localStorage. Origin comes from `page_url`. */
const char *storage_get(const char *page_url, int session, const char *key); /* NULL if absent */
/* 0 on success, -1 if the origin would exceed STORAGE_QUOTA bytes (keys + values) or memory runs out. */
int storage_set(const char *page_url, int session, const char *key, const char *val);
void storage_remove(const char *page_url, int session, const char *key);
void storage_clear(const char *page_url, int session);
int storage_length(const char *page_url, int session);
const char *storage_key(const char *page_url, int session, int index); /* insertion order; NULL out of range */

void storage_load(void);                 /* restore localStorage at browser startup (best-effort) */
void storage_flush(void);                /* write localStorage if it changed */
void storage_drop_session(unsigned sid); /* a tab closed: forget its sessionStorage */

#endif /* OWOS_BROWSER_STORAGE_H */
