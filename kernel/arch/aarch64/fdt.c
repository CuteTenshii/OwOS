/* fdt.c: minimal flattened device tree reader: system RAM and reserved ranges. Every read is bounds-checked. */
#include "fdt.h"
#include <stdbool.h>
#include <stddef.h>

#define FDT_MAGIC 0xd00dfeedU
#define FDT_HEADER_SIZE 40
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

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

/* Called for each node start (name set), node end (name and prop NULL) and property. Nonzero stops the walk with that
 * value. */
typedef int (*fdt_visit)(void *ctx, int depth, const char *name, const char *prop, const uint8_t *val, uint32_t len);

static int fdt_walk(const void *fdt, fdt_visit visit, void *ctx)
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
  int depth = 0;

  while (end - p >= 4) {
    uint32_t tok = be32(p);
    p += 4;
    if (tok == FDT_BEGIN_NODE) {
      long n = str_len(p, end);
      if (n < 0)
        return -1;
      depth++;
      int rc = visit(ctx, depth, (const char *)p, 0, 0, 0);
      if (rc)
        return rc;
      uint64_t advance = ((uint64_t)n + 1 + 3) & ~3ULL;
      if (advance > (uint64_t)(end - p))
        return -1;
      p += advance;
    } else if (tok == FDT_END_NODE) {
      if (--depth < 0)
        return -1;
      int rc = visit(ctx, depth + 1, 0, 0, 0, 0);
      if (rc)
        return rc;
    } else if (tok == FDT_PROP) {
      if (end - p < 8)
        return -1;
      uint32_t len = be32(p), name_off = be32(p + 4);
      const uint8_t *val = p + 8;
      uint64_t padded = ((uint64_t)len + 3) & ~3ULL;
      if (padded > (uint64_t)(end - val) || name_off >= strings_size || str_len(strings + name_off, strings_end) < 0)
        return -1;
      p = val + padded;
      int rc = visit(ctx, depth, 0, (const char *)strings + name_off, val, len);
      if (rc)
        return rc;
    } else if (tok == FDT_END) {
      return 0;
    } else if (tok != FDT_NOP) {
      return -1;
    }
  }
  return -1;
}

struct cells {
  uint32_t addr, size;
};

static bool cells_valid(const struct cells *c) { return c->addr >= 1 && c->addr <= 2 && c->size >= 1 && c->size <= 2; }

struct mem_ctx {
  struct cells root;
  bool in_memory;
  uint64_t *base, *size;
};

static int mem_visit(void *ctx, int depth, const char *name, const char *prop, const uint8_t *val, uint32_t len)
{
  struct mem_ctx *m = ctx;
  if (name) {
    m->in_memory = depth == 2 && is_memory_node(name);
  } else if (!prop) {
    m->in_memory = false;
  } else if (depth == 1 && str_eq(prop, "#address-cells") && len >= 4) {
    m->root.addr = be32(val);
  } else if (depth == 1 && str_eq(prop, "#size-cells") && len >= 4) {
    m->root.size = be32(val);
  } else if (m->in_memory && str_eq(prop, "reg")) {
    if (!cells_valid(&m->root) || len < 4 * (m->root.addr + m->root.size))
      return -1;
    *m->base = read_cells(val, m->root.addr);
    *m->size = read_cells(val + 4 * m->root.addr, m->root.size);
    return 1;
  }
  return 0;
}

int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size)
{
  struct mem_ctx m = {.root = {2, 1}, .base = base, .size = size};
  return fdt_walk(fdt, mem_visit, &m) == 1 ? 0 : -1;
}

struct rsv_ctx {
  struct cells root, rm;
  bool in_rm;
  struct fdt_range *out;
  int max, n;
};

static void rsv_add(struct rsv_ctx *r, uint64_t base, uint64_t size)
{
  if (size && r->n < r->max)
    r->out[r->n++] = (struct fdt_range){base, size};
}

