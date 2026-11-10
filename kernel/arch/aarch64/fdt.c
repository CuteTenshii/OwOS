/* fdt.c: minimal flattened device tree reader, enough to find system RAM. Every read is bounds-checked. */
#include "fdt.h"
#include <stdbool.h>
#include <stddef.h>

#define FDT_MAGIC 0xd00dfeedU
#define FDT_HEADER_SIZE 40
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

static uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t read_cells(const uint8_t *p, uint32_t cells)
{
  uint64_t v = 0;
  for (uint32_t i = 0; i < cells; i++)
    v = v << 32 | be32(p + 4 * i);
  return v;
}

static bool str_eq(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static bool is_memory_node(const char *name)
{
  static const char want[] = "memory";
  for (size_t i = 0; i < sizeof want - 1; i++)
    if (name[i] != want[i])
      return false;
  return name[6] == '\0' || name[6] == '@';
}

/* Length of the NUL-terminated string at p, or -1 if it does not end before `end`. */
static long str_len(const uint8_t *p, const uint8_t *end)
{
  for (const uint8_t *q = p; q < end; q++)
    if (!*q)
      return q - p;
  return -1;
}

static bool block_ok(uint64_t total, uint32_t off, uint32_t size) { return (uint64_t)off + size <= total; }

int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size)
{
  const uint8_t *hdr = fdt;
  if (!fdt || be32(hdr) != FDT_MAGIC)
    return -1;

  uint64_t total = be32(hdr + 4);
  uint32_t struct_off = be32(hdr + 8), strings_off = be32(hdr + 12);
  uint32_t strings_size = be32(hdr + 32), struct_size = be32(hdr + 36);
  if (total < FDT_HEADER_SIZE || (struct_off & 3) || !block_ok(total, struct_off, struct_size) ||
      !block_ok(total, strings_off, strings_size))
    return -1;

  const uint8_t *p = hdr + struct_off, *end = p + struct_size;
  const uint8_t *strings = hdr + strings_off, *strings_end = strings + strings_size;
  uint32_t addr_cells = 2, size_cells = 1;
  int depth = 0;
  bool in_memory = false;

  while (end - p >= 4) {
    uint32_t tok = be32(p);
    p += 4;
    if (tok == FDT_BEGIN_NODE) {
      long n = str_len(p, end);
      if (n < 0)
        return -1;
      depth++;
      in_memory = depth == 2 && is_memory_node((const char *)p);
      uint64_t advance = ((uint64_t)n + 1 + 3) & ~3ULL;
      if (advance > (uint64_t)(end - p))
        return -1;
      p += advance;
    } else if (tok == FDT_END_NODE) {
      if (--depth < 0)
        return -1;
      in_memory = false;
    } else if (tok == FDT_PROP) {
      if (end - p < 8)
        return -1;
      uint32_t len = be32(p), name_off = be32(p + 4);
      const uint8_t *val = p + 8;
      uint64_t padded = ((uint64_t)len + 3) & ~3ULL;
      if (padded > (uint64_t)(end - val) || name_off >= strings_size || str_len(strings + name_off, strings_end) < 0)
        return -1;
      const char *pname = (const char *)strings + name_off;
      p = val + padded;

      if (depth == 1 && str_eq(pname, "#address-cells") && len >= 4)
        addr_cells = be32(val);
      else if (depth == 1 && str_eq(pname, "#size-cells") && len >= 4)
        size_cells = be32(val);
      else if (in_memory && str_eq(pname, "reg")) {
        if (addr_cells == 0 || addr_cells > 2 || size_cells == 0 || size_cells > 2 ||
            len < 4 * (addr_cells + size_cells))
          return -1;
        *base = read_cells(val, addr_cells);
        *size = read_cells(val + 4 * addr_cells, size_cells);
        return 0;
      }
    } else if (tok != FDT_NOP) {
      return -1;
    }
  }
  return -1;
}
