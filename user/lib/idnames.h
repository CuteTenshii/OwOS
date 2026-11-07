/* idnames.h: user/group names from /etc/passwd and /etc/group. */
#ifndef OWOS_IDNAMES_H
#define OWOS_IDNAMES_H

#include "owos.h"
#include <stdbool.h>

struct idnames {
  char *passwd; /* NULL when unreadable (non-root, or absent) */
  char *group;
  unsigned self_uid; /* the caller, known from /proc/user even when /etc/passwd is unreadable */
  char self_name[32];
  bool have_self;
};

static inline char *idn_slurp(const char *path)
{
  struct statinfo st;
  if (stat(path, &st) != 0 || st.is_dir)
    return NULL;
  char *buf = malloc(st.size + 1);
  if (!buf)
    return NULL;
  int n = st.size ? readfile(path, buf, (int)st.size) : 0;
  if (n < 0) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

static inline void idn_load(struct idnames *n)
{
  n->passwd = idn_slurp("/etc/passwd");
  n->group = idn_slurp("/etc/group");
  n->self_uid = (unsigned)getuid();
  n->have_self = user_ident(n->self_name, sizeof n->self_name, NULL, 0) != 0;
}

static inline void idn_free(struct idnames *n)
{
  free(n->passwd);
  free(n->group);
  n->passwd = n->group = NULL;
}

/* Copy colon field `idx` of the line starting at p into out. */
static inline void idn_field(const char *p, int idx, char *out, int cap)
{
  for (int f = 0; f < idx && *p && *p != '\n'; p++)
    if (*p == ':')
      f++;
  int i = 0;
  while (*p && *p != ':' && *p != '\n' && i < cap - 1)
    out[i++] = *p++;
  out[i] = '\0';
}

static inline bool idn_decimal(const char *s, unsigned *out)
{
  unsigned v = 0;
  if (!*s)
    return false;
  for (; *s; s++) {
    if (*s < '0' || *s > '9')
      return false;
    v = v * 10 + (unsigned)(*s - '0');
  }
  *out = v;
  return true;
}

/* Copy field `out_idx` of the `text` line whose field `key_idx` equals `key` into out. */
static inline bool idn_find(const char *text, int key_idx, const char *key, int out_idx, char *out, int cap)
{
  char f[64];
  for (const char *p = text; p && *p;) {
    idn_field(p, key_idx, f, sizeof f);
    if (f[0] && streq(f, key)) {
      idn_field(p, out_idx, out, cap);
      return true;
    }
    while (*p && *p != '\n')
      p++;
    if (*p == '\n')
      p++;
  }
  return false;
}

/* Name of uid / gid, or its decimal form when unknown. */
static inline void idn_user_name(const struct idnames *n, unsigned uid, char *out, int cap)
{
  char key[16];
  snprintf(key, sizeof key, "%u", uid);
  if (n->passwd && idn_find(n->passwd, 2, key, 0, out, cap))
    return;
  strlcpy(out, n->have_self && uid == n->self_uid ? n->self_name : uid == 0 ? "root" : key, cap);
}

static inline void idn_group_name(const struct idnames *n, unsigned gid, char *out, int cap)
{
  char key[16];
  snprintf(key, sizeof key, "%u", gid);
  if (!n->group || !idn_find(n->group, 1, key, 0, out, cap))
    strlcpy(out, gid == 0 ? "root" : key, cap);
}

/* A numeric id, or a name looked up in the database. */
static inline bool idn_parse_user(const struct idnames *n, const char *s, unsigned *uid)
{
  char f[16];
  if (idn_decimal(s, uid))
    return true;
  if (!n->passwd || !idn_find(n->passwd, 0, s, 2, f, sizeof f))
    return false;
  return idn_decimal(f, uid);
}

static inline bool idn_parse_group(const struct idnames *n, const char *s, unsigned *gid)
{
  char f[16];
  if (idn_decimal(s, gid))
    return true;
  if (!n->group || !idn_find(n->group, 0, s, 1, f, sizeof f))
    return false;
  return idn_decimal(f, gid);
}

#endif
