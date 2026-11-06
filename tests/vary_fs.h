/* Host stand-ins for the guest file calls cache.c makes; force-included into cache.c by scripts/run-tests.sh. */
#ifndef OWOS_TEST_VARY_FS_H
#define OWOS_TEST_VARY_FS_H

struct dirent_u {
  char name[64];
  int is_dir;
  unsigned size;
};

int vary_mkdir(const char *path);
int vary_unlink(const char *path);
int vary_readdir(const char *path, int idx, struct dirent_u *out);
int stream_begin(const char *name);
int stream_write(const void *buf, int len);
int stream_end(void);

#endif
