/* Host test for GPT lookup (kernel/part.c). */
#define OWOS_SUITE_TEST
#include "../kernel/part.h"
#include "crc32.h"
#include "test.h"
#include <string.h>

#define DISK_SECTORS 128
static uint8_t disk[DISK_SECTORS * 512];
static uint8_t *const hdr = disk + 512;
static uint8_t *const ents = disk + 1024;
#define ROOT_ENTRY (128u * 5u)

static int disk_read(uint32_t lba, void *buf)
{
  if (lba >= DISK_SECTORS)
    return -1;
  memcpy(buf, disk + lba * 512u, 512);
  return 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void seal(void)
{
  put32(hdr + 88, crc32(0, ents, 128u * 128u));
  put32(hdr + 16, 0);
  put32(hdr + 16, crc32(0, hdr, 92));
}

/* A GPT per the UEFI spec: header at LBA 1, 128 entries of 128 bytes from LBA 2, an ESP then an OwOS root. */
static void build(void)
{
  memset(disk, 0, sizeof disk);
  memcpy(hdr, "EFI PART", 8);
  put32(hdr + 8, 0x00010000);
  put32(hdr + 12, 92);
  put64(hdr + 24, 1);
  put64(hdr + 72, 2);
  put32(hdr + 80, 128);
  put32(hdr + 84, 128);
  memcpy(ents, PART_TYPE_ESP, 16);
  put64(ents + 32, 34);
  put64(ents + 40, 39);
  memcpy(ents + ROOT_ENTRY, PART_TYPE_OWOS_ROOT, 16); /* used entries need not be adjacent */
  put64(ents + ROOT_ENTRY + 32, 40);
  put64(ents + ROOT_ENTRY + 40, 63);
  seal();
}

static void check_finds_partitions(void)
{
  uint32_t start = 0, count = 0;
  build();
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), 0);
  expect_eq(start, 40);
  expect_eq(count, 24);
  expect_eq(part_find(disk_read, PART_TYPE_ESP, &start, &count), 0);
  expect_eq(start, 34);
  expect_eq(part_find(disk_read, PART_TYPE_BIOS_BOOT, &start, &count), -1);
}

static void check_rejects_bad_tables(void)
{
  uint32_t start, count;
  build();
  hdr[0] = 'X';
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), -1);

  build();
  hdr[56] ^= 1; /* the disk GUID changed without the header CRC */
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), -1);

  build();
  ents[ROOT_ENTRY + 40] = 62; /* an entry changed without the array CRC */
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), -1);

  build();
  put64(ents + ROOT_ENTRY + 40, 0x100000000ull); /* ends past what 32-bit LBAs reach */
  seal();
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), -1);
}

static int disk_write(uint32_t lba, const void *buf)
{
  if (lba >= DISK_SECTORS)
    return -1;
  memcpy(disk + lba * 512u, buf, 512);
  return 0;
}

static void check_written_table_reads_back(void)
{
  uint8_t random[48] = {1};
  struct part_spec parts[] = {{PART_TYPE_ESP, 34, 4, "EFI system"},
                              {PART_TYPE_OWOS_ROOT, 38, DISK_SECTORS - 33 - 38, "OwOS"}};
  memset(disk, 0xA5, sizeof disk);
  expect_eq(part_write_gpt(disk_write, DISK_SECTORS, parts, 2, random), 0);

  uint32_t start = 0, count = 0;
  expect_eq(part_find(disk_read, PART_TYPE_OWOS_ROOT, &start, &count), 0);
  expect_eq(start, 38);
  expect_eq(count, DISK_SECTORS - 33 - 38);
  expect_eq(disk[510], 0x55);
  expect_eq(disk[446 + 4], 0xEE);
}

static void check_refuses_bad_layouts(void)
{
  uint8_t random[48] = {0};
  struct part_spec overlap[] = {{PART_TYPE_ESP, 34, 8, 0}, {PART_TYPE_OWOS_ROOT, 40, 4, 0}};
  struct part_spec into_backup[] = {{PART_TYPE_OWOS_ROOT, 34, DISK_SECTORS - 33 - 33, 0}};
  memset(disk, 0xA5, sizeof disk);
  expect_eq(part_write_gpt(disk_write, DISK_SECTORS, overlap, 2, random), -1);
  expect_eq(part_write_gpt(disk_write, DISK_SECTORS, into_backup, 1, random), -1);
  expect_eq(disk[0], 0xA5); /* nothing written */
}

int main(void)
{
  test("finds partitions by type in a valid GPT", check_finds_partitions);
  test("rejects bad signatures, CRCs and out-of-range partitions", check_rejects_bad_tables);
  test("a written table reads back with its partitions", check_written_table_reads_back);
  test("bad layouts are refused before anything is written", check_refuses_bad_layouts);
  return 0;
}
