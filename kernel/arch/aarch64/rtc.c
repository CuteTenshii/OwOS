/* rtc.c: the shared rtc.h API on a PL031, which counts seconds since the Unix epoch. Without one the clock reads 0
 * (1970) until something sets the wall clock, such as ntpd. */
#include "rtc.h"
#include "arch.h"
#include "syscall_path.h"

#define RTCDR 0x00 /* current count */
#define RTCLR 0x08 /* load a new count */

unsigned rtc_epoch(void) { return plat.rtc_base ? mmio_read32(plat.rtc_base + RTCDR) : 0; }

void rtc_now(struct rtc_time *t) { epoch_to_rtc(rtc_epoch(), t); }

/* Days from 1970-01-01 to a civil date (Hinnant's days-from-civil), so the PL031 can be loaded from one. */
static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
  y -= m <= 2;
  uint32_t era = y / 400, yoe = y - era * 400;
  uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void rtc_set(const struct rtc_time *t)
{
  if (!plat.rtc_base || t->year < 1970)
    return;
  uint32_t secs = days_from_civil(t->year, t->month, t->day) * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
  mmio_write32(plat.rtc_base + RTCLR, secs);
}
