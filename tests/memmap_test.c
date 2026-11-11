/* memmap_test.c: carving reserved ranges out of RAM (kernel/arch/common/memmap.c). */
#define OWOS_SUITE_TEST
#include "../kernel/arch/common/memmap.h"
#include "test.h"

#define MB (1ULL << 20)

static void check_no_reservations(void)
{
  struct fdt_range out[4];
  expect_eq(memmap_subtract(0x40000000, 256 * MB, 0, 0, out, 4), 1);
  expect_eq(out[0].base, 0x40000000);
  expect_eq(out[0].size, 256 * MB);
}

static void check_middle_split(void)
{
  struct fdt_range out[4], rsv[] = {{0x43000000, 2 * MB}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, rsv, 1, out, 4), 2);
  expect_eq(out[0].base, 0x40000000);
  expect_eq(out[0].size, 48 * MB);
  expect_eq(out[1].base, 0x43200000);
  expect_eq(out[1].size, 206 * MB);
}

static void check_edges_and_outside(void)
{
  struct fdt_range out[4], rsv[] = {{0x3f000000, 16 * MB}, {0x4ff00000, 4 * MB}, {0x80000000, MB}, {0x40000000, 0}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, rsv, 4, out, 4), 1);
  expect_eq(out[0].base, 0x40000000);
  expect_eq(out[0].size, 255 * MB);
}

static void check_full_cover_and_overlaps(void)
{
  struct fdt_range out[4], all[] = {{0x30000000, 1ULL << 30}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, all, 1, out, 4), 0);

  struct fdt_range two[] = {{0x41000000, 4 * MB}, {0x41200000, 4 * MB}, {0x41000000, MB}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, two, 3, out, 4), 2);
  expect_eq(out[0].size, 16 * MB);
  expect_eq(out[1].base, 0x41600000);
  expect_eq(out[1].base + out[1].size, 0x50000000);
}

static void check_unordered_many(void)
{
  struct fdt_range out[8], rsv[] = {{0x46000000, MB}, {0x42000000, MB}, {0x44000000, MB}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, rsv, 3, out, 8), 4);
  expect_eq(out[0].base, 0x40000000);
  expect_eq(out[1].base, 0x42100000);
  expect_eq(out[2].base, 0x44100000);
  expect_eq(out[3].base, 0x46100000);
  expect_eq(out[3].size, 0x50000000 - 0x46100000);
}

static void check_overflow_and_wrap(void)
{
  struct fdt_range out[2], rsv[] = {{0x41000000, MB}, {0x42000000, MB}};
  expect_eq(memmap_subtract(0x40000000, 256 * MB, rsv, 2, out, 2), -1);
  expect_eq(memmap_subtract(0x40000000, 256 * MB, 0, 0, out, 0), -1);

  struct fdt_range wrap[] = {{0x48000000, ~0ULL}}; /* size wraps: treated as running to the top */
  expect_eq(memmap_subtract(0x40000000, 256 * MB, wrap, 1, out, 2), 1);
  expect_eq(out[0].size, 128 * MB);
}

int main(void)
{
  test("no reservations", check_no_reservations);
  test("reservation in the middle splits", check_middle_split);
  test("edges and ranges outside RAM", check_edges_and_outside);
  test("full cover and overlapping reservations", check_full_cover_and_overlaps);
  test("unordered reservations", check_unordered_many);
  test("piece overflow and wrapped sizes", check_overflow_and_wrap);
  return 0;
}
