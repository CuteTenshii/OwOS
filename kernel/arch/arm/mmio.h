/* mmio.h: raw device register access. */
#ifndef OWOS_ARCH_ARM_MMIO_H
#define OWOS_ARCH_ARM_MMIO_H

#include <stdint.h>

static inline uint32_t mmio_read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void mmio_write32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#endif
