/* fdt_test.c: the AArch64 flattened device tree reader (kernel/arch/aarch64/fdt.c) against hand-built blobs,
 * including truncated and hostile ones. Blobs sit right before a PROT_NONE page so any read past the end faults. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/fdt.h"
#include "test.h"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define MAXBLOB 4096

struct blob {
  uint8_t st[MAXBLOB];
  uint32_t st_len;
  char strs[MAXBLOB];
  uint32_t strs_len;
  uint32_t reg_end; /* struct offset just past the memory node's reg property */
};

static void put32(struct blob *b, uint32_t v)
{
  for (int i = 3; i >= 0; i--)
    b->st[b->st_len++] = (uint8_t)(v >> (8 * i));
}

static void pad(struct blob *b)
{
  while (b->st_len & 3)
    b->st[b->st_len++] = 0;
}

static void begin(struct blob *b, const char *name)
{
  put32(b, 1);
  size_t n = strlen(name) + 1;
  memcpy(b->st + b->st_len, name, n);
  b->st_len += (uint32_t)n;
  pad(b);
}

static void end_node(struct blob *b) { put32(b, 2); }

static uint32_t name_off(struct blob *b, const char *name)
{
  uint32_t off = b->strs_len;
  memcpy(b->strs + off, name, strlen(name) + 1);
  b->strs_len += (uint32_t)strlen(name) + 1;
  return off;
}

static void prop(struct blob *b, const char *name, const uint32_t *cells, int n)
{
  put32(b, 3);
  put32(b, (uint32_t)n * 4);
  put32(b, name_off(b, name));
  for (int i = 0; i < n; i++)
    put32(b, cells[i]);
}

static void prop1(struct blob *b, const char *name, uint32_t v) { prop(b, name, &v, 1); }

/* root { #address-cells, #size-cells, [cpus {...}], memory@<unit> { reg } } */
static void build(struct blob *b, int addr_cells, int size_cells, const char *mem_name, uint64_t base, uint64_t size)
{
  memset(b, 0, sizeof *b);
  begin(b, "");
  if (addr_cells >= 0)
    prop1(b, "#address-cells", (uint32_t)addr_cells);
  if (size_cells >= 0)
    prop1(b, "#size-cells", (uint32_t)size_cells);
  begin(b, "cpus");
  prop1(b, "#address-cells", 1);
  begin(b, "memory@dead"); /* depth 3: must not count */
  uint32_t decoy[2] = {0, 0x1000};
  prop(b, "reg", decoy, 2);
  end_node(b);
  end_node(b);
  begin(b, mem_name);
  uint32_t cells[4];
  int n = 0;
  int ac = addr_cells < 0 ? 2 : addr_cells, sc = size_cells < 0 ? 1 : size_cells;
  if (ac == 2)
    cells[n++] = (uint32_t)(base >> 32);
  cells[n++] = (uint32_t)base;
  if (sc == 2)
    cells[n++] = (uint32_t)(size >> 32);
  cells[n++] = (uint32_t)size;
  prop(b, "reg", cells, n);
  b->reg_end = b->st_len;
  end_node(b);
  end_node(b);
  put32(b, 9);
}

static uint8_t *guard_page;

/* Serialise to the end of a mapping whose next page is inaccessible. */
static const uint8_t *serialize(const struct blob *b, uint32_t struct_len, uint32_t magic, uint32_t *total_out)
{
  uint32_t struct_off = 40, strs_off = 40 + struct_len;
  uint32_t total = strs_off + b->strs_len;
  if (!guard_page) {
    guard_page = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    mprotect(guard_page + 4096, 4096, PROT_NONE);
  }
  uint8_t *p = guard_page + 4096 - total;
  memset(guard_page, 0, 4096);
  uint32_t hdr[10] = {magic, total, struct_off, strs_off, 0, 17, 16, 0, b->strs_len, struct_len};
  for (int i = 0; i < 10; i++)
    for (int k = 0; k < 4; k++)
      p[i * 4 + k] = (uint8_t)(hdr[i] >> (8 * (3 - k)));
  memcpy(p + struct_off, b->st, struct_len);
  memcpy(p + strs_off, b->strs, b->strs_len);
  if (total_out)
    *total_out = total;
  return p;
}

static int parse(const struct blob *b, uint64_t *base, uint64_t *size)
{
  return fdt_memory(serialize(b, b->st_len, 0xd00dfeed, 0), base, size);
}

static void check_qemu_layout(void)
{
  struct blob b;
  uint64_t base = 0, size = 0;
  build(&b, 2, 2, "memory@40000000", 0x40000000, 256ULL << 20);
  expect_eq(parse(&b, &base, &size), 0);
  expect_eq(base, 0x40000000);
  expect_eq(size, 256ULL << 20);

  build(&b, 2, 2, "memory@8000000000", 0x8000000000ULL, 6ULL << 30);
  expect_eq(parse(&b, &base, &size), 0);
  expect_eq(base, 0x8000000000ULL);
  expect_eq(size, 6ULL << 30);
}

