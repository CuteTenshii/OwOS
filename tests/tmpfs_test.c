/* tmpfs_test.c: host test for kernel/tmpfs.c: paths, ownership, rename and write_at rules, the heap quota, and that a
 * failed allocation never loses a file's old contents. Run with `make test`. */
#define OWOS_SUITE_TEST
#define OWOS_CONSOLE_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H
#define OWOS_SCHED_H
#define OWOS_WALLCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct timeval64 {
  long long tv_sec;
  int tv_usec;
};

static long long g_clock = 1700000000LL;
static size_t g_heap_free = 1u << 30;
static long g_fail_alloc = -1; /* the allocation that fails, counting from 0; -1 never */
static long g_allocs, g_live;
static uint32_t g_uid, g_gid, g_umask = 022;
static char g_out[4096];
static size_t g_out_len;

static void wallclock_get(struct timeval64 *tv)
{
  tv->tv_sec = g_clock;
  tv->tv_usec = 0;
}

static void *kmalloc(size_t n)
{
  if (g_allocs++ == g_fail_alloc)
    return NULL;
  void *p = malloc(n);
  if (p)
    g_live++;
  return p;
}

static void kfree(void *p)
{
  if (p)
    g_live--;
  free(p);
}

static size_t heap_free_bytes(void) { return g_heap_free; }
static uint32_t sched_uid(void) { return g_uid; }
static uint32_t sched_gid(void) { return g_gid; }
static uint32_t sched_umask(void) { return g_umask; }

static void con_write(const char *s)
{
  int n = snprintf(g_out + g_out_len, sizeof g_out - g_out_len, "%s", s);
  if (n > 0 && (size_t)n < sizeof g_out - g_out_len)
    g_out_len += (size_t)n;
}

static void con_putc(char c)
{
  char s[2] = {c, '\0'};
  con_write(s);
}

#include "../kernel/tmpfs.c"
#include "test.h"

static const struct fs_ops *ops;

static void *fresh(void)
{
  g_heap_free = 1u << 30;
  g_fail_alloc = -1;
  g_allocs = 0;
  g_uid = g_gid = 0;
  g_umask = 022;
  ops = tmpfs_ops();
  void *fs = tmpfs_new();
  expect(fs != NULL);
  return fs;
}

static void done(void *fs)
{
  ops->close(fs);
  expect_eq(g_live, 0);
}

static int put(void *fs, const char *path, const char *text) { return ops->create(fs, path, text, strlen(text)); }

static void expect_file(void *fs, const char *path, const char *want)
{
  char buf[256] = {0};
  int is_dir = -1;
  uint32_t size = 0;
  expect_eq(ops->stat(fs, path, &is_dir, &size), 0);
  expect_eq(is_dir, 0);
  expect_eq(size, strlen(want));
  expect_eq(ops->read_at(fs, path, 0, buf, sizeof buf - 1), (int)strlen(want));
  expect_string(buf, want);
}

static bool exists(void *fs, const char *path) { return ops->stat(fs, path, NULL, NULL) == 0; }

static void t_root(void)
{
  void *fs = fresh();
  uint32_t mode = 0, uid = 99, gid = 99;
  int is_dir = 0;
  expect_eq(ops->getattr(fs, "", &mode, &uid, &gid), 0);
  expect_eq(mode, 01777);
  expect_eq(uid, 0);
  expect_eq(gid, 0);
  expect_eq(ops->stat(fs, "/", &is_dir, NULL), 0);
  expect_eq(is_dir, 1);
  expect_string(ops->type(fs), "tmpfs");
  expect_eq(ops->del(fs, "/"), -1);
  expect_eq(ops->rmdir(fs, "/"), -1);
  expect_eq(ops->mkdir(fs, "/"), -1);
  expect_eq(put(fs, "/", "x"), -1);
  expect_eq(ops->rename(fs, "/", "/x"), -1);
  done(fs);
}

