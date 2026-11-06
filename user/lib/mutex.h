/* mutex.h: futex-backed mutex. A contended locker sleeps in the kernel instead of spinning, and
 * unlock only enters the kernel when someone is waiting */
#ifndef OWOS_MUTEX_H
#define OWOS_MUTEX_H

#include <stdbool.h>

typedef struct {
  unsigned state; /* 0 free, 1 locked, 2 locked with sleepers */
} mutex_t;

#define MUTEX_INIT {0}

void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
bool mutex_timedlock(mutex_t *m, unsigned ms); /* false on timeout */
void mutex_unlock(mutex_t *m);
void mutex_reset(mutex_t *m); /* force-unlock after a crash longjmp, waking sleepers */

/* Kernel futex: wait returns true on timeout/interrupt, false if woken or *addr != expected */
bool futex_wait(unsigned *addr, unsigned expected, unsigned ms);
int futex_wake(unsigned *addr, int n);

#endif
