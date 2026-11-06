/* slew.h: pure arithmetic for bounded clock slewing (adjtime-style). */
#ifndef OWOS_SLEW_H
#define OWOS_SLEW_H

#include <stdint.h>

#define SLEW_MAX_PPM 500
#define SLEW_US_PER_S 1000000u

struct slew {
  int64_t remaining; /* offset still to be applied, microseconds */
  uint64_t carry;    /* sub-microsecond budget, in microsecond-ppm */
};

/* Advance `delta_us` of elapsed time; returns the correction to add to the clock (|rate| <= SLEW_MAX_PPM). */
static inline int64_t slew_step(struct slew *s, int64_t delta_us)
{
  if (delta_us <= 0 || s->remaining == 0)
    return 0;
  s->carry += (uint64_t)delta_us * SLEW_MAX_PPM;
  int64_t budget = (int64_t)(s->carry / SLEW_US_PER_S);
  s->carry %= SLEW_US_PER_S;
  int64_t adj = s->remaining;
  if (adj > budget)
    adj = budget;
  else if (adj < -budget)
    adj = -budget;
  s->remaining -= adj;
  if (s->remaining == 0)
    s->carry = 0;
  return adj;
}

#endif
