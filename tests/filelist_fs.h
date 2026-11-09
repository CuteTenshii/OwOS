/* Host stand-in for the guest readdir filelist.c calls; force-included by scripts/tests.mk. */
#ifndef OWOS_TEST_FILELIST_FS_H
#define OWOS_TEST_FILELIST_FS_H

struct dirent_u {
  char name[64];
  int is_dir;
  unsigned size;
};

int filelist_test_readdir(const char *path, int idx, struct dirent_u *out);

#endif
