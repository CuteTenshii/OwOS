/* irqflags.h: save/restore the interrupt flag (width-correct on i386 and x86_64), and the other
 * interrupt-state primitives the shared kernel needs, per arch.
 *
 * The kernel's pervasive "snapshot EFLAGS, then cli" idiom (so a routine can
 * run a critical section and restore the caller's IF — see CLAUDE.md on why IF
 * state must be preserved across cooperative yields). In long mode pushf/popf
 * move the 64-bit RFLAGS, so the destination must be register-width; uintptr_t
 * is correct on both arches. Centralised here so the idiom has one definition.
 */
#ifndef OWOS_IRQFLAGS_H
#define OWOS_IRQFLAGS_H

#include <stdint.h>

#ifdef __aarch64__
/* DAIF plays the role of EFLAGS: masked bits mean interrupts off. */
static inline uintptr_t irq_save(void)
{
  uintptr_t f;
  __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(f)::"memory");
  return f;
}

static inline void irq_restore(uintptr_t f) { __asm__ volatile("msr daif, %0" : : "r"(f) : "memory"); }

/* Whether a snapshot from irq_save/irq_flags had interrupts enabled. */
static inline int irq_flags_on(uintptr_t f) { return !(f & 0x80); }

static inline uintptr_t irq_flags(void)
{
  uintptr_t f;
  __asm__ volatile("mrs %0, daif" : "=r"(f)::"memory");
  return f;
}

/* Mask interrupts and wait; call in a loop to halt for good. */
static inline void cpu_halt(void) { __asm__ volatile("msr daifset, #2\n\twfi"); }

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
/* Wait for an interrupt, leaving the mask as it is (a masked one still ends the wait). */
static inline void cpu_wait(void) { __asm__ volatile("wfi" ::: "memory"); }
/* Wait for an interrupt and take it: wfi before unmasking, so one arriving in between still ends the wait. */
static inline void cpu_wait_enabled(void) { __asm__ volatile("wfi\n\tmsr daifclr, #2" ::: "memory"); }
/* Spin-wait hint for a busy loop. */
static inline void cpu_relax(void) { __asm__ volatile("yield" ::: "memory"); }
#elif defined(__arm__)
/* CPSR plays the role of EFLAGS: the I bit set means interrupts off. */
static inline uintptr_t irq_save(void)
{
  uintptr_t f;
  __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(f)::"memory");
  return f;
}

/* Writes only the control byte (mode, I, F, T), which irq_save captured in this same mode. */
static inline void irq_restore(uintptr_t f) { __asm__ volatile("msr cpsr_c, %0" : : "r"(f) : "memory"); }

static inline int irq_flags_on(uintptr_t f) { return !(f & 0x80); }

static inline uintptr_t irq_flags(void)
{
  uintptr_t f;
  __asm__ volatile("mrs %0, cpsr" : "=r"(f)::"memory");
  return f;
}

static inline void cpu_halt(void) { __asm__ volatile("cpsid i\n\twfi"); }

static inline void irq_enable(void) { __asm__ volatile("cpsie i" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cpsid i" ::: "memory"); }
static inline void cpu_wait(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void cpu_wait_enabled(void) { __asm__ volatile("wfi\n\tcpsie i" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("yield" ::: "memory"); }
#else
/* Snapshot the flags register and disable interrupts. Pair with irq_restore. */
static inline uintptr_t irq_save(void)
{
  uintptr_t f;
  __asm__ volatile("pushf\n\tpop %0\n\tcli" : "=r"(f)::"memory");
  return f;
}

/* Restore a flags snapshot taken by irq_save/irq_flags (re-enables IF if it was
 * set when saved). */
static inline void irq_restore(uintptr_t f) { __asm__ volatile("push %0\n\tpopf" : : "r"(f) : "memory", "cc"); }

/* Whether a snapshot from irq_save/irq_flags had interrupts enabled (IF). */
static inline int irq_flags_on(uintptr_t f) { return (f & 0x200) != 0; }

/* Read the flags register without altering the interrupt state. */
static inline uintptr_t irq_flags(void)
{
  uintptr_t f;
  __asm__ volatile("pushf\n\tpop %0" : "=r"(f)::"memory");
  return f;
}

static inline void cpu_halt(void) { __asm__ volatile("cli; hlt"); }

static inline void irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
/* Wait for an interrupt, leaving IF as it is. */
static inline void cpu_wait(void) { __asm__ volatile("hlt" ::: "memory"); }
/* Wait for an interrupt and take it: sti's one-instruction shadow keeps one from slipping in before the hlt. */
static inline void cpu_wait_enabled(void) { __asm__ volatile("sti\n\thlt" ::: "memory"); }
/* Spin-wait hint for a busy loop. */
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }
#endif

#endif /* OWOS_IRQFLAGS_H */
