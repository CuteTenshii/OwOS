/* Host stand-in for kernel/spinlock.h: no interrupts to mask, just the lock. */
#ifndef UVC_HOST_SPINLOCK_H
#define UVC_HOST_SPINLOCK_H

#include <stdint.h>

typedef volatile uint32_t spinlock_t;

static inline uintptr_t spin_lock_irqsave(spinlock_t *lock)
{
  while (__sync_lock_test_and_set(lock, 1))
    ;
  return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uintptr_t flags)
{
  (void)flags;
  __sync_lock_release(lock);
}

#endif
