/* tz.c: POSIX TZ rule evaluation over the built-in zone table in zones.h. */
#include "tz.h"
#include "owos.h"

#ifndef TZ_CONFIG_PATH
#define TZ_CONFIG_PATH "/etc/timezone"
#endif

#include "zones.h"

static const struct {
  const char *alias;
  const char *zone;
} aliases[] = {
    {"GMT", "Etc/GMT"}, {"Z", "UTC"}, {"UCT", "UTC"}, {"Zulu", "UTC"}, {"Universal", "UTC"}, {"Etc/UTC", "UTC"},
};

#define N_ZONES ((int)(sizeof(tz_zones) / sizeof(tz_zones[0])))
#define N_ALIASES ((int)(sizeof(aliases) / sizeof(aliases[0])))

static long long floor_div(long long a, long long b)
{
  long long q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static bool is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(long long y, int m)
{
  static const char dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : dim[m - 1];
}

static long long days_from_civil(long long y, int m, int d)
{
  y -= m <= 2;
  long long era = floor_div(y, 400);
  long long yoe = y - era * 400;
  long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, long long *y, int *m, int *d)
{
  z += 719468;
  long long era = floor_div(z, 146097);
  long long doe = z - era * 146097;
  long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long long mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = yoe + era * 400 + (*m <= 2);
}

static int weekday(long long days) { return (int)(((days % 7) + 11) % 7); }

void tz_utc_to_tm(long long t, struct tz_tm *out)
{
  long long days = floor_div(t, 86400);
  int rem = (int)(t - days * 86400);
  civil_from_days(days, &out->year, &out->month, &out->day);
  out->hour = rem / 3600;
  out->min = rem % 3600 / 60;
  out->sec = rem % 60;
  out->wday = weekday(days);
  out->yday = (int)(days - days_from_civil(out->year, 1, 1));
}

long long tz_gm_to_epoch(const struct tz_tm *t)
{
  long long m = t->month - 1;
  long long y = t->year + floor_div(m, 12);
  int mon = (int)(m - floor_div(m, 12) * 12) + 1;
  long long days = days_from_civil(y, mon, 1) + (t->day - 1);
  return days * 86400 + t->hour * 3600LL + t->min * 60LL + t->sec;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static int parse_num(const char **p, int max, int *out)
{
  if (!digit(**p))
    return -1;
  int v = 0;
  while (digit(**p)) {
    v = v * 10 + (**p - '0');
    if (v > max)
      return -1;
    (*p)++;
  }
  *out = v;
  return 0;
}

static int parse_name(const char **p, char *out)
{
  const char *s = *p;
  int n = 0;
  bool quoted = *s == '<';
  if (quoted)
    s++;
  for (; quoted ? *s && *s != '>' : lower(*s) >= 'a' && lower(*s) <= 'z'; s++) {
    if (n >= TZ_ABBR_MAX - 1)
      return -1;
    out[n++] = *s;
  }
  if (quoted && *s++ != '>')
    return -1;
  if (n < 3)
    return -1;
  out[n] = 0;
  *p = s;
  return 0;
}

/* [+-]hh[:mm[:ss]] as written, in seconds. */
static int parse_hms(const char **p, int *out)
{
  int sign = 1, h, m = 0, s = 0;
  if (**p == '+' || **p == '-')
    sign = *(*p)++ == '-' ? -1 : 1;
  if (parse_num(p, 167, &h) < 0)
    return -1;
  if (**p == ':') {
    (*p)++;
    if (parse_num(p, 59, &m) < 0)
      return -1;
    if (**p == ':') {
      (*p)++;
      if (parse_num(p, 59, &s) < 0)
        return -1;
    }
  }
  *out = sign * (h * 3600 + m * 60 + s);
  return 0;
}

static int parse_rule(const char **p, struct tz_rule *r)
{
  r->time = 7200;
  r->month = r->week = r->day = 0;
  if (**p == 'M') {
    (*p)++;
    r->kind = 'M';
    if (parse_num(p, 12, &r->month) < 0 || r->month < 1 || *(*p)++ != '.')
      return -1;
    if (parse_num(p, 5, &r->week) < 0 || r->week < 1 || *(*p)++ != '.')
      return -1;
    if (parse_num(p, 6, &r->day) < 0)
      return -1;
  } else {
    r->kind = **p == 'J' ? 'J' : 'N';
    if (**p == 'J')
      (*p)++;
    if (parse_num(p, 365, &r->day) < 0 || (r->kind == 'J' && r->day < 1))
      return -1;
  }
  if (**p == '/') {
    (*p)++;
    return parse_hms(p, &r->time);
  }
  return 0;
}

int tz_parse(struct tz *z, const char *posix)
{
  const char *p = posix;
  int v;
  z->has_dst = false;
  z->dst[0] = 0;
  if (parse_name(&p, z->std) < 0 || parse_hms(&p, &v) < 0)
    return -1;
  z->std_off = -v;
  z->dst_off = z->std_off;
  if (*p == 0)
    return 0;
  if (parse_name(&p, z->dst) < 0)
    return -1;
  z->dst_off = z->std_off + 3600;
  if (*p != ',' && *p != 0) {
    if (parse_hms(&p, &v) < 0)
      return -1;
    z->dst_off = -v;
  }
  z->has_dst = true;
  if (*p == 0) {
    const char *def_start = "M3.2.0", *def_end = "M11.1.0";
    parse_rule(&def_start, &z->start);
    parse_rule(&def_end, &z->end);
    return 0;
  }
  if (*p++ != ',' || parse_rule(&p, &z->start) < 0 || *p++ != ',' || parse_rule(&p, &z->end) < 0 || *p != 0)
    return -1;
  return 0;
}

/* Rule instant of `year` in UTC, given the offset in force just before it. */
static long long rule_utc(const struct tz_rule *r, long long year, int off)
{
  long long days;
  if (r->kind == 'M') {
    int first = weekday(days_from_civil(year, r->month, 1));
    int dom = 1 + (r->day - first + 7) % 7 + (r->week - 1) * 7;
    while (dom > days_in_month(year, r->month))
      dom -= 7;
    days = days_from_civil(year, r->month, dom);
  } else {
    int n = r->day;
    if (r->kind == 'J')
      n += (n >= 60 && is_leap(year)) - 1;
    days = days_from_civil(year, 1, 1) + n;
  }
  return days * 86400 + r->time - off;
}

int tz_offset_at(const struct tz *z, long long utc, int *is_dst, const char **abbr)
{
  bool dst = false;
  if (z->has_dst) {
    struct tz_tm tm;
    tz_utc_to_tm(utc + z->std_off, &tm);
    long long s = rule_utc(&z->start, tm.year, z->std_off);
    long long e = rule_utc(&z->end, tm.year, z->dst_off);
    dst = s < e ? (utc >= s && utc < e) : (utc < e || utc >= s);
  }
  if (is_dst)
    *is_dst = dst;
  if (abbr)
    *abbr = dst ? z->dst : z->std;
  return dst ? z->dst_off : z->std_off;
}

long long tz_local_to_utc(const struct tz *z, long long local)
{
  int hi = z->std_off > z->dst_off ? z->std_off : z->dst_off;
  int lo = z->std_off > z->dst_off ? z->dst_off : z->std_off;
  if (!z->has_dst || hi == lo)
    return local - z->std_off;
  long long early = local - hi;
  return tz_offset_at(z, early, 0, 0) == hi ? early : local - lo;
}

int tz_count(void) { return N_ZONES; }

const char *tz_name_at(int idx) { return idx >= 0 && idx < N_ZONES ? tz_zones[idx].name : 0; }

const char *tz_rule_at(int idx) { return idx >= 0 && idx < N_ZONES ? tz_zones[idx].rule : 0; }

int tz_find(const char *name)
{
  for (int i = 0; i < N_ALIASES; i++)
    if (streq_ci(name, aliases[i].alias))
      name = aliases[i].zone;
  for (int i = 0; i < N_ZONES; i++)
    if (streq_ci(name, tz_zones[i].name))
      return i;
  return -1;
}

int tz_load(struct tz *z, int idx)
{
  const char *rule = tz_rule_at(idx);
  return rule ? tz_parse(z, rule) : -1;
}

static int put(char *buf, int cap, int n, char c)
{
  if (n < cap - 1)
    buf[n] = c;
  return n + 1;
}

static int finish(char *buf, int cap, int n)
{
  if (cap <= 0)
    return 0;
  n = n < cap - 1 ? n : cap - 1;
  buf[n] = 0;
  return n;
}

int tz_format_offset(char *buf, int cap, int off)
{
  int n = 0, a = off < 0 ? -off : off;
  for (const char *s = "UTC"; *s; s++)
    n = put(buf, cap, n, *s);
  n = put(buf, cap, n, off < 0 ? '-' : '+');
  n = put(buf, cap, n, (char)('0' + a / 36000));
  n = put(buf, cap, n, (char)('0' + a / 3600 % 10));
  n = put(buf, cap, n, ':');
  n = put(buf, cap, n, (char)('0' + a % 3600 / 600));
  n = put(buf, cap, n, (char)('0' + a % 600 / 60));
  return finish(buf, cap, n);
}

int tz_city(const char *name, char *buf, int cap)
{
  const char *city = name;
  for (const char *s = name; *s; s++)
    if (*s == '/')
      city = s + 1;
  int n = 0;
  for (; *city; city++)
    n = put(buf, cap, n, *city == '_' ? ' ' : *city);
  return finish(buf, cap, n);
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int config_result(struct tz *out, char *name_out, int cap, int idx, int ret)
{
  tz_load(out, idx);
  if (name_out && cap > 0) {
    const char *n = tz_name_at(idx);
    int len = 0;
    while (n[len] && len < cap - 1) {
      name_out[len] = n[len];
      len++;
    }
    name_out[len] = 0;
  }
  return ret;
}

int tz_load_config(struct tz *out, char *name_out, int cap)
{
  char buf[64];
  int len = readfile(TZ_CONFIG_PATH, buf, (int)sizeof(buf) - 1);
  int a = 0, idx = -1;
  if (len > 0) {
    while (a < len && is_space(buf[a]))
      a++;
    while (len > a && is_space(buf[len - 1]))
      len--;
    buf[len] = 0;
    idx = tz_find(buf + a);
  }
  if (idx < 0)
    return config_result(out, name_out, cap, tz_find(TZ_DEFAULT_ZONE), -1);
  return config_result(out, name_out, cap, idx, 0);
}

int tz_save_config(const char *name)
{
  int idx = tz_find(name);
  if (idx < 0)
    return -1;
  char buf[64];
  const char *n = tz_name_at(idx);
  int len = 0;
  while (n[len] && len < (int)sizeof(buf) - 2) {
    buf[len] = n[len];
    len++;
  }
  buf[len++] = '\n';
  return writefile(TZ_CONFIG_PATH, buf, len);
}
