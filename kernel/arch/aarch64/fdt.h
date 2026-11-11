/* fdt.h: flattened device tree reader. */
#ifndef OWOS_ARCH_AARCH64_FDT_H
#define OWOS_ARCH_AARCH64_FDT_H

#include <stdint.h>

/* Base and size of the first /memory node. Returns 0 on success, -1 if the blob is invalid or has no memory. */
int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size);

struct fdt_range {
  uint64_t base, size;
};

/* Ranges the loader marks off-limits: the memory reservation block plus every /reserved-memory child with a reg.
 * Fills at most max entries and returns the count, or -1 if the blob is invalid. */
int fdt_reserved(const void *fdt, struct fdt_range *out, int max);

/* Size of the blob in bytes, 0 if it is not a device tree. */
uint32_t fdt_totalsize(const void *fdt);

/* The idx-th node whose compatible list contains compat: reg entry `entry`, decoded with the parent's cell widths. */
int fdt_compat_reg(const void *fdt, const char *compat, int idx, int entry, uint64_t *base, uint64_t *size);

/* String property `prop` of the first node matching compat, copied with its NUL into buf. */
int fdt_compat_str(const void *fdt, const char *compat, const char *prop, char *buf, uint32_t cap);

/* First cell of integer property `prop` of the first node matching compat. */
int fdt_compat_u32(const void *fdt, const char *compat, const char *prop, uint32_t *val);

/* The same two lookups on the idx-th matching node, counted as fdt_compat_reg counts them. */
int fdt_compat_str_at(const void *fdt, const char *compat, int idx, const char *prop, char *buf, uint32_t cap);
int fdt_compat_u32_at(const void *fdt, const char *compat, int idx, const char *prop, uint32_t *val);

/* MPIDR affinity bits of the idx-th /cpus/cpu node, in tree order. */
#define FDT_MPIDR_AFFINITY 0xff00ffffffULL
int fdt_cpu_mpidr(const void *fdt, int idx, uint64_t *mpidr);

#endif
