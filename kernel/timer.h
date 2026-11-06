/* timer.h — system timer on IRQ0.
 *
 * We program the PIT to a fixed TIMER_HZ so ticks map cleanly to wall-clock
 * time and timer_sleep() can block for a real duration.
 */
#ifndef OWOS_TIMER_H
#define OWOS_TIMER_H

#include <stdint.h>

#define TIMER_HZ                                                                                                       \
  250 /* PIT interrupts per second. Finer than the classic                                                             \
       * 100 Hz so the compositor can pace to a real 60/120 Hz                                                         \
       * frame budget (10 ms tick granularity quantised it to                                                          \
       * 50 fps) and input latency drops to one 4 ms tick.                                                             \
       * Raw timer_ticks() deadlines are expressed in TIMER_HZ                                                         \
       * units (N * TIMER_HZ seconds), so they track this. */

typedef void (*timer_tick_fn)(void);

void timer_init(void);                /* program the PIT, claim IRQ0             */
void timer_on_tick(timer_tick_fn cb); /* run cb on every tick (e.g. sched) */
uint32_t timer_ticks(void);           /* raw ticks since boot                    */
uint64_t timer_us64(void);            /* 64-bit microseconds since boot          */
uint32_t timer_us(void);              /* microseconds since boot (PIT resolution)*/
uint32_t timer_seconds(void);         /* uptime in seconds                       */
void timer_sleep(uint32_t ms);        /* block (hlt) for at least ms millisecs   */

#endif /* OWOS_TIMER_H */
