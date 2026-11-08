/* env.c - process environment strings (see env.h). */

#include "env.h"
#include "heap.h"
#include "kstring.h"

/* Length of the name in an entry, or of a bare name */
static uint32_t name_len(const char *s)
{
  uint32_t n = 0;
  while (s[n] && s[n] != '=')
    n++;
  return n;
}

static const char *find(const struct env *e, const char *name, uint32_t nlen, uint32_t *offset)
{
  for (uint32_t off = 0; off < e->len;) {
    const char *entry = e->data + off;
    uint32_t elen = (uint32_t)strlen(entry);
    if (name_len(entry) == nlen && memcmp(entry, name, nlen) == 0) {
      if (offset)
        *offset = off;
      return entry;
    }
    off += elen + 1;
  }
  return 0;
}

const char *env_get(const struct env *e, const char *name)
{
  const char *entry = find(e, name, (uint32_t)strlen(name), 0);
  return entry ? entry + name_len(entry) + 1 : 0;
}

/* Replace the bytes [off, off + drop) with `insert` (insert_len bytes); false when out of memory */
static bool splice(struct env *e, uint32_t off, uint32_t drop, const char *insert, uint32_t insert_len)
{
  uint32_t len = e->len - drop + insert_len;
  char *data = len ? kmalloc(len) : 0;
  if (len && !data)
    return false;
  if (data) {
    memcpy(data, e->data, off);
    memcpy(data + off, insert, insert_len);
    memcpy(data + off + insert_len, e->data + off + drop, e->len - off - drop);
  }
  kfree(e->data);
  e->data = data;
  e->len = len;
  return true;
}

int env_set(struct env *e, const char *name, const char *value, bool overwrite)
{
  uint32_t nlen = (uint32_t)strlen(name), vlen = (uint32_t)strlen(value);
  if (!nlen || name_len(name) != nlen)
    return -1;
  uint32_t off = 0, drop = 0;
  const char *old = find(e, name, nlen, &off);
  if (old) {
    if (!overwrite)
      return 0;
    drop = (uint32_t)strlen(old) + 1;
  } else {
    off = e->len;
  }
  char *entry = kmalloc(nlen + vlen + 2);
  if (!entry)
    return -1;
  memcpy(entry, name, nlen);
  entry[nlen] = '=';
  memcpy(entry + nlen + 1, value, vlen + 1);
  bool ok = splice(e, off, drop, entry, nlen + vlen + 2);
  kfree(entry);
  return ok ? 0 : -1;
}

void env_unset(struct env *e, const char *name)
{
  uint32_t off;
  const char *entry = find(e, name, (uint32_t)strlen(name), &off);
  if (entry)
    splice(e, off, (uint32_t)strlen(entry) + 1, 0, 0);
}

const char *env_entry(const struct env *e, uint32_t index)
{
  for (uint32_t off = 0; off < e->len; index--) {
    const char *entry = e->data + off;
    if (index == 0)
      return entry;
    off += (uint32_t)strlen(entry) + 1;
  }
  return 0;
}

int env_copy(struct env *dst, const struct env *src)
{
  if (!src->len)
    return 0;
  dst->data = kmalloc(src->len);
  if (!dst->data)
    return -1;
  memcpy(dst->data, src->data, src->len);
  dst->len = src->len;
  return 0;
}

int env_from_block(struct env *e, const char *block, uint32_t len)
{
  /* Keep well-formed entries only: non-empty NAME, a '=', and a NUL inside the block */
  char *data = len ? kmalloc(len) : 0;
  uint32_t out = 0;
  if (len && !data)
    return -1;
  for (uint32_t off = 0; off < len;) {
    uint32_t elen = 0;
    while (off + elen < len && block[off + elen])
      elen++;
    if (off + elen < len) {
      uint32_t nlen = name_len(block + off);
      if (nlen && nlen < elen) {
        memcpy(data + out, block + off, elen + 1);
        out += elen + 1;
      }
    }
    off += elen + 1;
  }
  if (!out) {
    kfree(data);
    return 0;
  }
  e->data = data;
  e->len = out;
  return 0;
}

void env_free(struct env *e)
{
  kfree(e->data);
  e->data = 0;
  e->len = 0;
}