static void check_cell_widths(void)
{
  struct blob b;
  uint64_t base = 0, size = 0;
  build(&b, 1, 1, "memory", 0x80000000u, 0x10000000u);
  expect_eq(parse(&b, &base, &size), 0);
  expect_eq(base, 0x80000000u);
  expect_eq(size, 0x10000000u);

  build(&b, -1, -1, "memory@0", 0x1ffff0000ULL, 0x20000000u); /* defaults: 2 address cells, 1 size cell */
  expect_eq(parse(&b, &base, &size), 0);
  expect_eq(base, 0x1ffff0000ULL);
  expect_eq(size, 0x20000000u);
}

static void check_node_names(void)
{
  struct blob b;
  uint64_t base = 0, size = 0;
  build(&b, 2, 2, "memorybank@0", 0x1000, 0x1000);
  expect_eq(parse(&b, &base, &size), -1);
  build(&b, 2, 2, "mem", 0x1000, 0x1000);
  expect_eq(parse(&b, &base, &size), -1);
}

static void check_header_rejects(void)
{
  struct blob b;
  uint64_t base, size;
  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  expect_eq(fdt_memory(0, &base, &size), -1);
  expect_eq(fdt_memory(serialize(&b, b.st_len, 0xd00dfeee, 0), &base, &size), -1);

  uint8_t *p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, 0);
  p[11] = 41; /* struct offset not 4-aligned */
  expect_eq(fdt_memory(p, &base, &size), -1);

  p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, 0);
  p[7] = 8; /* total size smaller than the header */
  p[6] = p[5] = p[4] = 0;
  expect_eq(fdt_memory(p, &base, &size), -1);

  p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, 0);
  p[39] = 0xff; /* struct block claims to run past the blob */
  p[38] = 0xff;
  expect_eq(fdt_memory(p, &base, &size), -1);
}

/* Cutting the struct block at every byte must never read past the cut, and succeeds only once reg is complete. */
static void check_truncation(void)
{
  struct blob b;
  build(&b, 2, 2, "memory@40000000", 0x40000000, 0x10000000);
  for (uint32_t cut = 0; cut <= b.st_len; cut++) {
    uint64_t base = 0, size = 0;
    int rc = fdt_memory(serialize(&b, cut, 0xd00dfeed, 0), &base, &size);
    expect_eq(rc, cut >= b.reg_end ? 0 : -1);
  }
}

static void patch32(struct blob *b, uint32_t off, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    b->st[off + i] = (uint8_t)(v >> (8 * (3 - i)));
}

static void check_hostile_lengths(void)
{
  struct blob b;
  uint64_t base, size;

  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  patch32(&b, b.reg_end - 4 * 4 - 8 + 0, 0xffffffffu); /* reg length */
  expect_eq(parse(&b, &base, &size), -1);

  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  patch32(&b, b.reg_end - 4 * 4 - 8 + 4, 0x7fffffffu); /* name offset beyond the strings block */
  expect_eq(parse(&b, &base, &size), -1);

  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  patch32(&b, b.reg_end - 4 * 4 - 8, 8); /* reg shorter than address+size cells */
  expect_eq(parse(&b, &base, &size), -1);

  build(&b, 3, 2, "memory@0", 0x1000, 0x1000); /* unsupported cell count */
  expect_eq(parse(&b, &base, &size), -1);

  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  b.st[0] = 0;
  b.st[3] = 7; /* unknown token */
  expect_eq(parse(&b, &base, &size), -1);

  memset(&b, 0, sizeof b);
  put32(&b, 2); /* END_NODE with no open node */
  put32(&b, 9);
  expect_eq(parse(&b, &base, &size), -1);

  memset(&b, 0, sizeof b);
  put32(&b, 1);
  memset(b.st + b.st_len, 'a', 64); /* node name that never terminates */
  b.st_len += 64;
  expect_eq(parse(&b, &base, &size), -1);

  memset(&b, 0, sizeof b);
  begin(&b, "");
  end_node(&b);
  put32(&b, 9);
  expect_eq(parse(&b, &base, &size), -1); /* valid tree, no memory node */
}

int main(void)
{
  test("qemu layout", check_qemu_layout);
  test("address and size cell widths", check_cell_widths);
  test("only memory nodes at depth 2 count", check_node_names);
  test("invalid headers rejected", check_header_rejects);
  test("truncated struct block never read past", check_truncation);
  test("hostile lengths and tokens rejected", check_hostile_lengths);
  return 0;
}
