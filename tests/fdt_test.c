/* fdt_test.c: the device tree reader shared by the ARM ports (kernel/arch/common/fdt.c) against hand-built blobs,
 * including truncated and hostile ones. Blobs sit right before a PROT_NONE page so any read past the end faults. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/common/fdt.h"
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
  uint32_t reg_end;   /* struct offset just past the memory node's reg property */
  uint64_t rsv[8][2]; /* memory reservation block entries */
  int nrsv;
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
  uint32_t rsv_len = 16 * (uint32_t)(b->nrsv + 1);
  uint32_t struct_off = 40 + rsv_len, strs_off = struct_off + struct_len;
  uint32_t total = strs_off + b->strs_len;
  if (!guard_page) {
    guard_page = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    mprotect(guard_page + 4096, 4096, PROT_NONE);
  }
  uint8_t *p = guard_page + 4096 - total;
  memset(guard_page, 0, 4096);
  uint32_t hdr[10] = {magic, total, struct_off, strs_off, 40, 17, 16, 0, b->strs_len, struct_len};
  for (int i = 0; i < 10; i++)
    for (int k = 0; k < 4; k++)
      p[i * 4 + k] = (uint8_t)(hdr[i] >> (8 * (3 - k)));
  memset(p + 40, 0, rsv_len);
  for (int i = 0; i < b->nrsv; i++)
    for (int k = 0; k < 8; k++) {
      p[40 + 16 * i + k] = (uint8_t)(b->rsv[i][0] >> (8 * (7 - k)));
      p[48 + 16 * i + k] = (uint8_t)(b->rsv[i][1] >> (8 * (7 - k)));
    }
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

static void prop_str(struct blob *b, const char *name, const char *bytes, uint32_t len)
{
  put32(b, 3);
  put32(b, len);
  put32(b, name_off(b, name));
  memcpy(b->st + b->st_len, bytes, len);
  b->st_len += len;
  pad(b);
}

static void prop64(struct blob *b, const char *name, uint64_t addr, uint64_t size)
{
  uint32_t c[4] = {(uint32_t)(addr >> 32), (uint32_t)addr, (uint32_t)(size >> 32), (uint32_t)size};
  prop(b, name, c, 4);
}

/* root { memory, reserved-memory { atf, dynamic (size only), lk }, soc { reserved-memory decoy } } plus memreserve
 * entries */
static void build_reserved(struct blob *b)
{
  memset(b, 0, sizeof *b);
  b->rsv[b->nrsv][0] = 0x48000000, b->rsv[b->nrsv++][1] = 0x10000;
  begin(b, "");
  prop1(b, "#address-cells", 2);
  prop1(b, "#size-cells", 2);
  begin(b, "reserved-memory");
  prop1(b, "#address-cells", 2);
  prop1(b, "#size-cells", 2);
  begin(b, "atf@43000000");
  prop64(b, "reg", 0x43000000, 0x30000);
  end_node(b);
  begin(b, "dynamic");
  prop1(b, "size", 0x1000000);
  end_node(b);
  begin(b, "lk@46000000");
  uint32_t two[8] = {0, 0x46000000, 0, 0x200000, 0, 0x47000000, 0, 0x1000};
  prop(b, "reg", two, 8);
  end_node(b);
  end_node(b);
  begin(b, "soc");
  begin(b, "reserved-memory");
  begin(b, "decoy@0");
  prop64(b, "reg", 0, 0x1000);
  end_node(b);
  end_node(b);
  end_node(b);
  end_node(b);
  put32(b, 9);
}

static void check_reserved(void)
{
  struct blob b;
  struct fdt_range r[16];
  build_reserved(&b);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, 0);
  expect_eq(fdt_reserved(fdt, r, 16), 4);
  expect_eq(r[0].base, 0x48000000);
  expect_eq(r[0].size, 0x10000);
  expect_eq(r[1].base, 0x43000000);
  expect_eq(r[1].size, 0x30000);
  expect_eq(r[2].base, 0x46000000);
  expect_eq(r[2].size, 0x200000);
  expect_eq(r[3].base, 0x47000000);
  expect_eq(r[3].size, 0x1000);

  expect_eq(fdt_reserved(fdt, r, 2), 2); /* output is capped, not overrun */
}

static void check_reserved_cells(void)
{
  struct blob b;
  struct fdt_range r[4];
  memset(&b, 0, sizeof b);
  begin(&b, "");
  prop1(&b, "#address-cells", 2);
  prop1(&b, "#size-cells", 2);
  begin(&b, "reserved-memory");
  prop1(&b, "#address-cells", 1);
  prop1(&b, "#size-cells", 1);
  begin(&b, "fw@80000000");
  uint32_t reg[2] = {0x80000000u, 0x100000};
  prop(&b, "reg", reg, 2);
  end_node(&b);
  end_node(&b);
  end_node(&b);
  put32(&b, 9);
  expect_eq(fdt_reserved(serialize(&b, b.st_len, 0xd00dfeed, 0), r, 4), 1);
  expect_eq(r[0].base, 0x80000000u);
  expect_eq(r[0].size, 0x100000);
}

