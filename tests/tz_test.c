/* Host test for the time zone library (user/lib/tz/tz.c), cross-checked against the host zoneinfo. */
#define OWOS_SUITE_TEST
#define _GNU_SOURCE
#include "test.h"
#include "tz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOUR 3600LL
#define DAY 86400LL

static struct tz load(const char *name)
{
  struct tz z;
  int idx = tz_find(name);
  expect(idx >= 0 && tz_load(&z, idx) == 0);
  return z;
}

static int off_at(const struct tz *z, long long t) { return tz_offset_at(z, t, 0, 0); }

static const char *abbr_at(const struct tz *z, long long t)
{
  const char *a;
  tz_offset_at(z, t, 0, &a);
  return a;
}

static long long epoch(long long y, int mo, int d, int h, int mi, int s)
{
  struct tz_tm t = {.year = y, .month = mo, .day = d, .hour = h, .min = mi, .sec = s};
  return tz_gm_to_epoch(&t);
}

static void check_calendar(void)
{
  struct tz_tm t;
  test("calendar");
  tz_utc_to_tm(0, &t);
  expect(t.year == 1970 && t.month == 1 && t.day == 1 && t.wday == 4 && t.yday == 0);
  tz_utc_to_tm(-1, &t);
  expect(t.year == 1969 && t.month == 12 && t.day == 31 && t.hour == 23 && t.sec == 59 && t.wday == 3 && t.yday == 364);
  tz_utc_to_tm(951782400, &t);
  expect(t.year == 2000 && t.month == 2 && t.day == 29 && t.wday == 2 && t.yday == 59);
  tz_utc_to_tm(2147483648LL, &t);
  expect(t.year == 2038 && t.month == 1 && t.day == 19 && t.hour == 3 && t.min == 14 && t.sec == 8 && t.wday == 2);
  tz_utc_to_tm(4102444800LL, &t);
  expect(t.year == 2100 && t.month == 1 && t.day == 1 && t.wday == 5);
  tz_utc_to_tm(4102444800LL + 31 * DAY + 28 * DAY, &t);
  expect(t.month == 3 && t.day == 1 && t.yday == 59);
  expect(epoch(2100, 3, 1, 0, 0, 0) == 4102444800LL + 59 * DAY);
  expect(epoch(2000, 2, 30, 0, 0, 0) == epoch(2000, 3, 1, 0, 0, 0));
  expect(epoch(1999, 14, 1, 0, 0, 0) == epoch(2000, 2, 1, 0, 0, 0));
  expect(epoch(2024, 1, 1, 0, 0, -1) == epoch(2023, 12, 31, 23, 59, 59));
  expect(epoch(1969, 12, 31, 23, 59, 59) == -1);

  unsigned long long x = 12345;
  for (int i = 0; i < 20000; i++) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    long long v = (long long)(x >> 24) % 20000000000LL - 10000000000LL;
    time_t tt = (time_t)v;
    struct tm h;
    gmtime_r(&tt, &h);
    tz_utc_to_tm(v, &t);
    if (!(t.year == h.tm_year + 1900 && t.month == h.tm_mon + 1 && t.day == h.tm_mday && t.hour == h.tm_hour &&
          t.min == h.tm_min && t.sec == h.tm_sec && t.wday == h.tm_wday && t.yday == h.tm_yday)) {
      expect(0 && "gmtime mismatch");
      break;
    }
    if (tz_gm_to_epoch(&t) != v) {
      expect(0 && "round trip mismatch");
      break;
    }
  }
}

