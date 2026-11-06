#include "mutex.h"

extern unsigned msec(void);

static inline unsigned cas(unsigned *p, unsigned expect, unsigned want)
{
  __atomic_compare_exchange_n(p, &expect, want, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  return expect;
}

bool mutex_trylock(mutex_t *m) { return cas(&m->state, 0, 1) == 0; }

void mutex_lock(mutex_t *m)
{
  unsigned c = cas(&m->state, 0, 1);
  if (c == 0)
    return;
  do {
    if (c == 2 || cas(&m->state, 1, 2) != 0)
      futex_wait(&m->state, 2, 0);
  } while ((c = cas(&m->state, 0, 2)) != 0);
}

bool mutex_timedlock(mutex_t *m, unsigned ms)
{
  unsigned c = cas(&m->state, 0, 1);
  if (c == 0)
    return true;
  unsigned start = msec();
  do {
    unsigned used = msec() - start;
    if (used >= ms)
      return false;
    if (c == 2 || cas(&m->state, 1, 2) != 0)
      futex_wait(&m->state, 2, ms - used);
  } while ((c = cas(&m->state, 0, 2)) != 0);
  return true;
}

void mutex_unlock(mutex_t *m)
{
  if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
    futex_wake(&m->state, 1);
}

void mutex_reset(mutex_t *m)
{
  __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
  futex_wake(&m->state, 0x7fffffff);
}
