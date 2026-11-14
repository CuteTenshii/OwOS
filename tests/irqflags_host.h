/* irqflags_host.h: stands in for kernel/irqflags.h in host tests (force-included, see tests.mk). The real header
 * touches CPU interrupt state; here there is nothing to mask. */
#ifndef OWOS_IRQFLAGS_H
#define OWOS_IRQFLAGS_H

#include <stdint.h>

static inline uintptr_t irq_save(void) { return 0; }
static inline void irq_restore(uintptr_t f) { (void)f; }
static inline uintptr_t irq_flags(void) { return 0; }
static inline int irq_flags_on(uintptr_t f) { return f != 0; }
static inline void cpu_halt(void) {}
static inline void irq_enable(void) {}
static inline void irq_disable(void) {}
static inline void cpu_wait(void) {}
static inline void cpu_wait_enabled(void) {}
static inline void cpu_relax(void) {}

#endif
