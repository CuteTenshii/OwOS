/* smp.h: the CPU numbering the shared kernel uses on AArch64; the CPUs come up through PSCI (smp.c). */
#ifndef OWOS_SMP_H
#define OWOS_SMP_H

#include "cpu_defs.h"
#include <stdint.h>

#define SMP_MAX_CPUS MAX_CPUS

/* Index of the calling CPU, the boot CPU 0. */
int smp_this_cpu(void);

/* CPUs online. */
uint32_t smp_cpu_count(void);

/* TLB maintenance broadcasts in the inner shareable domain, so no CPU ever has a flush to run for another. */
static inline void smp_tlb_service(void) {}

#endif /* OWOS_SMP_H */
