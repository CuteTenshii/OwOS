/* Host test for the installed-file manifest (kernel/pkgdb.c). */
#define OWOS_SUITE_TEST
#include "../kernel/crc32c.h"
#include "../kernel/pkgdb.h"
#include "test.h"
#include <string.h>

static const struct pkg_rec rec_a = {0x0012abcdu, 0, PKG_BASE, "v0.6.0", "/bin/init"};
static const struct pkg_rec rec_b = {0xffffffffu, 4294967295u, PKG_UPDATE, "v1.0.0-2-gabc1234", "/etc/fonts/A B.ttf"};

static uint32_t build_two(uint8_t *out, uint32_t cap)
{
  uint32_t len = pkgdb_init(out, cap);
  expect_eq(pkgdb_append(out, cap, &len, &rec_a), 0);
  expect_eq(pkgdb_append(out, cap, &len, &rec_b), 0);
  return len;
}

static void check_round_trip(void)
{
  uint8_t buf[256];
  uint32_t len = build_two(buf, sizeof buf);
  /* install.c sizes its buffer with pkgdb_record_size. */
  expect_eq(len, PKGDB_HEADER_SIZE + pkgdb_record_size(&rec_a) + pkgdb_record_size(&rec_b));

  struct pkg_rec r[2];
  expect_eq(pkgdb_parse(buf, len, r, 2), 2);
  expect_eq(r[0].crc, rec_a.crc);
  expect_string(r[0].version, rec_a.version);
  expect_eq(r[1].size, rec_b.size);
  expect_eq(r[1].origin, PKG_UPDATE);
  expect_string(r[1].path, rec_b.path);
}

static void check_rejects_damage(void)
{
  uint8_t good[256], buf[256];
  uint32_t len = build_two(good, sizeof good);
  struct pkg_rec r[4];

  for (uint32_t cut = 0; cut < len; cut++)
    expect_eq(pkgdb_parse(good, cut, r, 4), -1);

  memcpy(buf, good, len);
  buf[len - 3] ^= 0x01;
  expect_eq(pkgdb_parse(buf, len, r, 4), -1);

  /* The count sits outside the checksum, so the record walk has to catch a wrong one. */
  memcpy(buf, good, len);
  buf[8] = 1;
  expect_eq(pkgdb_parse(buf, len, r, 4), -1);

  /* A well-checksummed record whose path is relative. */
  memcpy(buf, good, len);
  buf[PKGDB_HEADER_SIZE + 12 + sizeof "v0.6.0"] = 'b';
  uint32_t crc = crc32c_raw(0xFFFFFFFFu, buf + PKGDB_HEADER_SIZE, len - PKGDB_HEADER_SIZE) ^ 0xFFFFFFFFu;
  memcpy(buf + 12, &crc, 4);
  expect_eq(pkgdb_parse(buf, len, r, 4), -1);
}

static void check_append_overflow_leaves_buffer_intact(void)
{
  uint8_t buf[32], before[32];
  uint32_t len = pkgdb_init(buf, sizeof buf);
  memcpy(before, buf, sizeof buf);
  expect_eq(pkgdb_append(buf, sizeof buf, &len, &rec_b), -1);
  expect_eq(len, PKGDB_HEADER_SIZE);
  expect_eq(memcmp(buf, before, sizeof buf), 0);
}

static void check_find(void)
{
  struct pkg_rec r[] = {
      {1, 1, PKG_BASE, "v1", "/usr/share/x"}, {2, 2, PKG_BASE, "v1", "/bin/sh"}, {3, 3, PKG_BASE, "v1", "/bin/init"}};
  pkgdb_sort(r, 3);
  expect_eq(pkgdb_find(r, 3, "/bin/sh")->crc, 2);
  expect_eq(pkgdb_find(r, 3, "/usr/share/x")->crc, 1);
  expect(pkgdb_find(r, 3, "/bin/s") == 0);
}

static int vercmp(const char *a, const char *b)
{
  int c = 99;
  return pkgdb_vercmp(a, b, &c) ? c : 99;
}

static void check_vercmp(void)
{
  expect_eq(vercmp("v0.10.0", "v0.9.9"), 1);
  expect_eq(vercmp("v0.6.0-3-gabc1234", "v0.6.0"), 1);
  expect_eq(vercmp("v0.6.0-3-gabc1234", "v0.6.0-12-gdef5678-dirty"), -1);
  expect_eq(vercmp("v0.6.0-dirty", "v0.6.0"), 0);
  expect_eq(vercmp("abc1234", "v0.6.0"), 99);
}

static void check_decide(void)
{
  struct pkg_rec base = {7, 100, PKG_BASE, "v0.6.0", "/bin/a"};
  struct pkg_rec upd = {9, 200, PKG_UPDATE, "v0.7.0", "/bin/a"};

  expect_eq(pkgdb_decide(0, 100, 7, 100, "v0.6.0"), PKG_INSTALL);
  expect_eq(pkgdb_decide(&base, 100, 7, 100, "v0.6.0"), PKG_KEEP);
  expect_eq(pkgdb_decide(&base, -1, 7, 100, "v0.6.0"), PKG_INSTALL);
  /* Same size, new contents: the case a one-line fix produces. */
  expect_eq(pkgdb_decide(&base, 100, 8, 100, "v0.6.0"), PKG_INSTALL);

  expect_eq(pkgdb_decide(&upd, 200, 7, 100, "v0.7.0"), PKG_KEEP);
  expect_eq(pkgdb_decide(&upd, 200, 7, 100, "v0.7.0-1-gabc1234"), PKG_INSTALL);
  expect_eq(pkgdb_decide(&upd, 150, 7, 100, "v0.6.0"), PKG_INSTALL);
  expect_eq(pkgdb_decide(&upd, 200, 7, 100, "abc1234"), PKG_INSTALL);
}

int main(void)
{
  test("append writes records parse reads back", check_round_trip);
  test("truncated, corrupted or inconsistent manifests are rejected", check_rejects_damage);
  test("an append that does not fit leaves the buffer as it was", check_append_overflow_leaves_buffer_intact);
  test("find locates records after sorting", check_find);
  test("git describe versions order by tag then commit count", check_vercmp);
  test("decide keeps current files and newer updates only", check_decide);
  return 0;
}
