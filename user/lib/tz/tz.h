/* tz.h: time zones from POSIX TZ rules, calendar helpers and the /etc/timezone config.
 * Integer only, no libc. Offsets are seconds east of UTC. */
#ifndef OWOS_TZ_H
#define OWOS_TZ_H

#include <stdbool.h>

#define TZ_ABBR_MAX 16
#define TZ_DEFAULT_ZONE "UTC"

struct tz_zone {
  const char *name; /* IANA name, e.g. "Europe/Paris" */
  const char *rule; /* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
};

struct tz_rule {
  char kind; /* 'M' month.week.day, 'J' day 1..365 skipping Feb 29, 'N' day 0..365 */
  int month; /* 'M': 1..12 */
  int week;  /* 'M': 1..5, 5 is the last */
  int day;   /* 'M': 0..6 from Sunday; 'J'/'N': day number */
  int time;  /* seconds after local midnight, may be negative or over 24h */
};

struct tz {
  char std[TZ_ABBR_MAX];
  char dst[TZ_ABBR_MAX];
  int std_off;
  int dst_off;
  bool has_dst;
  struct tz_rule start; /* into DST, in local standard time */
  struct tz_rule end;   /* out of DST, in local DST time */
};

struct tz_tm {
  long long year;
  int month; /* 1..12 */
  int day;   /* 1..31 */
  int hour;
  int min;
  int sec;
  int wday; /* 0 = Sunday */
  int yday; /* 0..365 */
};

/* Parse a POSIX TZ string; 0, or -1 if malformed. */
int tz_parse(struct tz *z, const char *posix);

/* Zone table, sorted by name. tz_find is case-insensitive, accepts the UTC/GMT/Z/UCT/Zulu aliases and
 * returns the table index or -1. tz_load parses table entry `idx`. */
int tz_count(void);
const char *tz_name_at(int idx);
const char *tz_rule_at(int idx);
int tz_find(const char *name);
int tz_load(struct tz *z, int idx);

/* Offset in effect at `utc`. is_dst and abbr may be NULL. */
int tz_offset_at(const struct tz *z, long long utc, int *is_dst, const char **abbr);

/* Local wall time (seconds since the epoch, read as if UTC) to UTC. A time repeated by a fall-back
 * resolves to its first occurrence. A time skipped by a spring-forward resolves with the offset in
 * effect before the gap, so it lands after the gap (02:30 becomes 03:30). */
long long tz_local_to_utc(const struct tz *z, long long local);

/* Broken-down time of `t` seconds since the epoch; add the zone offset first for local time. */
void tz_utc_to_tm(long long t, struct tz_tm *out);
/* Inverse of tz_utc_to_tm. Ignores wday/yday and tolerates out-of-range month, day, hour, min, sec. */
long long tz_gm_to_epoch(const struct tz_tm *t);

/* "UTC+05:30" into buf; returns the length written (truncated to cap-1). */
int tz_format_offset(char *buf, int cap, int off);
/* "America/Argentina/Buenos_Aires" becomes "Buenos Aires"; returns the length written. */
int tz_city(const char *name, char *buf, int cap);

/* Read /etc/timezone into *out and the canonical zone name into name_out.
 * 0 if the file named a known zone, -1 if it fell back to UTC. */
int tz_load_config(struct tz *out, char *name_out, int cap);
/* Write the canonical name for `name` to /etc/timezone; -1 if unknown or the write failed. */
int tz_save_config(const char *name);

#endif
