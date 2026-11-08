/* blob: object URL store: byte copies readable from the fetch worker threads. */

#include "browser.h"
#include "mutex.h"

struct blob_entry {
  struct blob_entry *next;
  char url[URL_MAX];
  char type[128];
  unsigned char *data;
  int len;
};

static struct blob_entry *g_entries;
static mutex_t g_lock = MUTEX_INIT;

static void entry_free(struct blob_entry *e)
{
  free(e->data);
  free(e);
}

bool blobstore_put(const char *url, const unsigned char *data, int len, const char *type)
{
  struct blob_entry *e = calloc(1, sizeof *e);
  if (!e)
    return false;
  e->data = malloc((unsigned)len + 1);
  if (!e->data) {
    free(e);
    return false;
  }
  if (len > 0)
    memcpy(e->data, data, (unsigned)len);
  e->len = len;
  strlcpy(e->url, url, sizeof e->url);
  strlcpy(e->type, type ? type : "", sizeof e->type);
  mutex_lock(&g_lock);
  e->next = g_entries;
  g_entries = e;
  mutex_unlock(&g_lock);
  return true;
}

void blobstore_del(const char *url)
{
  struct blob_entry **pp, *dead = 0;
  mutex_lock(&g_lock);
  for (pp = &g_entries; *pp; pp = &(*pp)->next) {
    if (streq((*pp)->url, url)) {
      dead = *pp;
      *pp = dead->next;
      break;
    }
  }
  mutex_unlock(&g_lock);
  if (dead)
    entry_free(dead);
}

void blobstore_clear(void)
{
  struct blob_entry *e;
  mutex_lock(&g_lock);
  e = g_entries;
  g_entries = 0;
  mutex_unlock(&g_lock);
  while (e) {
    struct blob_entry *next = e->next;
    entry_free(e);
    e = next;
  }
}

unsigned char *blobstore_get(const char *url, int *len, char *ctype, int cap)
{
  struct blob_entry *e;
  unsigned char *out = 0;
  mutex_lock(&g_lock);
  for (e = g_entries; e; e = e->next) {
    if (!streq(e->url, url))
      continue;
    out = malloc((unsigned)e->len + 1);
    if (out) {
      if (e->len > 0)
        memcpy(out, e->data, (unsigned)e->len);
      *len = e->len;
      if (ctype)
        strlcpy(ctype, e->type, cap);
    }
    break;
  }
  mutex_unlock(&g_lock);
  return out;
}