static void t_files(void)
{
  void *fs = fresh();
  expect_eq(put(fs, "/a.txt", "hello"), 0);
  expect_file(fs, "/a.txt", "hello");
  expect_eq(put(fs, "a.txt", "a longer replacement"), 0);
  expect_file(fs, "//a.txt", "a longer replacement");
  expect_eq(put(fs, "/a.txt", ""), 0);
  expect_file(fs, "/a.txt", "");

  char buf[8];
  expect_eq(put(fs, "/b", "0123456789"), 0);
  expect_eq(ops->read_at(fs, "/b", 7, buf, sizeof buf), 3);
  expect(memcmp(buf, "789", 3) == 0);
  expect_eq(ops->read_at(fs, "/b", 10, buf, sizeof buf), 0);
  expect_eq(ops->read_at(fs, "/missing", 0, buf, sizeof buf), -1);
  expect_eq(ops->create(fs, "/c", NULL, 4), -1);

  expect_eq(ops->del(fs, "/b"), 0);
  expect(!exists(fs, "/b"));
  expect_eq(ops->del(fs, "/b"), -1);
  done(fs);
}

static void t_dirs(void)
{
  void *fs = fresh();
  expect_eq(ops->mkdir(fs, "/d"), 0);
  expect_eq(ops->mkdir(fs, "/d"), -1);
  expect_eq(ops->mkdir(fs, "/d/e/"), 0);
  expect_eq(put(fs, "/d/e/f", "deep"), 0);
  expect_file(fs, "/d/./e/../e/f", "deep");
  expect_file(fs, "/../d/e/f", "deep");
  expect_eq(put(fs, "/nodir/f", "x"), -1);
  expect_eq(put(fs, "/d/e/f/g", "x"), -1);
  expect_eq(put(fs, "/d", "x"), -1);
  expect_eq(ops->mkdir(fs, "/d/e/f/sub"), -1);

  expect_eq(ops->del(fs, "/d/e"), -1);
  expect_eq(ops->rmdir(fs, "/d/e"), -1);
  expect_eq(ops->rmdir(fs, "/d/e/f"), -1);
  expect_eq(ops->del(fs, "/d/e/f"), 0);
  expect_eq(ops->rmdir(fs, "/d/e"), 0);

  expect_eq(put(fs, "/d/one", "1"), 0);
  expect_eq(ops->mkdir(fs, "/d/two"), 0);
  char name[TMPFS_NAME_LEN];
  int is_dir;
  uint32_t size;
  int seen_one = 0, seen_two = 0, n = 0;
  while (ops->enum_dir(fs, "/d", n, name, &is_dir, &size)) {
    seen_one += !strcmp(name, "one") && !is_dir && size == 1;
    seen_two += !strcmp(name, "two") && is_dir;
    n++;
  }
  expect_eq(n, 2);
  expect_eq(seen_one, 1);
  expect_eq(seen_two, 1);
  expect_eq(ops->enum_dir(fs, "/d", -1, name, &is_dir, &size), 0);
  expect_eq(ops->enum_dir(fs, "/d/one", 0, name, &is_dir, &size), 0);
  done(fs);
}

static void t_names(void)
{
  void *fs = fresh();
  char longest[TMPFS_NAME_LEN + 2] = "/";
  memset(longest + 1, 'n', TMPFS_NAME_LEN - 1);
  expect_eq(put(fs, longest, "max"), 0);
  expect_file(fs, longest, "max");

  char name[TMPFS_NAME_LEN];
  expect_eq(ops->enum_dir(fs, "/", 0, name, NULL, NULL), 1);
  expect_eq(strlen(name), TMPFS_NAME_LEN - 1);

  char too_long[TMPFS_NAME_LEN + 2] = "/";
  memset(too_long + 1, 'n', TMPFS_NAME_LEN);
  expect_eq(put(fs, too_long, "x"), -1);
  expect_eq(ops->mkdir(fs, too_long), -1);
  expect(!exists(fs, too_long));
  done(fs);
}

