/* memmap.h: carve reserved ranges out of RAM. */
#ifndef OWOS_ARCH_AARCH64_MEMMAP_H
#define OWOS_ARCH_AARCH64_MEMMAP_H

#include "fdt.h"

/* Writes the parts of [base, base + size) not covered by any of the n reservations to out, in address order.
 * Returns the piece count, or -1 if more than max pieces would be needed. */
int memmap_subtract(uint64_t base, uint64_t size, const struct fdt_range *rsv, int n, struct fdt_range *out, int max);

#endif
