/* Host test for the archive reader (user/lib/archive): archives built by the host tar/gzip/zstd/brotli/zip are
 * opened, listed and extracted through the real API, plus hand-built hostile TARs and ZIPs. The library's guest
 * file calls (mkdir, stream_*) are renamed onto the host stand-ins below by scripts/tests.mk. */
#define OWOS_SUITE_TEST
#include "archive.h"
#include "archive_fs.h"
#include "test.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE *out_file;

int archive_test_mkdir(const char *path) { return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1; }
int archive_test_stream_begin(const char *path)
{
  out_file = fopen(path, "wb");
  return out_file ? 0 : -1;
}
int archive_test_stream_write(const void *buf, int len)
{
  return (out_file && (len == 0 || fwrite(buf, 1, (size_t)len, out_file) == (size_t)len)) ? len : -1;
}
int archive_test_stream_end(void)
{
  int rc = (out_file && fclose(out_file) == 0) ? 0 : -1;
  out_file = 0;
  return rc;
}

static char dir[512];

static void path_in(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

static int sh(const char *fmt, const char *arg)
{
  char cmd[2048];
  snprintf(cmd, sizeof cmd, fmt, arg, arg, arg);
  return system(cmd);
}

static int have(const char *tool)
{
  char cmd[256];
  snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
  return system(cmd) == 0;
}

static unsigned char *slurp(const char *path, long *len)
{
  FILE *f = fopen(path, "rb");
  *len = -1;
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(*len ? (size_t)*len : 1);
  if (fread(b, 1, (size_t)*len, f) != (size_t)*len)
    *len = -1;
  fclose(f);
  return b;
}

static void put(const char *path, const void *buf, size_t len)
{
  FILE *f = fopen(path, "wb");
  expect(f != 0);
  if (f) {
    expect(len == 0 || fwrite(buf, 1, len, f) == len);
    fclose(f);
  }
}

static void expect_same_file(const char *a, const char *b)
{
  long la, lb;
  unsigned char *da = slurp(a, &la), *db = slurp(b, &lb);
  expect(la >= 0);
  expect_eq(la, lb);
  expect(da && db && la == lb && memcmp(da, db, (size_t)la) == 0);
  free(da);
  free(db);
}

static int find_entry(struct archive *a, const char *name, struct arcent *out)
{
  for (int i = 0; i < archive_count(a); i++)
    if (archive_entry(a, i, out) == 0 && strcmp(out->name, name) == 0)
      return i;
  return -1;
}

/* src/hello.txt, src/sub/data.bin (70000 bytes, crosses the 4 KiB extract chunk), src/sub/empty. */
static void make_tree(void)
{
  char p[600];
  path_in(p, sizeof p, "src");
  archive_test_mkdir(p);
  path_in(p, sizeof p, "src/sub");
  archive_test_mkdir(p);
  path_in(p, sizeof p, "src/hello.txt");
  put(p, "hello world\n", 12);
  unsigned char *bin = malloc(70000);
  unsigned s = 1;
  for (int i = 0; i < 70000; i++) {
    s = s * 1103515245u + 12345u;
    bin[i] = (i % 3) ? (unsigned char)(s >> 24) : (unsigned char)'x';
  }
  path_in(p, sizeof p, "src/sub/data.bin");
  put(p, bin, 70000);
  free(bin);
  path_in(p, sizeof p, "src/sub/empty");
  put(p, "", 0);
}

static void check_built(const char *file, const char *want_fmt)
{
  char p[600], out[600], a_file[700], b_file[700];
  path_in(p, sizeof p, file);
  expect_string(archive_detect(p), want_fmt);
  struct archive *a = archive_open(p);
  expect(a != 0);
  if (!a)
    return;
  expect_string(archive_format(a), want_fmt);

  struct arcent e;
  expect(find_entry(a, "hello.txt", &e) >= 0 && e.size == 12 && !e.is_dir);
  expect(find_entry(a, "sub", &e) >= 0 && e.is_dir);
  expect(find_entry(a, "sub/data.bin", &e) >= 0 && e.size == 70000);
  expect(find_entry(a, "sub/empty", &e) >= 0 && e.size == 0);
  expect(archive_entry(a, archive_count(a), &e) < 0);

  snprintf(out, sizeof out, "%s/out-%s", dir, file);
  expect_eq(archive_extract_all(a, out), 0);
  static const char *const files[] = {"hello.txt", "sub/data.bin", "sub/empty"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(a_file, sizeof a_file, "%s/src/%s", dir, files[i]);
    snprintf(b_file, sizeof b_file, "%s/%s", out, files[i]);
    expect_same_file(a_file, b_file);
  }
  archive_close(a);
}

static void check_host_archives(void)
{
  make_tree();
  if (!have("tar")) {
    printf("archive: tar not installed, skipping host-built archives\n");
    return;
  }
  expect_eq(sh("tar -C '%s/src' -cf '%s/t.tar' hello.txt sub", dir), 0);
  check_built("t.tar", "TAR");
  if (have("gzip")) {
    expect_eq(sh("gzip -9 -c '%s/t.tar' > '%s/t.tar.gz'", dir), 0);
    check_built("t.tar.gz", "TAR.GZ");
  }
  if (have("zstd")) {
    expect_eq(sh("zstd -q -19 -c '%s/t.tar' > '%s/t.tar.zst'", dir), 0);
    check_built("t.tar.zst", "TAR.ZST");
  }
  if (have("brotli")) {
    expect_eq(sh("brotli -q 9 -c '%s/t.tar' > '%s/t.tar.br'", dir), 0);
    struct archive *a;
    char p[600];
    path_in(p, sizeof p, "t.tar.br");
    /* Brotli has no magic number, so detection can't name it; open still finds it by trial decode. */
    expect_string(archive_detect(p), "unknown");
    a = archive_open(p);
    expect(a != 0);
    if (a)
      expect_string(archive_format(a), "TAR.BR");
    archive_close(a);
  }
  if (have("zip")) {
    expect_eq(sh("cd '%s/src' && zip -q -r '%s/t.zip' hello.txt sub", dir), 0);
    check_built("t.zip", "ZIP");
  }
}

static unsigned tar_sum(const unsigned char *h)
{
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += (i >= 148 && i < 156) ? 32u : h[i];
  return sum;
}

static void tar_header(unsigned char *h, const char *name, unsigned size, char type)
{
  memset(h, 0, 512);
  memcpy(h, name, strlen(name) < 100 ? strlen(name) : 100);
  snprintf((char *)h + 100, 8, "%07o", 0644);
  snprintf((char *)h + 124, 12, "%011o", size);
  snprintf((char *)h + 136, 12, "%011o", 0);
  h[156] = (unsigned char)type;
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  snprintf((char *)h + 148, 8, "%06o", tar_sum(h));
  h[155] = ' ';
}

/* Appends one entry (header + padded data) at *pos in buf. */
static void tar_add(unsigned char *buf, size_t *pos, const char *name, const void *data, unsigned size, char type)
{
  tar_header(buf + *pos, name, size, type);
  *pos += 512;
  memcpy(buf + *pos, data, size);
  *pos += (size + 511) / 512 * 512;
}

/* A GNU long name far past ARC_NAME_MAX must be truncated, not copied over the parser's stack buffer. */
static void check_long_name(void)
{
  static unsigned char tar[8192];
  char longname[1001], p[600];
  size_t pos = 0;
  memset(tar, 0, sizeof tar);
  memset(longname, 'a', 1000);
  longname[1000] = 0;
  tar_add(tar, &pos, "././@LongLink", longname, 1001, 'L');
  tar_add(tar, &pos, "short", "data", 4, '0');
  path_in(p, sizeof p, "long.tar");
  put(p, tar, pos + 1024);
  struct archive *a = archive_open(p);
  expect(a != 0);
  struct arcent e;
  expect(a && archive_count(a) == 1);
  expect(a && archive_entry(a, 0, &e) == 0 && strlen(e.name) == ARC_NAME_MAX - 1 && e.size == 4);
  archive_close(a);
}

static void check_traversal(void)
{
  static unsigned char tar[8192];
  char p[600], probe[700];
  size_t pos = 0;
  memset(tar, 0, sizeof tar);
  tar_add(tar, &pos, "../escape.txt", "x", 1, '0');
  tar_add(tar, &pos, "/abs.txt", "x", 1, '0');
  tar_add(tar, &pos, "a/../../up.txt", "x", 1, '0');
  tar_add(tar, &pos, "ok.txt", "fine", 4, '0');
  path_in(p, sizeof p, "evil.tar");
  put(p, tar, pos + 1024);
  struct archive *a = archive_open(p);
  expect(a != 0 && archive_count(a) == 4);
  path_in(p, sizeof p, "jail");
  archive_test_mkdir(p);
  path_in(p, sizeof p, "jail/inner");
  expect(a && archive_extract_all(a, p) == 0);
  struct stat st;
  snprintf(probe, sizeof probe, "%s/jail/inner/ok.txt", dir);
  expect(stat(probe, &st) == 0 && st.st_size == 4);
  snprintf(probe, sizeof probe, "%s/jail/escape.txt", dir);
  expect(stat(probe, &st) != 0);
  snprintf(probe, sizeof probe, "%s/jail/up.txt", dir);
  expect(stat(probe, &st) != 0);
  expect(stat("/abs.txt", &st) != 0);
  archive_close(a);
}

/* A ZIP whose method is neither store nor deflate is listed, but extracting it must fail. */
static void check_unsupported_zip_method(void)
{
  if (!have("zip"))
    return;
  char p[600], out[600];
  expect_eq(sh("cd '%s/src' && zip -q -0 '%s/odd.zip' hello.txt", dir), 0);
  path_in(p, sizeof p, "odd.zip");
  long len;
  unsigned char *z = slurp(p, &len);
  expect(z && len > 0);
  for (long i = 0; z && i + 12 <= len; i++) {
    if (!memcmp(z + i, "PK\3\4", 4))
      z[i + 8] = 99;
    else if (!memcmp(z + i, "PK\1\2", 4))
      z[i + 10] = 99;
  }
  put(p, z, z ? (size_t)len : 0);
  free(z);
  struct archive *a = archive_open(p);
  struct arcent e;
  expect(a != 0);
  expect(a && find_entry(a, "hello.txt", &e) == 0 && e.method == 99);
  path_in(out, sizeof out, "odd.out");
  expect(a && archive_extract(a, 0, out) < 0);
  archive_close(a);
}

static void check_rejects_garbage(void)
{
  char p[600];
  unsigned char junk[2048];
  for (size_t i = 0; i < sizeof junk; i++)
    junk[i] = (unsigned char)(i * 131 + 7);
  path_in(p, sizeof p, "junk.bin");
  put(p, junk, sizeof junk);
  expect_string(archive_detect(p), "unknown");
  expect(archive_open(p) == 0);
  path_in(p, sizeof p, "missing.tar");
  expect_string(archive_detect(p), "unknown");
  expect(archive_open(p) == 0);
}

int main(void)
{
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/owos-archive-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  expect(mkdtemp(dir) != 0);

  test("host-built archives", check_host_archives);
  test("GNU long name overflow", check_long_name);
  test("path traversal", check_traversal);
  test("unsupported ZIP method", check_unsupported_zip_method);
  test("garbage input", check_rejects_garbage);

  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  (void)!system(cmd);
  return 0;
}
