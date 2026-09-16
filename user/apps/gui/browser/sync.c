#include "browser.h"

static volatile int blk_lk;

static inline int blk_xchg(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}

void browser_buf_lock(void)
{
  while (blk_xchg(&blk_lk, 1))
    msleep(0);
}

void browser_buf_unlock(void)
{
  __asm__ volatile("" ::: "memory");
  blk_lk = 0;
}
