/* fdt.h: flattened device tree reader. */
#ifndef OWOS_ARCH_AARCH64_FDT_H
#define OWOS_ARCH_AARCH64_FDT_H

#include <stdint.h>

/* Base and size of the first /memory node. Returns 0 on success, -1 if the blob is invalid or has no memory. */
int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size);

#endif