static void check_parse(void)
{
  struct tz z;
  test("parse");
  expect(tz_parse(&z, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);
  expect(z.has_dst && z.std_off == 3600 && z.dst_off == 7200 && strcmp(z.std, "CET") == 0 &&
         strcmp(z.dst, "CEST") == 0);
  expect(z.start.kind == 'M' && z.start.month == 3 && z.start.week == 5 && z.start.day == 0 && z.start.time == 7200);
  expect(z.end.month == 10 && z.end.time == 10800);
  expect(tz_parse(&z, "<+0330>-3:30") == 0 && !z.has_dst && z.std_off == 12600 && strcmp(z.std, "+0330") == 0);
  expect(tz_parse(&z, "EST5EDT") == 0 && z.has_dst && z.std_off == -18000 && z.dst_off == -14400);
  expect(z.start.month == 3 && z.start.week == 2 && z.end.month == 11 && z.end.week == 1);
  expect(tz_parse(&z, "<-02>2<-01>,M3.5.0/-1,M10.5.0/0") == 0 && z.start.time == -3600);
  expect(tz_parse(&z, "EET-2EEST,M3.4.4/50,M10.4.4/50") == 0 && z.start.time == 50 * 3600);
  expect(tz_parse(&z, "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0") == 0 && z.std_off == 37800 && z.dst_off == 39600);
  expect(tz_parse(&z, "AAA3BBB,J60/2,300") == 0 && z.start.kind == 'J' && z.end.kind == 'N' && z.end.day == 300);
  expect(tz_parse(&z, "") == -1);
  expect(tz_parse(&z, "CET") == -1);
  expect(tz_parse(&z, "CET-1CEST,M13.1.0,M10.5.0") == -1);
  expect(tz_parse(&z, "CET-1CEST,M3.0.0,M10.5.0") == -1);
  expect(tz_parse(&z, "CET-1CEST,M3.5.0") == -1);
  expect(tz_parse(&z, "CET-1CEST,M3.5.0,M10.5.0x") == -1);
  expect(tz_parse(&z, "<+03-3") == -1);
  expect(tz_parse(&z, "CET-1:60") == -1);
  expect(tz_parse(&z, "AB-1") == -1);
}

static void check_rules(void)
{
  struct tz ny = load("America/New_York"), syd = load("Australia/Sydney");
  int dst;
  const char *abbr;
  test("transitions");
  expect(tz_offset_at(&ny, 1710053999, &dst, &abbr) == -18000 && !dst && strcmp(abbr, "EST") == 0);
  expect(tz_offset_at(&ny, 1710054000, &dst, &abbr) == -14400 && dst && strcmp(abbr, "EDT") == 0);
  expect(off_at(&ny, 1730613599) == -14400 && off_at(&ny, 1730613600) == -18000);
  expect(off_at(&syd, 1712419199) == 39600 && off_at(&syd, 1712419200) == 36000);
  expect(off_at(&syd, 1728143999) == 36000 && off_at(&syd, 1728144000) == 39600);
  expect(tz_offset_at(&syd, epoch(2024, 1, 15, 0, 0, 0), &dst, &abbr) == 39600 && dst && strcmp(abbr, "AEDT") == 0);
  expect(tz_offset_at(&syd, epoch(2024, 7, 15, 0, 0, 0), &dst, &abbr) == 36000 && !dst && strcmp(abbr, "AEST") == 0);
  expect(off_at(&syd, epoch(2024, 12, 31, 23, 59, 59)) == 39600 && off_at(&syd, epoch(2025, 1, 1, 0, 0, 0)) == 39600);
  expect(off_at(&ny, 2152162800LL - 1) == -18000 && off_at(&ny, 2152162800LL) == -14400);
  expect(off_at(&ny, 4108690800LL - 1) == -18000 && off_at(&ny, 4108690800LL) == -14400);
  expect(off_at(&ny, epoch(2038, 1, 19, 3, 14, 8)) == -18000);
  expect(off_at(&ny, -1) == -18000 && off_at(&ny, 0) == -18000);
  expect(off_at(&ny, epoch(1970, 7, 1, 0, 0, 0)) == -14400);
}

static void check_calendar_rules(void)
{
  struct tz z;
  test("day-of-year and leap rules");
  expect(tz_parse(&z, "AAA0BBB,J60,J300") == 0);
  expect(off_at(&z, epoch(2000, 3, 1, 1, 59, 59)) == 0 && off_at(&z, epoch(2000, 3, 1, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2001, 3, 1, 1, 59, 59)) == 0 && off_at(&z, epoch(2001, 3, 1, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2100, 3, 1, 1, 59, 59)) == 0 && off_at(&z, epoch(2100, 3, 1, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2000, 2, 29, 12, 0, 0)) == 0 && off_at(&z, epoch(2000, 2, 28, 12, 0, 0)) == 0);
  expect(tz_parse(&z, "AAA0BBB,59,300") == 0);
  expect(off_at(&z, epoch(2000, 2, 29, 1, 59, 59)) == 0 && off_at(&z, epoch(2000, 2, 29, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2001, 3, 1, 1, 59, 59)) == 0 && off_at(&z, epoch(2001, 3, 1, 2, 0, 0)) == 3600);
  expect(tz_parse(&z, "AAA0BBB,M2.5.0,M11.1.0") == 0);
  expect(off_at(&z, epoch(2004, 2, 29, 1, 59, 59)) == 0 && off_at(&z, epoch(2004, 2, 29, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2000, 2, 27, 1, 59, 59)) == 0 && off_at(&z, epoch(2000, 2, 27, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2100, 2, 28, 1, 59, 59)) == 0 && off_at(&z, epoch(2100, 2, 28, 2, 0, 0)) == 3600);
  expect(off_at(&z, epoch(2026, 2, 22, 1, 59, 59)) == 0 && off_at(&z, epoch(2026, 2, 22, 2, 0, 0)) == 3600);
  expect(tz_parse(&z, "AAA0BBB,M10.5.0,M11.1.0") == 0);
  expect(off_at(&z, epoch(2025, 10, 26, 2, 0, 0)) == 3600 && off_at(&z, epoch(2025, 10, 25, 2, 0, 0)) == 0);
}

static void check_offsets(void)
{
  struct tz z;
  char buf[32];
  test("fixed offsets");
  z = load("Asia/Tokyo");
  expect(off_at(&z, 1700000000) == 32400 && strcmp(abbr_at(&z, 0), "JST") == 0);
  z = load("Asia/Kolkata");
  expect(off_at(&z, 1700000000) == 19800);
  z = load("Asia/Kathmandu");
  expect(off_at(&z, 1700000000) == 20700);
  z = load("Pacific/Honolulu");
  expect(off_at(&z, 1700000000) == -36000);
  z = load("Pacific/Kiritimati");
  expect(off_at(&z, 1700000000) == 14 * 3600);
  z = load("Etc/GMT+12");
  expect(off_at(&z, 1700000000) == -12 * 3600);
  z = load("Etc/GMT-14");
  expect(off_at(&z, 1700000000) == 14 * 3600);
  z = load("America/St_Johns");
  expect(off_at(&z, epoch(2024, 1, 1, 0, 0, 0)) == -12600 && off_at(&z, epoch(2024, 7, 1, 0, 0, 0)) == -9000);
  z = load("Australia/Lord_Howe");
  expect(off_at(&z, epoch(2024, 1, 1, 0, 0, 0)) == 39600 && off_at(&z, epoch(2024, 7, 1, 0, 0, 0)) == 37800);
  z = load("Europe/Dublin");
  expect(off_at(&z, epoch(2024, 1, 1, 0, 0, 0)) == 0 && off_at(&z, epoch(2024, 7, 1, 0, 0, 0)) == 3600);
  z = load("UTC");
  expect(off_at(&z, 1700000000) == 0 && strcmp(abbr_at(&z, 0), "UTC") == 0);

  test("format");
  tz_format_offset(buf, sizeof(buf), 19800);
  expect_string(buf, "UTC+05:30");
  tz_format_offset(buf, sizeof(buf), -12600);
  expect_string(buf, "UTC-03:30");
  tz_format_offset(buf, sizeof(buf), 20700);
  expect_string(buf, "UTC+05:45");
  tz_format_offset(buf, sizeof(buf), 0);
  expect_string(buf, "UTC+00:00");
  tz_format_offset(buf, sizeof(buf), 14 * 3600);
  expect_string(buf, "UTC+14:00");
  expect(tz_format_offset(buf, 6, 3600) == 5 && strcmp(buf, "UTC+0") == 0);
  tz_city("America/Argentina/Buenos_Aires", buf, sizeof(buf));
  expect_string(buf, "Buenos Aires");
  tz_city("Europe/Paris", buf, sizeof(buf));
  expect_string(buf, "Paris");
  tz_city("UTC", buf, sizeof(buf));
  expect_string(buf, "UTC");
  expect(tz_city("America/New_York", buf, 4) == 3 && strcmp(buf, "New") == 0);
}

static void check_local(void)
{
  struct tz ny = load("America/New_York"), syd = load("Australia/Sydney"), tok = load("Asia/Tokyo");
  test("local to utc");
  expect(tz_local_to_utc(&ny, epoch(2024, 3, 10, 1, 59, 59)) == epoch(2024, 3, 10, 6, 59, 59));
  expect(tz_local_to_utc(&ny, epoch(2024, 3, 10, 2, 30, 0)) == epoch(2024, 3, 10, 7, 30, 0));
  expect(tz_local_to_utc(&ny, epoch(2024, 3, 10, 3, 0, 0)) == 1710054000);
  expect(tz_local_to_utc(&ny, epoch(2024, 11, 3, 0, 59, 59)) == epoch(2024, 11, 3, 4, 59, 59));
  expect(tz_local_to_utc(&ny, epoch(2024, 11, 3, 1, 30, 0)) == epoch(2024, 11, 3, 5, 30, 0));
  expect(tz_local_to_utc(&ny, epoch(2024, 11, 3, 2, 0, 0)) == 1730613600 + HOUR);
  expect(tz_local_to_utc(&syd, epoch(2024, 4, 7, 2, 30, 0)) == epoch(2024, 4, 6, 15, 30, 0));
  expect(tz_local_to_utc(&syd, epoch(2024, 10, 6, 2, 30, 0)) == epoch(2024, 10, 5, 16, 30, 0));
  expect(tz_local_to_utc(&tok, epoch(2024, 1, 1, 9, 0, 0)) == epoch(2024, 1, 1, 0, 0, 0));

  /* every local time outside a gap maps back to itself */
  int bad = 0;
  for (long long l = epoch(2023, 1, 1, 0, 0, 0); l < epoch(2025, 1, 1, 0, 0, 0); l += 900) {
    long long u = tz_local_to_utc(&ny, l);
    if (u + off_at(&ny, u) != l && !(l >= epoch(2024, 3, 10, 2, 0, 0) && l < epoch(2024, 3, 10, 3, 0, 0)) &&
        !(l >= epoch(2023, 3, 12, 2, 0, 0) && l < epoch(2023, 3, 12, 3, 0, 0)))
      bad++;
  }
  expect(bad == 0);
}

static void check_table(void)
{
  struct tz z;
  test("table");
  expect(tz_count() >= 120);
  for (int i = 0; i < tz_count(); i++) {
    expect(tz_load(&z, i) == 0);
    expect(tz_find(tz_name_at(i)) == i);
    if (i > 0)
      expect(strcmp(tz_name_at(i - 1), tz_name_at(i)) < 0);
  }
  expect(tz_name_at(-1) == 0 && tz_name_at(tz_count()) == 0 && tz_load(&z, tz_count()) == -1);
  expect(tz_find("europe/paris") == tz_find("Europe/Paris") && tz_find("EUROPE/PARIS") >= 0);
  expect(tz_find("Mars/Base") == -1 && tz_find("") == -1 && tz_find("Europe/Pari") == -1);
  expect(tz_find("UTC") >= 0 && tz_find("utc") == tz_find("UTC") && tz_find("Z") == tz_find("UTC"));
  expect(tz_find("GMT") == tz_find("Etc/GMT") && tz_find("GMT") != tz_find("UTC"));
  expect(strcmp(tz_name_at(tz_find("Zulu")), "UTC") == 0);
}

static void check_config(void)
{
  struct tz z;
  char name[48];
  test("config");
  unlink(TZ_CONFIG_PATH);
  expect(tz_load_config(&z, name, sizeof(name)) == -1);
  expect_string(name, "UTC");
  expect(z.std_off == 0 && !z.has_dst);
  expect(tz_save_config("europe/paris") == 0);
  expect(tz_load_config(&z, name, sizeof(name)) == 0);
  expect_string(name, "Europe/Paris");
  expect(z.has_dst && z.std_off == 3600);
  expect(tz_save_config("Mars/Base") == -1);
  expect(tz_load_config(&z, name, sizeof(name)) == 0);
  expect_string(name, "Europe/Paris");
  FILE *f = fopen(TZ_CONFIG_PATH, "w");
  fputs("  Asia/Tokyo \r\n", f);
  fclose(f);
  expect(tz_load_config(&z, name, sizeof(name)) == 0);
  expect_string(name, "Asia/Tokyo");
  expect(z.std_off == 32400);
  f = fopen(TZ_CONFIG_PATH, "w");
  fputs("Nowhere/Land\n", f);
  fclose(f);
  expect(tz_load_config(&z, name, sizeof(name)) == -1);
  expect_string(name, "UTC");
  expect(tz_load_config(&z, name, 4) == -1 && strcmp(name, "UTC") == 0);
  expect(tz_load_config(&z, 0, 0) == -1);
  unlink(TZ_CONFIG_PATH);
}

/* The footer rule only describes a zone after its last explicit transition, so every zone is compared
 * from 2028 and the long-stable ones from 2012. Casablanca's Ramadan shifts are explicit past 2037. */
static const char *const stable[] = {
    "America/New_York", "America/Chicago",  "America/Denver",     "America/Los_Angeles", "America/Anchorage",
    "America/Toronto",  "America/Halifax",  "America/St_Johns",   "Europe/London",       "Europe/Paris",
    "Europe/Berlin",    "Europe/Dublin",    "Europe/Lisbon",      "Europe/Athens",       "Europe/Helsinki",
    "Australia/Sydney", "Australia/Hobart", "Australia/Adelaide", "Australia/Lord_Howe", "Pacific/Auckland",
    "Pacific/Chatham",  "Asia/Tokyo",       "Asia/Kolkata",       "Atlantic/Azores",
};

static bool is_stable(const char *name)
{
  for (size_t i = 0; i < sizeof(stable) / sizeof(stable[0]); i++)
    if (strcmp(name, stable[i]) == 0)
      return true;
  return false;
}

static void host_at(long long t, int *off, int *dst, char *abbr)
{
  time_t tt = (time_t)t;
  struct tm h;
  localtime_r(&tt, &h);
  *off = (int)h.tm_gmtoff;
  *dst = h.tm_isdst > 0;
  snprintf(abbr, 16, "%s", h.tm_zone);
}

static bool same_as_host(const struct tz *z, long long t)
{
  int hoff, hdst, dst;
  char habbr[16];
  const char *abbr;
  host_at(t, &hoff, &hdst, habbr);
  int off = tz_offset_at(z, t, &dst, &abbr);
  return off == hoff && dst == hdst && strcmp(abbr, habbr) == 0;
}

/* Compare every 6 hours, and each host transition at -1s, 0s and +1s. Returns the transitions seen. */
static int compare_zone(int idx, long long from, long long to, bool *ok)
{
  struct tz z;
  int transitions = 0, prev_off, prev_dst, off, dst, moff, mdst;
  char abbr[16];
  setenv("TZ", tz_name_at(idx), 1);
  tzset();
  tz_load(&z, idx);
  host_at(from, &prev_off, &prev_dst, abbr);
  for (long long t = from, prev_t = from; t <= to; prev_t = t, t += 6 * HOUR) {
    host_at(t, &off, &dst, abbr);
    *ok = *ok && same_as_host(&z, t);
    if (off != prev_off || dst != prev_dst) {
      long long lo = prev_t, hi = t;
      while (hi - lo > 1) {
        long long mid = lo + (hi - lo) / 2;
        host_at(mid, &moff, &mdst, abbr);
        if (moff == prev_off && mdst == prev_dst)
          lo = mid;
        else
          hi = mid;
      }
      transitions++;
      for (long long d = -1; d <= 1; d++)
        *ok = *ok && same_as_host(&z, hi + d);
    }
    prev_off = off;
    prev_dst = dst;
  }
  return transitions;
}

static void check_host(void)
{
  int transitions = 0;
  test("host zoneinfo");
  for (int i = 0; i < tz_count(); i++) {
    const char *name = tz_name_at(i);
    bool ok = true;
    if (strcmp(name, "Africa/Casablanca") == 0)
      continue;
    transitions +=
        compare_zone(i, epoch(is_stable(name) ? 2012 : 2028, 1, 1, 0, 0, 0), epoch(2037, 12, 31, 0, 0, 0), &ok);
    if (!ok)
      fprintf(stderr, "tz mismatch: %s\n", name);
    expect(ok);
  }
  unsetenv("TZ");
  tzset();
  expect(transitions > 1500);
}

int main(void)
{
  check_calendar();
  check_parse();
  check_rules();
  check_calendar_rules();
  check_offsets();
  check_local();
  check_table();
  check_config();
  check_host();
  return 0;
}