static void t_owner(void)
{
  void *fs = fresh();
  g_uid = 1000;
  g_gid = 100;
  g_umask = 027;
  expect_eq(put(fs, "/mine", "x"), 0);
  expect_eq(ops->mkdir(fs, "/dir"), 0);

  uint32_t mode, uid, gid;
  expect_eq(ops->getattr(fs, "/mine", &mode, &uid, &gid), 0);
  expect_eq(mode, 0640);
  expect_eq(uid, 1000);
  expect_eq(gid, 100);
  expect_eq(ops->getattr(fs, "/dir", &mode, &uid, &gid), 0);
  expect_eq(mode, 0750);

  g_uid = 0;
  expect_eq(put(fs, "/mine", "rewritten by root"), 0);
  expect_eq(ops->getattr(fs, "/mine", &mode, &uid, &gid), 0);
  expect_eq(uid, 1000);
  expect_eq(ops->getattr(fs, "/missing", &mode, &uid, &gid), -1);

  g_out_len = 0;
  g_out[0] = '\0';
  expect_eq(ops->list(fs, "/"), 0);
  expect(strstr(g_out, "-rw-r-----        17  mine\n") != NULL);
  expect(strstr(g_out, "drwxr-x---     <DIR>  dir/\n") != NULL);
  done(fs);
}

static void t_mtime(void)
{
  void *fs = fresh();
  uint32_t t = 0;
  g_clock = 1700000100;
  expect_eq(put(fs, "/f", "x"), 0);
  expect_eq(ops->mtime(fs, "/f", &t), 0);
  expect_eq(t, 1700000100);
  expect_eq(ops->mtime(fs, "/", &t), 0);
  expect_eq(t, 1700000100);

  g_clock = 1700000200;
  expect_eq(ops->write_at(fs, "/f", 0, "y", 1), 1);
  expect_eq(ops->mtime(fs, "/f", &t), 0);
  expect_eq(t, 1700000200);
  expect_eq(ops->mtime(fs, "/missing", &t), -1);
  done(fs);
}

static void t_rename(void)
{
  void *fs = fresh();
  expect_eq(ops->mkdir(fs, "/d"), 0);
  expect_eq(ops->mkdir(fs, "/d/sub"), 0);
  expect_eq(put(fs, "/a", "A"), 0);
  expect_eq(put(fs, "/b", "B"), 0);

  expect_eq(ops->rename(fs, "/a", "/d/a2"), 0);
  expect(!exists(fs, "/a"));
  expect_file(fs, "/d/a2", "A");

  expect_eq(ops->rename(fs, "/b", "/d/a2"), 0);
  expect(!exists(fs, "/b"));
  expect_file(fs, "/d/a2", "B");

  expect_eq(ops->rename(fs, "/d/a2", "/d/a2"), 0);
  expect_file(fs, "/d/a2", "B");
  expect_eq(ops->rename(fs, "/d/a2", "/d/sub"), -1);
  expect_eq(ops->rename(fs, "/d", "/d/sub/inside"), -1);
  expect_eq(ops->rename(fs, "/d", "/d"), 0);
  expect_eq(ops->rename(fs, "/missing", "/x"), -1);
  expect_eq(ops->rename(fs, "/d/a2", "/nodir/x"), -1);
  expect_eq(ops->rename(fs, "/d/a2", "/d/.."), -1);

  expect_eq(ops->rename(fs, "/d", "/moved"), 0);
  expect_file(fs, "/moved/a2", "B");
  expect(exists(fs, "/moved/sub"));
  done(fs);
}

static void t_write_at(void)
{
  void *fs = fresh();
  expect_eq(put(fs, "/w", "abc"), 0);
  expect_eq(ops->write_at(fs, "/w", 1, "XY", 2), 2);
  expect_file(fs, "/w", "aXY");
  expect_eq(ops->write_at(fs, "/w", 5, "end", 3), 3);
  char buf[8];
  expect_eq(ops->read_at(fs, "/w", 0, buf, sizeof buf), 8);
  expect(memcmp(buf, "aXY\0\0end", 8) == 0);
  expect_eq(ops->write_at(fs, "/w", 0, "", 0), 0);
  expect_eq(ops->write_at(fs, "/missing", 0, "x", 1), -1);
  expect_eq(ops->mkdir(fs, "/dir"), 0);
  expect_eq(ops->write_at(fs, "/dir", 0, "x", 1), -1);
  expect_eq(ops->write_at(fs, "/w", UINT32_MAX, "xy", 2), -1);
  done(fs);
}

