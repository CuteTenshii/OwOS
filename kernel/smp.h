/* smp.h — CPU topology from the ACPI MADT. Discovery only: brings no core
 * online and leaves the scheduler untouched. */
#ifndef OWOS_SMP_H
#define OWOS_SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 32

/* IDT vector for the per-core Local APIC timer (the AP scheduler tick). Above
 * the remapped PIC range (32..47), distinct from every device IRQ. */
#define LAPIC_TIMER_VEC 0x40

/* No-op when ACPI is unreachable (count then falls back to 1). */
void smp_init(void);

/* Wake the discovered APs via INIT-SIPI-SIPI; they park (halt) for now. Needs
 * the timer running (uses busy delays) — call after sti. No-op on a single CPU.
 */
void smp_start_aps(void);

int smp_cpu_count(void);    /* logical CPUs reported (>= 1)        */
int smp_online_count(void); /* cores actually running (>= 1)       */
int smp_this_cpu(void);     /* index of the calling CPU into 0..count-1 */

/* The Local APIC timer interrupt handler (called from irq_handler on vector
 * LAPIC_TIMER_VEC). Runs on an AP; acks its own LAPIC. */
struct registers;
void smp_lapic_tick(struct registers *r);
uint8_t smp_apic_id(int i);     /* i-th CPU's Local APIC ID            */
uint32_t smp_lapic_base(void);  /* Local APIC MMIO base               */
uint32_t smp_ioapic_base(void); /* first IO APIC MMIO base, 0 if none */

#endif /* OWOS_SMP_H */
