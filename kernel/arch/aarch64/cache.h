/* cache.h: data cache maintenance for buffers a device reads or writes by DMA. */
#ifndef OWOS_ARCH_AARCH64_CACHE_H
#define OWOS_ARCH_AARCH64_CACHE_H

#include <stddef.h>
#include <stdint.h>

static inline uintptr_t dcache_line(void)
{
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  return (uintptr_t)4 << ((ctr >> 16) & 0xf);
}

/* Write dirty lines covering [p, p+len) back to memory so a device reading it sees the CPU's data. */
static inline void dcache_clean(const void *p, size_t len)
{
  uintptr_t line = dcache_line();
  for (uintptr_t a = (uintptr_t)p & ~(line - 1); a < (uintptr_t)p + len; a += line)
    __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb sy" : : : "memory");
}

/* Drop lines covering [p, p+len) so the CPU rereads what a device wrote. Whole lines are discarded: the range must
 * not share a line with data the CPU still needs. */
static inline void dcache_inval(void *p, size_t len)
{
  uintptr_t line = dcache_line();
  for (uintptr_t a = (uintptr_t)p & ~(line - 1); a < (uintptr_t)p + len; a += line)
    __asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb sy" : : : "memory");
}

#endif
