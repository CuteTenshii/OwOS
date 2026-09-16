/* irqflags.h — save/restore the interrupt flag, width-correct on i386 and
 * x86_64.
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

/* Read the flags register without altering the interrupt state. */
static inline uintptr_t irq_flags(void)
{
  uintptr_t f;
  __asm__ volatile("pushf\n\tpop %0" : "=r"(f)::"memory");
  return f;
}

#endif /* OWOS_IRQFLAGS_H */
