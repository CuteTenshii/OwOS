#ifndef OWOS_SPINLOCK_H
#define OWOS_SPINLOCK_H

#include "irqflags.h"

typedef volatile uint32_t spinlock_t;

static inline uintptr_t spin_lock_irqsave(spinlock_t *lock)
{
  uintptr_t flags = irq_save();
  while (__sync_lock_test_and_set(lock, 1))
#ifdef __aarch64__
    __asm__ volatile("yield");
#else
    __asm__ volatile("pause");
#endif
  return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uintptr_t flags)
{
  __sync_lock_release(lock);
  irq_restore(flags);
}

#endif
