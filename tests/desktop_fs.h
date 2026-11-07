/* Host stand-in for the guest readdir desktop.c calls; force-included by run-tests.sh. */
#ifndef OWOS_TEST_DESKTOP_FS_H
#define OWOS_TEST_DESKTOP_FS_H

struct dirent_u {
  char name[64];
  int is_dir;
  unsigned size;
};

int desktop_test_readdir(const char *path, int idx, struct dirent_u *out);

#endif
