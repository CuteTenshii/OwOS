/* a64_host.h: force-included when kernel/arch/aarch64/elf.c is built for host tests. Frames are slices of a
 * test-owned arena instead of identity-mapped physical memory. */
#ifndef OWOS_A64_HOST_H
#define OWOS_A64_HOST_H

#include <stdint.h>
#include <string.h>

/* The host libc supplies the mem* functions kernel/kstring.h would declare with kernel-sized types. */
#define OWOS_KSTRING_H

void *a64_host_ptr(uint64_t phys);
#define PHYS_TO_PTR(phys) a64_host_ptr(phys)

#endif
