/* fdt.c: minimal flattened device tree reader, enough to find system RAM. */
#include "arch.h"
#include "kstring.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static uint32_t be32(const void *p)
{
  const uint8_t *b = p;
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t read_cells(const uint8_t *p, uint32_t cells)
{
  uint64_t v = 0;
  for (uint32_t i = 0; i < cells; i++)
    v = v << 32 | be32(p + 4 * i);
  return v;
}

static bool is_memory_node(const char *name)
{
  return memcmp(name, "memory", 6) == 0 && (name[6] == '\0' || name[6] == '@');
}

int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size)
{
  const uint8_t *hdr = fdt;
  if (!fdt || be32(hdr) != FDT_MAGIC)
    return -1;

  const uint8_t *p = hdr + be32(hdr + 8);
  const char *strings = (const char *)hdr + be32(hdr + 12);
  uint32_t addr_cells = 2, size_cells = 1;
  int depth = 0;
  bool in_memory = false;

  for (;;) {
    uint32_t tok = be32(p);
    p += 4;
    if (tok == FDT_BEGIN_NODE) {
      const char *name = (const char *)p;
      depth++;
      in_memory = depth == 2 && is_memory_node(name);
      p += (strlen(name) + 1 + 3) & ~3u;
    } else if (tok == FDT_END_NODE) {
      depth--;
      in_memory = false;
    } else if (tok == FDT_PROP) {
      uint32_t len = be32(p);
      const char *pname = strings + be32(p + 4);
      const uint8_t *val = p + 8;
      p += 8 + ((len + 3) & ~3u);
      if (depth == 1 && streq(pname, "#address-cells"))
        addr_cells = be32(val);
      else if (depth == 1 && streq(pname, "#size-cells"))
        size_cells = be32(val);
      else if (in_memory && streq(pname, "reg") && len >= 4 * (addr_cells + size_cells)) {
        *base = read_cells(val, addr_cells);
        *size = read_cells(val + 4 * addr_cells, size_cells);
        return 0;
      }
    } else if (tok != FDT_NOP) {
      return -1;
    }
    if (tok == FDT_END || depth < 0)
      return -1;
  }
}
