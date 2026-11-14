/* timer_a64.h: the generic timer's interrupt side, for exc.c; the rest is the shared timer.h API (timer.c). */
#ifndef OWOS_ARCH_AARCH64_TIMER_A64_H
#define OWOS_ARCH_AARCH64_TIMER_A64_H

/* Load the next interval on the calling CPU, which lowers its (level-triggered) timer interrupt. */
void timer_rearm(void);

/* One tick on the calling CPU: CPU 0 keeps time and wakes sleepers; every CPU then runs the tick callback. */
void timer_tick(void);

#endif /* OWOS_ARCH_AARCH64_TIMER_A64_H */