static void check_reserved_none_and_invalid(void)
{
  struct blob b;
  struct fdt_range r[4];
  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  expect_eq(fdt_reserved(serialize(&b, b.st_len, 0xd00dfeed, 0), r, 4), 0);
  expect_eq(fdt_reserved(0, r, 4), -1);

  build_reserved(&b);
  uint8_t *p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, 0);
  p[19] = 44; /* reservation block offset not 8-aligned */
  expect_eq(fdt_reserved(p, r, 4), -1);

  p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, 0);
  p[19] = 0xf0; /* reservation block starts past the blob */
  expect_eq(fdt_reserved(p, r, 4), -1);

  uint32_t total;
  build_reserved(&b);
  p = (uint8_t *)serialize(&b, b.st_len, 0xd00dfeed, &total);
  p[19] = (uint8_t)(total - 8); /* block runs off the end before its terminator */
  expect_eq(fdt_reserved(p, r, 4), -1);
}

/* A phone-shaped tree: cluster-split cpus, a soc node with its own cell widths, a GICv3 and a UART whose reg follows
 * compatible. */
static void build_soc(struct blob *b)
{
  memset(b, 0, sizeof *b);
  begin(b, "");
  prop1(b, "#address-cells", 2);
  prop1(b, "#size-cells", 2);
  begin(b, "cpus");
  prop1(b, "#address-cells", 1);
  prop1(b, "#size-cells", 0);
  static const uint32_t mpidr[] = {0x000, 0x001, 0x100, 0x101};
  static const char *const names[] = {"cpu@0", "cpu@1", "cpu@100", "cpu@101"};
  for (int i = 0; i < 4; i++) {
    begin(b, names[i]);
    prop_str(b, "device_type", "cpu", 4);
    prop1(b, "reg", mpidr[i]);
    end_node(b);
  }
  begin(b, "cpu-map");
  begin(b, "cluster0");
  prop1(b, "reg", 0xdead);
  end_node(b);
  end_node(b);
  end_node(b);
  begin(b, "psci");
  prop_str(b, "compatible", "arm,psci-1.0\0arm,psci-0.2", 27);
  prop_str(b, "method", "smc", 4);
  end_node(b);
  begin(b, "soc");
  prop1(b, "#address-cells", 2);
  prop1(b, "#size-cells", 2);
  begin(b, "interrupt-controller@c000000");
  prop64(b, "reg", 0x0c000000, 0x40000);
  prop_str(b, "compatible", "arm,gic-v3", 11);
  end_node(b);
  begin(b, "serial@11002000");
  prop_str(b, "compatible", "mediatek,mt6765-uart\0mediatek,mt6577-uart", 42);
  prop64(b, "reg", 0x11002000, 0x400);
  prop_str(b, "status", "okay", 5);
  end_node(b);
  begin(b, "serial@11003000");
  prop_str(b, "compatible", "mediatek,mt6577-uart", 21);
  prop64(b, "reg", 0x11003000, 0x400);
  end_node(b);
  static const uint32_t i2c0_reg[] = {0, 0x11007000, 0, 0x1000, 0, 0x11000080, 0, 0x80};
  begin(b, "i2c0@11007000");
  prop_str(b, "compatible", "mediatek,i2c", 13);
  prop1(b, "id", 3);
  prop(b, "reg", i2c0_reg, 8);
  prop1(b, "clock-div", 5);
  end_node(b);
  begin(b, "i2c1@11008000");
  prop_str(b, "compatible", "mediatek,i2c", 13);
  prop1(b, "clock-frequency", 400000);
  prop_str(b, "status", "disabled", 9);
  end_node(b);
  end_node(b);
  end_node(b);
  put32(b, 9);
}

static void check_compat_reg(void)
{
  struct blob b;
  uint64_t base, size;
  build_soc(&b);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, 0);
  expect_eq(fdt_compat_reg(fdt, "arm,gic-v3", 0, 0, &base, &size), 0);
  expect_eq(base, 0x0c000000);
  expect_eq(size, 0x40000);
  expect_eq(fdt_compat_reg(fdt, "mediatek,mt6577-uart", 0, 0, &base, &size), 0);
  expect_eq(base, 0x11002000);
  expect_eq(fdt_compat_reg(fdt, "mediatek,mt6577-uart", 1, 0, &base, &size), 0);
  expect_eq(base, 0x11003000);
  expect_eq(fdt_compat_reg(fdt, "mediatek,mt6577-uart", 2, 0, &base, &size), -1);
  expect_eq(fdt_compat_reg(fdt, "mediatek,mt6765-uart", 0, 1, &base, &size), -1); /* only one reg entry */
  expect_eq(fdt_compat_reg(fdt, "arm,pl011", 0, 0, &base, &size), -1);
  expect_eq(fdt_compat_reg(fdt, "mediatek", 0, 0, &base, &size), -1); /* whole strings only */
  expect_eq(fdt_compat_reg(0, "arm,gic-v3", 0, 0, &base, &size), -1);
}

