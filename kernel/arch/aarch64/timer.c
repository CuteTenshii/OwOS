/* timer.c: the shared timer API (timer.h) on the generic timer: each CPU's EL1 physical timer ticks at TIMER_HZ, and
 * the virtual counter gives microseconds. */
#include "timer.h"
#include "arch.h"
#include "entropy.h"
#include "sched.h"
#include "smp.h"
#include "timer_a64.h"

#define CNTKCTL_EL0VCTEN (1u << 1) /* EL0 may read the virtual counter */

static volatile uint32_t ticks;
static volatile uint32_t ticks_hi; /* ticks wraps (198 days at 250 Hz); this extends it to 64 bits */
static timer_tick_fn tick_cb;
static uint64_t counter_hz, interval;

void timer_init(void)
{
  counter_hz = SYSREG_READ(cntfrq_el0);
  interval = counter_hz / TIMER_HZ;
  SYSREG_WRITE(cntkctl_el1, SYSREG_READ(cntkctl_el1) | CNTKCTL_EL0VCTEN);
  SYSREG_WRITE(cntp_tval_el0, interval);
  SYSREG_WRITE(cntp_ctl_el0, 1); /* enabled, interrupt unmasked */
}

void timer_rearm(void) { SYSREG_WRITE(cntp_tval_el0, interval); }

void timer_tick(void)
{
  if (smp_this_cpu() == 0) {
    if (++ticks == 0)
      ticks_hi++;
    entropy_stir(ticks);   /* interrupt-latency jitter feeds the CSPRNG */
    sched_wake_due(ticks); /* promote timed sleepers that are now due */
  }
  if (tick_cb)
    tick_cb(); /* the scheduler preempts here */
}

void timer_on_tick(timer_tick_fn cb) { tick_cb = cb; }

uint32_t timer_ticks(void) { return ticks; }

uint64_t timer_us64(void)
{
  uint64_t count = SYSREG_READ(cntvct_el0);
  return (uint64_t)((unsigned __int128)count * 1000000u / counter_hz);
}

uint32_t timer_us(void) { return (uint32_t)timer_us64(); }

uint32_t timer_seconds(void) { return ticks / TIMER_HZ; }

void timer_sleep(uint32_t ms)
{
  /* Rounded up so a short sleep never returns early, in 64 bits so a large ms can't wrap to a tiny wait. */
  uint32_t wait = (uint32_t)(((uint64_t)ms * TIMER_HZ + 999) / 1000);
  if (wait == 0)
    return;
  sched_sleep_until(ticks + wait); /* off the run queue until the tick that wakes it */
}
