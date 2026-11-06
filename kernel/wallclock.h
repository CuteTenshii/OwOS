/* wallclock.h: microsecond wall clock anchored to the RTC, steppable and slewable. */
#ifndef OWOS_WALLCLOCK_H
#define OWOS_WALLCLOCK_H

#include "timeval64.h"
#include <stdbool.h>

void wallclock_init(void);
void wallclock_get(struct timeval64 *tv);
bool wallclock_set(const struct timeval64 *tv);                              /* step; persists to CMOS */
bool wallclock_adjust(const struct timeval64 *delta, struct timeval64 *old); /* slew; delta may be NULL */

#endif