static void check_compat_str(void)
{
  struct blob b;
  char buf[8];
  build_soc(&b);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, 0);
  expect_eq(fdt_compat_str(fdt, "arm,psci-0.2", "method", buf, sizeof buf), 0);
  expect_eq(strcmp(buf, "smc"), 0);
  expect_eq(fdt_compat_str(fdt, "arm,psci-0.2", "nothing", buf, sizeof buf), -1);
  expect_eq(fdt_compat_str(fdt, "arm,psci-0.2", "method", buf, 3), -1); /* does not fit with its NUL */
  expect_eq(fdt_compat_str(fdt, "arm,pl011", "method", buf, sizeof buf), -1);
}

static void check_cpu_mpidr(void)
{
  struct blob b;
  uint64_t m;
  build_soc(&b);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, 0);
  static const uint64_t want[] = {0x000, 0x001, 0x100, 0x101};
  for (int i = 0; i < 4; i++) {
    expect_eq(fdt_cpu_mpidr(fdt, i, &m), 0);
    expect_eq(m, want[i]);
  }
  expect_eq(fdt_cpu_mpidr(fdt, 4, &m), -1); /* cpu-map entries are not cpus */
  expect_eq(fdt_cpu_mpidr(fdt, -1, &m), -1);

  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  expect_eq(fdt_cpu_mpidr(serialize(&b, b.st_len, 0xd00dfeed, 0), 0, &m), -1);
}

static void check_compat_indexed(void)
{
  struct blob b;
  uint64_t base, size;
  uint32_t v = 0;
  char buf[16];
  build_soc(&b);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, 0);
  expect_eq(fdt_compat_reg(fdt, "mediatek,i2c", 0, 1, &base, &size), 0); /* second reg entry: the DMA channel */
  expect_eq(base, 0x11000080);
  expect_eq(size, 0x80);
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", 0, "id", &v), 0);
  expect_eq(v, 3);
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", 0, "clock-div", &v), 0);
  expect_eq(v, 5);
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", 1, "clock-frequency", &v), 0);
  expect_eq(v, 400000);
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", 0, "clock-frequency", &v), -1); /* not the next node's */
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", 2, "id", &v), -1);
  expect_eq(fdt_compat_u32_at(fdt, "mediatek,i2c", -1, "id", &v), -1);
  expect_eq(fdt_compat_str_at(fdt, "mediatek,i2c", 1, "status", buf, sizeof buf), 0);
  expect_eq(strcmp(buf, "disabled"), 0);
  expect_eq(fdt_compat_str_at(fdt, "mediatek,i2c", 0, "status", buf, sizeof buf), -1);
  expect_eq(fdt_compat_u32(fdt, "mediatek,i2c", "clock-div", &v), 0); /* the unindexed form is node 0 */
  expect_eq(v, 5);
}

static void check_find_truncation(void)
{
  struct blob b;
  build_soc(&b);
  for (uint32_t cut = 0; cut < b.st_len; cut += 4) {
    uint64_t base, size, m;
    char buf[8];
    const uint8_t *fdt = serialize(&b, cut, 0xd00dfeed, 0);
    (void)fdt_compat_reg(fdt, "mediatek,mt6577-uart", 1, 0, &base, &size);
    (void)fdt_compat_str(fdt, "arm,psci-1.0", "method", buf, sizeof buf);
    (void)fdt_compat_str_at(fdt, "mediatek,i2c", 1, "status", buf, sizeof buf);
    (void)fdt_compat_u32_at(fdt, "mediatek,i2c", 1, "clock-frequency", (uint32_t[1]){0});
    (void)fdt_cpu_mpidr(fdt, 3, &m);
    (void)fdt_reserved(fdt, (struct fdt_range[4]){0}, 4);
  }
}

static void check_totalsize(void)
{
  struct blob b;
  uint32_t total;
  build(&b, 2, 2, "memory@0", 0x1000, 0x1000);
  const uint8_t *fdt = serialize(&b, b.st_len, 0xd00dfeed, &total);
  expect_eq(fdt_totalsize(fdt), total);
  expect_eq(fdt_totalsize(0), 0);
  expect_eq(fdt_totalsize(serialize(&b, b.st_len, 0xd00dfeee, 0)), 0);
}

int main(void)
{
  test("qemu layout", check_qemu_layout);
  test("address and size cell widths", check_cell_widths);
  test("only memory nodes at depth 2 count", check_node_names);
  test("invalid headers rejected", check_header_rejects);
  test("truncated struct block never read past", check_truncation);
  test("hostile lengths and tokens rejected", check_hostile_lengths);
  test("reserved ranges from memreserve and reserved-memory", check_reserved);
  test("reserved-memory own cell widths", check_reserved_cells);
  test("reserved ranges: none and invalid blobs", check_reserved_none_and_invalid);
  test("compatible lookup and reg decoding", check_compat_reg);
  test("compatible lookup of string properties", check_compat_str);
  test("indexed property lookups on repeated nodes", check_compat_indexed);
  test("cpu mpidr list", check_cpu_mpidr);
  test("lookups on truncated blobs stay in bounds", check_find_truncation);
  test("total size", check_totalsize);
  return 0;
}
