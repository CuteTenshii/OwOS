/* rtc.c — read date/time from the CMOS RTC (ports 0x70/0x71).
 *
 * QEMU defaults to BCD values and 24-hour mode, taken from the host clock.
 */

#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
	outb(CMOS_ADDR, reg);
	return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
	return cmos_read(0x0A) & 0x80;     /* status A, bit 7 = update-in-progress */
}

static uint8_t bcd_to_bin(uint8_t v)
{
	return (v & 0x0F) + ((v >> 4) * 10);
}

void rtc_now(struct rtc_time *t)
{
	/* Don't read mid-update; bounded so a quirky RTC can't hang us. */
	for (int i = 0; i < 1000000 && update_in_progress(); i++)
		;

	uint8_t sec   = cmos_read(0x00);
	uint8_t min   = cmos_read(0x02);
	uint8_t hour  = cmos_read(0x04);
	uint8_t day   = cmos_read(0x07);
	uint8_t month = cmos_read(0x08);
	uint8_t year  = cmos_read(0x09);
	uint8_t statusB = cmos_read(0x0B);

	if (!(statusB & 0x04)) {           /* bit 2 clear => values are BCD        */
		sec = bcd_to_bin(sec);
		min = bcd_to_bin(min);
		hour = bcd_to_bin(hour);
		day = bcd_to_bin(day);
		month = bcd_to_bin(month);
		year = bcd_to_bin(year);
	}

	t->sec = sec;
	t->min = min;
	t->hour = hour;
	t->day = day;
	t->month = month;
	t->year = (uint16_t)(2000 + year);
}
