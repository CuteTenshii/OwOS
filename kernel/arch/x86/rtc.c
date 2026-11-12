/* rtc.c: read date/time from the CMOS RTC (ports 0x70/0x71).
 *
 * QEMU defaults to BCD values and 24-hour mode, taken from the host clock.
 */

#include "rtc.h"
#include "io.h"
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
  outb(CMOS_ADDR, reg);
  return inb(CMOS_DATA);
}

static int update_in_progress(void) { return cmos_read(0x0A) & 0x80; /* status A, bit 7 = update-in-progress */ }

static void cmos_write(uint8_t reg, uint8_t val)
{
  outb(CMOS_ADDR, reg);
  outb(CMOS_DATA, val);
}

static uint8_t bcd_to_bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void rtc_now(struct rtc_time *t)
{
  /* Don't read mid-update; bounded so a quirky RTC can't hang us. */
  for (int i = 0; i < 1000000 && update_in_progress(); i++)
    ;

  uint8_t sec = cmos_read(0x00);
  uint8_t min = cmos_read(0x02);
  uint8_t hour = cmos_read(0x04);
  uint8_t day = cmos_read(0x07);
  uint8_t month = cmos_read(0x08);
  uint8_t year = cmos_read(0x09);
  uint8_t statusB = cmos_read(0x0B);
  bool h12 = !(statusB & 0x02); /* bit 1 clear => 12-hour mode, PM in hour bit 7 */
  bool pm = h12 && (hour & 0x80);
  if (h12)
    hour &= 0x7F;

  if (!(statusB & 0x04)) { /* bit 2 clear => values are BCD        */
    sec = bcd_to_bin(sec);
    min = bcd_to_bin(min);
    hour = bcd_to_bin(hour);
    day = bcd_to_bin(day);
    month = bcd_to_bin(month);
    year = bcd_to_bin(year);
  }
  if (h12)
    hour = (uint8_t)(hour % 12 + (pm ? 12 : 0)); /* 12 AM is hour 0, 12 PM hour 12 */

  t->sec = sec;
  t->min = min;
  t->hour = hour;
  t->day = day;
  t->month = month;
  t->year = (uint16_t)(2000 + year);
}

/* Unix epoch seconds (UTC) from the CMOS clock, the basis for JS Date. Assumes
 * the RTC holds UTC (the ntp service sets it so). Civil-to-days is the standard
 * leap-aware form; fits unsigned 32-bit through 2106. */
unsigned rtc_epoch(void)
{
  struct rtc_time t;
  rtc_now(&t);
  int y = t.year, m = t.month, d = t.day;
  static const int mdays[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  long days = (long)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += mdays[(m >= 1 && m <= 12) ? m - 1 : 0];
  if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    days += 1;
  days += d - 1;
  /* Unsigned math: a signed 32-bit long overflows (undefined) in 2038, unsigned is exact until 2106. */
  return (unsigned)days * 86400u + (unsigned)t.hour * 3600u + (unsigned)t.min * 60u + t.sec;
}

/* Write the wall clock back to CMOS, honouring whatever format the RTC is in
 * (BCD vs binary, 12- vs 24-hour) so rtc_now reads it back unchanged. Like
 * rtc_now, only the 2-digit year register is touched (the century register is
 * chipset-specific and rtc_now hardcodes the 21st century anyway). */
void rtc_set(const struct rtc_time *t)
{
  for (int i = 0; i < 1000000 && update_in_progress(); i++)
    ;

  uint8_t statusB = cmos_read(0x0B);
  int bcd = !(statusB & 0x04); /* bit 2 clear => BCD                  */
  int h24 = (statusB & 0x02);  /* bit 1 set   => 24-hour              */

  uint8_t sec = t->sec, min = t->min, hour = t->hour;
  uint8_t day = t->day, month = t->month, year = (uint8_t)(t->year % 100);

  int pm = 0;
  if (!h24) { /* fold into 12-hour if the RTC is so  */
    pm = hour >= 12;
    uint8_t h = (uint8_t)(hour % 12);
    hour = h ? h : 12;
  }

  if (bcd) {
    sec = bin_to_bcd(sec);
    min = bin_to_bcd(min);
    hour = bin_to_bcd(hour);
    day = bin_to_bcd(day);
    month = bin_to_bcd(month);
    year = bin_to_bcd(year);
  }
  if (!h24 && pm)
    hour |= 0x80; /* 12-hour mode PM flag                */

  cmos_write(0x0B, (uint8_t)(statusB | 0x80)); /* SET bit: freeze updates */
  cmos_write(0x00, sec);
  cmos_write(0x02, min);
  cmos_write(0x04, hour);
  cmos_write(0x07, day);
  cmos_write(0x08, month);
  cmos_write(0x09, year);
  cmos_write(0x0B, statusB); /* resume updating         */
}
