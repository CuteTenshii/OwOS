/* Host stand-in for kernel/timer.h: the test owns the clock. */
#ifndef UVC_HOST_TIMER_H
#define UVC_HOST_TIMER_H

#include <stdint.h>

#define TIMER_HZ 250
uint32_t timer_ticks(void);

#endif
