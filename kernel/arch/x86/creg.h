/* creg.h: control-register accessors, width-correct on i386 and x86_64.
 *
 * In long mode a `mov` to/from a control register is 64-bit only, so a 32-bit C
 * operand fails to assemble ("operand type mismatch"). uintptr_t is 32-bit on
 * i386 and 64-bit on x86_64, so these touch each register at its native width
 * on both. Pointer operands (e.g. a uint32_t* page directory) are already
 * register-width and need no helper (see paging.h's
 * paging_load/paging_current). */
#ifndef OWOS_CREG_H
#define OWOS_CREG_H

#include <stdint.h>

static inline uintptr_t read_cr0(void)
{
  uintptr_t v;
  __asm__ volatile("mov %%cr0, %0" : "=r"(v));
  return v;
}
static inline void write_cr0(uintptr_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uintptr_t read_cr2(void)
{
  uintptr_t v;
  __asm__ volatile("mov %%cr2, %0" : "=r"(v));
  return v;
}
static inline uintptr_t read_cr3(void)
{
  uintptr_t v;
  __asm__ volatile("mov %%cr3, %0" : "=r"(v));
  return v;
}
static inline void write_cr3(uintptr_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline uintptr_t read_cr4(void)
{
  uintptr_t v;
  __asm__ volatile("mov %%cr4, %0" : "=r"(v));
  return v;
}
static inline void write_cr4(uintptr_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

#endif /* OWOS_CREG_H */
