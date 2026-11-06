/* ntp.c: NTPv4 client arithmetic (see ntp.h). */

#include "ntp.h"

#define US 1000000ll
#define MIN_UNIX 946684800ll  /* 2000-01-01 */
#define MAX_UNIX 4102444800ll /* 2100-01-01 */
#define MAX_ROOT_DIST (16u << 16)
#define NTP_ERA_SECS (1ll << 32)

static int64_t i64_abs(int64_t v) { return v < 0 ? -v : v; }

static uint32_t get32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static struct ntp_ts get_ts(const uint8_t *p) { return (struct ntp_ts){get32(p), get32(p + 4)}; }

static void put_ts(uint8_t *p, struct ntp_ts ts)
{
  put32(p, ts.sec);
  put32(p + 4, ts.frac);
}

static bool ts_zero(struct ntp_ts ts) { return ts.sec == 0 && ts.frac == 0; }

/* Seconds below the 1970 delta are read as era 1 (2036 onwards). */
int64_t ntp_ts_to_unix_us(struct ntp_ts ts)
{
  int64_t secs = (int64_t)ts.sec - NTP_UNIX_DELTA;
  if (ts.sec < NTP_UNIX_DELTA)
    secs += NTP_ERA_SECS;
  return secs * US + (int64_t)(((uint64_t)ts.frac * (uint64_t)US) >> 32);
}

struct ntp_ts ntp_ts_from_unix_us(int64_t unix_us)
{
  int64_t secs = unix_us / US, rem = unix_us % US;
  if (rem < 0) {
    rem += US;
    secs--;
  }
  return (struct ntp_ts){(uint32_t)(secs + NTP_UNIX_DELTA), (uint32_t)(((uint64_t)rem << 32) / (uint64_t)US)};
}

bool ntp_ts_equal(struct ntp_ts a, struct ntp_ts b) { return a.sec == b.sec && a.frac == b.frac; }

void ntp_build_request(uint8_t pkt[NTP_PKT_LEN], struct ntp_ts xmit)
{
  for (int i = 0; i < NTP_PKT_LEN; i++)
    pkt[i] = 0;
  pkt[0] = (0 << 6) | (4 << 3) | 3; /* LI 0, VN 4, mode 3 (client) */
  put_ts(pkt + 40, xmit);
}

enum ntp_status ntp_parse(const uint8_t *buf, int len, struct ntp_packet *out)
{
  if (len < NTP_PKT_LEN)
    return NTP_BAD_LEN;
  out->li = buf[0] >> 6;
  out->vn = (buf[0] >> 3) & 7;
  out->mode = buf[0] & 7;
  out->stratum = buf[1];
  out->poll = (int8_t)buf[2];
  out->precision = (int8_t)buf[3];
  out->root_delay = get32(buf + 4);
  out->root_disp = get32(buf + 8);
  out->refid = get32(buf + 12);
  out->ref = get_ts(buf + 16);
  out->orig = get_ts(buf + 24);
  out->rx = get_ts(buf + 32);
  out->tx = get_ts(buf + 40);
  return NTP_OK;
}

static bool plausible(struct ntp_ts ts)
{
  if (ts_zero(ts))
    return false;
  int64_t secs = ntp_ts_to_unix_us(ts) / US;
  return secs >= MIN_UNIX && secs < MAX_UNIX;
}

enum ntp_status ntp_validate(const struct ntp_packet *p, struct ntp_ts sent)
{
  if (p->vn < 3 || p->vn > 4)
    return NTP_BAD_VERSION;
  if (p->mode != 4)
    return NTP_BAD_MODE;
  if (!ntp_ts_equal(p->orig, sent))
    return NTP_BAD_ORIGIN;
  if (p->stratum == 0)
    return NTP_KOD;
  if (p->stratum > 15)
    return NTP_BAD_STRATUM;
  if (p->li == 3)
    return NTP_UNSYNC;
  if (!plausible(p->rx) || !plausible(p->tx) || ntp_ts_to_unix_us(p->tx) < ntp_ts_to_unix_us(p->rx))
    return NTP_BAD_TIME;
  if (((uint64_t)(p->root_delay >> 1) + p->root_disp) >= MAX_ROOT_DIST)
    return NTP_BAD_ROOT;
  return NTP_OK;
}

