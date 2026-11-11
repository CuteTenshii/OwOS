/* counter.h: the architected system counter, for timeouts that must work before the scheduler tick runs. */
#ifndef OWOS_ARCH_AARCH64_COUNTER_H
#define OWOS_ARCH_AARCH64_COUNTER_H

#include <stdint.h>

static inline uint64_t counter_now(void)
{
  uint64_t v;
  __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) : : "memory");
  return v;
}

static inline uint64_t counter_hz(void)
{
  uint64_t v;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}

/* Counter value `us` microseconds from now. */
static inline uint64_t counter_deadline_us(uint64_t us) { return counter_now() + us * counter_hz() / 1000000; }

static inline int counter_passed(uint64_t deadline) { return (int64_t)(counter_now() - deadline) >= 0; }

static inline void counter_delay_us(uint64_t us)
{
  uint64_t end = counter_deadline_us(us);
  while (!counter_passed(end))
    ;
}

#endif
