#include "ascii.h"
#include "gfx.h"
#include "owos.h"

#define FILELIST_CAP0 64

static int cmp_ci(const char *a, const char *b)
{
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

/* Directories first, then by name without regard to case */
static int before(const struct filelist_entry *a, const struct filelist_entry *b)
{
  if (a->is_dir != b->is_dir)
    return a->is_dir;
  return cmp_ci(a->name, b->name) < 0;
}

int filelist_match(const char *name, const char *const *exts)
{
  if (!exts)
    return 1;
  int n = strlen(name);
  for (; *exts; exts++) {
    int l = strlen(*exts);
    if (n >= l && streq_ci(name + n - l, *exts))
      return 1;
  }
  return 0;
}

int filelist_read(const char *dir, const char *const *exts, int dirs_only, struct filelist_entry **out)
{
  int cap = FILELIST_CAP0, n = 0;
  struct filelist_entry *v = malloc(cap * sizeof *v);
  if (!v)
    return -1;
  struct dirent_u d;
  for (int i = 0; n < FILELIST_MAX && readdir(dir, i, &d) == 1; i++) {
    if (!d.is_dir && (dirs_only || !filelist_match(d.name, exts)))
      continue;
    if (n == cap) {
      struct filelist_entry *g = realloc(v, 2 * cap * sizeof *v);
      if (!g) {
        free(v);
        return -1;
      }
      v = g;
      cap *= 2;
    }
    struct filelist_entry e;
    strlcpy(e.name, d.name, sizeof e.name);
    e.is_dir = d.is_dir;
    e.size = d.size;
    int j = n;
    while (j > 0 && before(&e, &v[j - 1]))
      j--;
    memmove(&v[j + 1], &v[j], (n - j) * sizeof *v);
    v[j] = e;
    n++;
  }
  *out = v;
  return n;
}

void filelist_join(char *out, int outsz, const char *dir, const char *name)
{
  if (name[0] == '/')
    snprintf(out, outsz, "%s", name);
  else if (!dir[0] || streq(dir, "/"))
    snprintf(out, outsz, "/%s", name);
  else
    snprintf(out, outsz, "%s/%s", dir, name);
}

int filelist_parent(char *path)
{
  int n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    path[--n] = 0;
  if (n <= 1)
    return 0;
  while (n > 0 && path[n - 1] != '/')
    n--;
  path[n > 1 ? n - 1 : 1] = 0;
  return 1;
}
