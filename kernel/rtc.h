/* rtc.h — read the CMOS real-time clock. */
#ifndef OWOS_RTC_H
#define OWOS_RTC_H

#include <stdint.h>

struct rtc_time {
	uint8_t  sec, min, hour;
	uint8_t  day, month;
	uint16_t year;
};

void rtc_now(struct rtc_time *t);
void rtc_set(const struct rtc_time *t);   /* write the CMOS clock (e.g. from NTP) */
unsigned rtc_epoch(void);                 /* Unix epoch seconds (UTC) from the RTC */

#endif /* OWOS_RTC_H */
