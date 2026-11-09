/* persist.c: see persist.h. A file is "OWPS1\n", its length, the bytes and a checksum, so a write cut short is
 * recognised and dropped. owofs takes one writer at a time, hence the lock. */
#include "features/persist/persist.h"
#include "browser.h"
#include "features/cors/cors.h"
#include "mutex.h"

#ifndef PERSIST_PREFIX /* overridable so the host tests can redirect it */
#define PERSIST_PREFIX "/home/root/.browser-"
#endif
#define PERSIST_MAGIC "OWPS1\n"
#define PERSIST_MAX (64u << 20)

static mutex_t g_lock = MUTEX_INIT;

static unsigned fnv(const unsigned char *p, unsigned n, unsigned h)
{
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

static void put32(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static unsigned get32(const unsigned char *p)
{
  return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

int persist_path(const char *kind, const char *page_url, char *path, int cap)
{
  char origin[URL_MAX];
  unsigned h1 = 2166136261u, h2 = 0x9747b28cu;
  if (!starts_ci(page_url, "http://") && !starts_ci(page_url, "https://"))
    return -1;
  cors_origin(page_url, origin, sizeof origin);
  if (!origin[0] || streq(origin, "null"))
    return -1;
  h1 = fnv((const unsigned char *)origin, (unsigned)strlen(origin), h1);
  h2 = fnv((const unsigned char *)origin, (unsigned)strlen(origin), h2 ^ 0x5bd1e995u);
  snprintf(path, (unsigned)cap, "%s%s-%08x%08x.bin", PERSIST_PREFIX, kind, h1, h2);
  return 0;
}

unsigned char *persist_read(const char *path, int *len)
{
  struct statinfo si;
  unsigned char *raw, *out;
  unsigned n, head = sizeof PERSIST_MAGIC - 1 + 4;
  mutex_lock(&g_lock);
  if (stat(path, &si) != 0 || si.size < head + 4 || si.size > PERSIST_MAX + head + 4) {
    mutex_unlock(&g_lock);
    return 0;
  }
  raw = (unsigned char *)malloc(si.size);
  if (!raw || readfile_at(path, 0, raw, si.size) != (int)si.size) {
    free(raw);
    mutex_unlock(&g_lock);
    return 0;
  }
  mutex_unlock(&g_lock);
  n = get32(raw + sizeof PERSIST_MAGIC - 1);
  if (memcmp(raw, PERSIST_MAGIC, sizeof PERSIST_MAGIC - 1) != 0 || n != si.size - head - 4 ||
      fnv(raw + head, n, 2166136261u) != get32(raw + head + n)) {
    free(raw);
    return 0;
  }
  out = (unsigned char *)malloc(n ? n : 1);
  if (out) {
    memcpy(out, raw + head, n);
    *len = (int)n;
  }
  free(raw);
  return out;
}

int persist_write(const char *path, const void *buf, int len)
{
  unsigned head = sizeof PERSIST_MAGIC - 1 + 4, n = (unsigned)len;
  unsigned char *raw;
  int rc;
  if (len < 0 || n > PERSIST_MAX)
    return -1;
  raw = (unsigned char *)malloc(head + n + 4);
  if (!raw)
    return -1;
  memcpy(raw, PERSIST_MAGIC, sizeof PERSIST_MAGIC - 1);
  put32(raw + sizeof PERSIST_MAGIC - 1, n);
  memcpy(raw + head, buf, n);
  put32(raw + head + n, fnv((const unsigned char *)buf, n, 2166136261u));
  mutex_lock(&g_lock);
  rc = writefile(path, raw, (int)(head + n + 4));
  mutex_unlock(&g_lock);
  free(raw);
  return rc;
}