static void t_quota(void)
{
  void *fs = fresh();
  ops->close(fs);
  /* A quarter of the free heap: exactly the root, one file node and 1000 bytes of data. */
  const size_t node = sizeof(struct tnode);
  g_heap_free = 4 * (2 * node + 1000);
  fs = tmpfs_new();
  expect(fs != NULL);

  static char blob[2000];
  memset(blob, 'q', sizeof blob);
  expect_eq(ops->create(fs, "/big", blob, 1001), -1);
  expect_eq(ops->create(fs, "/big", blob, 1000), 0);
  expect_eq(ops->create(fs, "/other", blob, 1), -1);
  expect_eq(ops->create(fs, "/big", blob, 1000), 0);
  expect_eq(ops->write_at(fs, "/big", 999, "z", 1), 1);
  expect_eq(ops->write_at(fs, "/big", 1000, "z", 1), -1);
  expect_eq(ops->create(fs, "/big", blob, 400), 0);
  expect_eq(ops->create(fs, "/other", blob, 600 - node + 1), -1);
  expect_eq(ops->create(fs, "/other", blob, 600 - node), 0);
  expect_eq(ops->mkdir(fs, "/full"), -1);
  expect_eq(ops->del(fs, "/other"), 0);
  expect_eq(ops->mkdir(fs, "/full"), 0);
  expect_eq(ops->rmdir(fs, "/full"), 0);
  expect_eq(ops->del(fs, "/big"), 0);
  expect_eq(((struct tmpfs *)fs)->used, sizeof(struct tnode));
  done(fs);
}

static void t_alloc_failure(void)
{
  void *fs = fresh();
  expect_eq(put(fs, "/keep", "original"), 0);
  g_fail_alloc = g_allocs;
  expect_eq(put(fs, "/keep", "replacement"), -1);
  expect_file(fs, "/keep", "original");

  g_fail_alloc = g_allocs;
  expect_eq(ops->write_at(fs, "/keep", 8, "+more", 5), -1);
  expect_file(fs, "/keep", "original");

  g_fail_alloc = g_allocs + 1;
  expect_eq(put(fs, "/new", "data"), -1);
  expect(!exists(fs, "/new"));

  g_fail_alloc = g_allocs;
  expect_eq(ops->mkdir(fs, "/d"), -1);
  expect(!exists(fs, "/d"));
  done(fs);

  g_fail_alloc = g_allocs;
  expect(tmpfs_new() == NULL);
  g_fail_alloc = g_allocs + 1;
  expect(tmpfs_new() == NULL);
  expect_eq(g_live, 0);
}

static void t_teardown(void)
{
  void *fs = fresh();
  expect_eq(ops->mkdir(fs, "/a"), 0);
  expect_eq(ops->mkdir(fs, "/a/b"), 0);
  for (int i = 0; i < 50; i++) {
    char path[32];
    snprintf(path, sizeof path, "/a/b/file%d", i);
    expect_eq(put(fs, path, "content"), 0);
  }
  done(fs);
}

int main(void)
{
  test("root directory", t_root);
  test("create, read, overwrite and delete files", t_files);
  test("directories and path resolution", t_dirs);
  test("name length limit", t_names);
  test("owner and mode from the caller", t_owner);
  test("modification times", t_mtime);
  test("rename", t_rename);
  test("write_at extends with zeros", t_write_at);
  test("heap quota", t_quota);
  test("allocation failure keeps old contents", t_alloc_failure);
  test("close frees the whole tree", t_teardown);
  return 0;
}
