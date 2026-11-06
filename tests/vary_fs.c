/* Host implementation of tests/vary_fs.h, confined to the directory named by CACHE_DIR. */
#include "vary_fs.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *g_stream;

static int in_cache_dir(const char *path) { return strncmp(path, CACHE_DIR, strlen(CACHE_DIR)) == 0; }

int vary_mkdir(const char *path)
{
  struct stat st;
  if (!in_cache_dir(path))
    return -1;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
    return -1;
  return mkdir(path, 0755);
}

int vary_unlink(const char *path) { return in_cache_dir(path) ? unlink(path) : -1; }

int vary_readdir(const char *path, int idx, struct dirent_u *out)
{
  DIR *d = opendir(path);
  if (!d)
    return -1;
  int seen = 0, found = 0;
  for (struct dirent *e; !found && (e = readdir(d));) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (seen++ == idx) {
      snprintf(out->name, sizeof out->name, "%s", e->d_name);
      out->is_dir = e->d_type == DT_DIR;
      out->size = 0;
      found = 1;
    }
  }
  closedir(d);
  return found;
}

int stream_begin(const char *name)
{
  g_stream = in_cache_dir(name) ? fopen(name, "wb") : NULL;
  return g_stream ? 0 : -1;
}

int stream_write(const void *buf, int len) { return fwrite(buf, 1, (size_t)len, g_stream) == (size_t)len ? len : -1; }

int stream_end(void)
{
  int rc = fclose(g_stream);
  g_stream = NULL;
  return rc;
}