const char *ntp_status_str(enum ntp_status s)
{
  switch (s) {
  case NTP_OK:
    return "ok";
  case NTP_BAD_LEN:
    return "short reply";
  case NTP_BAD_VERSION:
    return "bad version";
  case NTP_BAD_MODE:
    return "not a server reply";
  case NTP_KOD:
    return "kiss-o-death";
  case NTP_BAD_STRATUM:
    return "bad stratum";
  case NTP_UNSYNC:
    return "server unsynchronized";
  case NTP_BAD_ORIGIN:
    return "origin mismatch";
  case NTP_BAD_TIME:
    return "implausible timestamps";
  case NTP_BAD_ROOT:
    return "root distance too large";
  }
  return "error";
}

int64_t ntp_offset_us(int64_t t1, int64_t t2, int64_t t3, int64_t t4) { return ((t2 - t1) + (t3 - t4)) / 2; }

int64_t ntp_delay_us(int64_t t1, int64_t t2, int64_t t3, int64_t t4) { return (t4 - t1) - (t3 - t2); }

/* Offset of rank `rank` (0-based) among the samples whose delay passes the gate. */
static int64_t kth_offset(const struct ntp_sample *s, int n, int64_t max_delay, int rank)
{
  int64_t pick = 0;
  for (int i = 0; i < n; i++) {
    if (s[i].delay_us > max_delay)
      continue;
    int lt = 0, le = 0;
    for (int j = 0; j < n; j++) {
      if (s[j].delay_us > max_delay)
        continue;
      lt += s[j].offset_us < s[i].offset_us;
      le += s[j].offset_us <= s[i].offset_us;
    }
    if (lt <= rank && rank < le)
      pick = s[i].offset_us;
  }
  return pick;
}

bool ntp_filter(const struct ntp_sample *s, int n, struct ntp_sample *out)
{
  if (n <= 0)
    return false;
  int64_t min_delay = s[0].delay_us;
  for (int i = 1; i < n; i++)
    if (s[i].delay_us < min_delay)
      min_delay = s[i].delay_us;

  int64_t gate = min_delay + (min_delay > 1000 ? min_delay : 1000), k = 0;
  for (int i = 0; i < n; i++)
    k += s[i].delay_us <= gate;
  int mid = (int)(k / 2);
  out->offset_us =
      k % 2 ? kth_offset(s, n, gate, mid) : (kth_offset(s, n, gate, mid - 1) + kth_offset(s, n, gate, mid)) / 2;
  out->delay_us = min_delay;
  return true;
}

int ntp_best(const struct ntp_sample *s, int n)
{
  int best = -1;
  for (int i = 0; i < n; i++)
    if (best < 0 || s[i].delay_us < s[best].delay_us)
      best = i;
  return best;
}

bool ntp_should_step(int64_t offset_us, bool first_sync) { return first_sync || i64_abs(offset_us) > NTP_STEP_US; }

int ntp_next_poll(int poll, int64_t offset_us)
{
  if (i64_abs(offset_us) >= NTP_STABLE_US)
    return poll < NTP_POLL_MIN ? NTP_POLL_MIN : poll;
  if (poll < NTP_POLL_MIN)
    return NTP_POLL_MIN;
  return poll * 2 > NTP_POLL_MAX ? NTP_POLL_MAX : poll * 2;
}

int ntp_retry_delay(int failures)
{
  int d = 4;
  for (int i = 0; i < failures && d < NTP_POLL_MIN; i++)
    d *= 2;
  return d > NTP_POLL_MIN ? NTP_POLL_MIN : d;
}
