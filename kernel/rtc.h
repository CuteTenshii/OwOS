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

#endif /* OWOS_RTC_H */