static int rsv_visit(void *ctx, int depth, const char *name, const char *prop, const uint8_t *val, uint32_t len)
{
  struct rsv_ctx *r = ctx;
  if (name) {
    if (depth == 2) {
      r->in_rm = str_eq(name, "reserved-memory");
      r->rm = r->root;
    }
  } else if (!prop) {
    if (depth == 2)
      r->in_rm = false;
  } else if (depth == 1 && str_eq(prop, "#address-cells") && len >= 4) {
    r->root.addr = be32(val);
  } else if (depth == 1 && str_eq(prop, "#size-cells") && len >= 4) {
    r->root.size = be32(val);
  } else if (r->in_rm && depth == 2 && str_eq(prop, "#address-cells") && len >= 4) {
    r->rm.addr = be32(val);
  } else if (r->in_rm && depth == 2 && str_eq(prop, "#size-cells") && len >= 4) {
    r->rm.size = be32(val);
  } else if (r->in_rm && depth == 3 && str_eq(prop, "reg")) {
    if (!cells_valid(&r->rm))
      return -1;
    uint32_t entry = 4 * (r->rm.addr + r->rm.size);
    for (uint32_t off = 0; len - off >= entry; off += entry)
      rsv_add(r, read_cells(val + off, r->rm.addr), read_cells(val + off + 4 * r->rm.addr, r->rm.size));
  }
  return 0;
}

int fdt_reserved(const void *fdt, struct fdt_range *out, int max)
{
  const uint8_t *hdr = fdt;
  if (!fdt || be32(hdr) != FDT_MAGIC)
    return -1;
  uint64_t total = be32(hdr + 4);
  uint32_t rsv_off = be32(hdr + 16);
  struct rsv_ctx r = {.root = {2, 1}, .out = out, .max = max};

  /* The memory reservation block holds big-endian address/size pairs ending with an all-zero pair. */
  if (total < FDT_HEADER_SIZE || (rsv_off & 7) || rsv_off > total)
    return -1;
  for (uint32_t off = rsv_off;; off += 16) {
    if (!block_ok(total, off, 16))
      return -1;
    uint64_t addr = read_cells(hdr + off, 2), size = read_cells(hdr + off + 8, 2);
    if (!addr && !size)
      break;
    rsv_add(&r, addr, size);
  }

  if (fdt_walk(fdt, rsv_visit, &r) < 0)
    return -1;
  return r.n;
}

uint32_t fdt_totalsize(const void *fdt)
{
  const uint8_t *hdr = fdt;
  return fdt && be32(hdr) == FDT_MAGIC ? be32(hdr + 4) : 0;
}

#define FDT_MAXDEPTH 16

struct found {
  const uint8_t *reg, *prop;
  uint32_t reg_len, prop_len;
  struct cells parent;
};

struct find_ctx {
  const char *compat, *prop;
  int want;
  struct found *out;
  struct cells cells[FDT_MAXDEPTH + 1]; /* widths the children of the node at that depth use */
  bool match[FDT_MAXDEPTH + 1];
  struct found node[FDT_MAXDEPTH + 1];
};

static bool compat_listed(const char *want, const uint8_t *val, uint32_t len)
{
  for (uint32_t i = 0; i < len;) {
    long n = str_len(val + i, val + len);
    if (n < 0)
      return false;
    if (str_eq(want, (const char *)val + i))
      return true;
    i += (uint32_t)n + 1;
  }
  return false;
}

static int find_visit(void *ctx, int depth, const char *name, const char *prop, const uint8_t *val, uint32_t len)
{
  struct find_ctx *f = ctx;
  if (depth > FDT_MAXDEPTH)
    return 0;
  struct found *n = &f->node[depth];
  if (name) {
    f->cells[depth] = (struct cells){2, 1};
    f->match[depth] = false;
    *n = (struct found){0};
    n->parent = depth > 1 ? f->cells[depth - 1] : f->cells[depth];
  } else if (prop) {
    if (str_eq(prop, "#address-cells") && len >= 4)
      f->cells[depth].addr = be32(val);
    else if (str_eq(prop, "#size-cells") && len >= 4)
      f->cells[depth].size = be32(val);
    else if (str_eq(prop, "compatible"))
      f->match[depth] = compat_listed(f->compat, val, len);
    else if (str_eq(prop, "reg")) {
      n->reg = val;
      n->reg_len = len;
    }
    if (f->prop && str_eq(prop, f->prop)) {
      n->prop = val;
      n->prop_len = len;
    }
  } else if (f->match[depth] && f->want-- == 0) {
    *f->out = *n;
    return 1;
  }
  return 0;
}

