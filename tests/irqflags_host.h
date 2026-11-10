/* irqflags_host.h: stands in for kernel/irqflags.h in host tests (force-included, see tests.mk). The real header
 * touches CPU interrupt state; here there is nothing to mask. */
#ifndef OWOS_IRQFLAGS_H
#define OWOS_IRQFLAGS_H

#include <stdint.h>

static inline uintptr_t irq_save(void) { return 0; }
static inline void irq_restore(uintptr_t f) { (void)f; }
static inline uintptr_t irq_flags(void) { return 0; }
static inline void cpu_halt(void) {}

#endif
