/* wallclock.c: wall time = RTC at boot + monotonic PIT time + applied slew. */

#include "wallclock.h"
#include "rtc.h"
#include "slew.h"
#include "spinlock.h"
#include "syscall_path.h"
#include "timer.h"

#define US_PER_S 1000000ll
#define MIN_EPOCH 946684800ll  /* 2000-01-01, the CMOS 2-digit year base */
#define MAX_EPOCH 4102444800ll /* 2100-01-01 */
#define MAX_ADJ_SECS 0x7fffffffll

static spinlock_t lock;
static int64_t wall_us; /* wall time at last_mono */
static uint64_t last_mono;
static struct slew slew;

static void advance(void)
{
  uint64_t now = timer_us64();
  if (now <= last_mono)
    return;
  int64_t delta = (int64_t)(now - last_mono);
  wall_us += delta + slew_step(&slew, delta);
  last_mono = now;
}

static void split(int64_t us, struct timeval64 *tv)
{
  int64_t sec = us / US_PER_S, rem = us % US_PER_S;
  if (rem < 0) {
    rem += US_PER_S;
    sec--;
  }
  tv->tv_sec = sec;
  tv->tv_usec = (int)rem;
}

void wallclock_init(void)
{
  uintptr_t fl = spin_lock_irqsave(&lock);
  wall_us = (int64_t)rtc_epoch() * US_PER_S;
  last_mono = timer_us64();
  spin_unlock_irqrestore(&lock, fl);
}

void wallclock_get(struct timeval64 *tv)
{
  uintptr_t fl = spin_lock_irqsave(&lock);
  advance();
  split(wall_us, tv);
  spin_unlock_irqrestore(&lock, fl);
}

bool wallclock_set(const struct timeval64 *tv)
{
  if (tv->tv_sec < MIN_EPOCH || tv->tv_sec >= MAX_EPOCH || tv->tv_usec < 0 || tv->tv_usec >= US_PER_S)
    return false;
  uintptr_t fl = spin_lock_irqsave(&lock);
  wall_us = tv->tv_sec * US_PER_S + tv->tv_usec;
  last_mono = timer_us64();
  slew.remaining = 0;
  slew.carry = 0;
  spin_unlock_irqrestore(&lock, fl);

  struct rtc_time t;
  epoch_to_rtc((uint32_t)tv->tv_sec, &t);
  rtc_set(&t);
  return true;
}

bool wallclock_adjust(const struct timeval64 *delta, struct timeval64 *old)
{
  if (delta && (delta->tv_sec < -MAX_ADJ_SECS || delta->tv_sec > MAX_ADJ_SECS || delta->tv_usec < 0 ||
                delta->tv_usec >= US_PER_S))
    return false;
  uintptr_t fl = spin_lock_irqsave(&lock);
  advance();
  if (old)
    split(slew.remaining, old);
  if (delta) {
    slew.remaining = delta->tv_sec * US_PER_S + delta->tv_usec;
    slew.carry = 0;
  }
  spin_unlock_irqrestore(&lock, fl);
  return true;
}