static bool find_node(const void *fdt, const char *compat, int idx, const char *prop, struct found *out)
{
  struct find_ctx f = {.compat = compat, .prop = prop, .want = idx, .out = out};
  return fdt_walk(fdt, find_visit, &f) == 1;
}

int fdt_compat_reg(const void *fdt, const char *compat, int idx, int entry, uint64_t *base, uint64_t *size)
{
  struct found n;
  if (idx < 0 || entry < 0 || !find_node(fdt, compat, idx, 0, &n) || !n.reg || !cells_valid(&n.parent))
    return -1;
  uint64_t stride = 4ULL * (n.parent.addr + n.parent.size);
  if ((uint64_t)(entry + 1) * stride > n.reg_len)
    return -1;
  const uint8_t *e = n.reg + (uint64_t)entry * stride;
  *base = read_cells(e, n.parent.addr);
  *size = read_cells(e + 4 * n.parent.addr, n.parent.size);
  return 0;
}

int fdt_compat_str(const void *fdt, const char *compat, const char *prop, char *buf, uint32_t cap)
{
  struct found n;
  if (!cap || !find_node(fdt, compat, 0, prop, &n) || !n.prop)
    return -1;
  long len = str_len(n.prop, n.prop + n.prop_len);
  if (len < 0 || (uint32_t)len >= cap)
    return -1;
  for (long i = 0; i <= len; i++)
    buf[i] = (char)n.prop[i];
  return 0;
}

int fdt_compat_u32(const void *fdt, const char *compat, const char *prop, uint32_t *val)
{
  struct found n;
  if (!find_node(fdt, compat, 0, prop, &n) || !n.prop || n.prop_len < 4)
    return -1;
  *val = be32(n.prop);
  return 0;
}

struct cpu_ctx {
  uint32_t addr_cells;
  bool in_cpus, is_cpu;
  int want, seen;
  uint64_t *mpidr;
};

static int cpu_visit(void *ctx, int depth, const char *name, const char *prop, const uint8_t *val, uint32_t len)
{
  struct cpu_ctx *c = ctx;
  if (name) {
    if (depth == 2) {
      c->in_cpus = str_eq(name, "cpus");
      c->addr_cells = 2;
    } else if (depth == 3) {
      c->is_cpu =
          c->in_cpus && (str_eq(name, "cpu") || (name[0] == 'c' && name[1] == 'p' && name[2] == 'u' && name[3] == '@'));
    }
  } else if (!prop) {
    if (depth == 2)
      c->in_cpus = false;
  } else if (c->in_cpus && depth == 2 && str_eq(prop, "#address-cells") && len >= 4) {
    c->addr_cells = be32(val);
  } else if (c->is_cpu && depth == 3 && str_eq(prop, "reg")) {
    if (c->addr_cells < 1 || c->addr_cells > 2 || len < 4 * c->addr_cells)
      return -1;
    if (c->seen++ == c->want) {
      *c->mpidr = read_cells(val, c->addr_cells) & FDT_MPIDR_AFFINITY;
      return 1;
    }
  }
  return 0;
}

int fdt_cpu_mpidr(const void *fdt, int idx, uint64_t *mpidr)
{
  struct cpu_ctx c = {.want = idx, .mpidr = mpidr};
  return idx >= 0 && fdt_walk(fdt, cpu_visit, &c) == 1 ? 0 : -1;
}
