/* ntp.h: NTPv4 (RFC 5905) client arithmetic: timestamps, packet codec and validation, offset/delay
 * math, sample filtering and poll scheduling. Pure computation, no I/O. */
#ifndef OWOS_NTP_H
#define OWOS_NTP_H

#include <stdbool.h>
#include <stdint.h>

#define NTP_PORT 123
#define NTP_PKT_LEN 48
#define NTP_UNIX_DELTA 2208988800u /* seconds from 1900-01-01 to 1970-01-01 */
#define NTP_STEP_US 128000         /* step above this offset, slew below */
#define NTP_STABLE_US 10000        /* offsets under this let the poll interval back off */
#define NTP_POLL_MIN 64
#define NTP_POLL_MAX 1024

struct ntp_ts {
  uint32_t sec;  /* seconds since 1900 */
  uint32_t frac; /* units of 2^-32 s */
};

struct ntp_packet {
  uint8_t li, vn, mode, stratum;
  int8_t poll, precision;
  uint32_t root_delay, root_disp; /* 16.16 fixed point seconds */
  uint32_t refid;
  struct ntp_ts ref, orig, rx, tx;
};

enum ntp_status {
  NTP_OK,
  NTP_BAD_LEN,
  NTP_BAD_VERSION,
  NTP_BAD_MODE,
  NTP_KOD,         /* stratum 0: kiss-o-death (refid holds the code) */
  NTP_BAD_STRATUM, /* above 15 */
  NTP_UNSYNC,      /* leap indicator 3 */
  NTP_BAD_ORIGIN,  /* origin does not echo our transmit timestamp */
  NTP_BAD_TIME,    /* zero, out-of-range or out-of-order timestamps */
  NTP_BAD_ROOT,    /* root distance too large */
};

struct ntp_sample {
  int64_t offset_us; /* server minus local */
  int64_t delay_us;
};

int64_t ntp_ts_to_unix_us(struct ntp_ts ts);
struct ntp_ts ntp_ts_from_unix_us(int64_t unix_us);
bool ntp_ts_equal(struct ntp_ts a, struct ntp_ts b);

void ntp_build_request(uint8_t pkt[NTP_PKT_LEN], struct ntp_ts xmit);
enum ntp_status ntp_parse(const uint8_t *buf, int len, struct ntp_packet *out);
/* Checks a parsed reply against the transmit timestamp we sent. */
enum ntp_status ntp_validate(const struct ntp_packet *p, struct ntp_ts sent);
const char *ntp_status_str(enum ntp_status s);

/* t1 client send, t2 server receive, t3 server send, t4 client receive, all unix microseconds. */
int64_t ntp_offset_us(int64_t t1, int64_t t2, int64_t t3, int64_t t4);
int64_t ntp_delay_us(int64_t t1, int64_t t2, int64_t t3, int64_t t4);

/* Drops samples with delay more than max(min delay, 1 ms) above the minimum, returns the median offset of the rest and
 * their minimum delay. False when n is 0. */
bool ntp_filter(const struct ntp_sample *s, int n, struct ntp_sample *out);
/* Index of the lowest-delay entry, or -1 when n is 0. */
int ntp_best(const struct ntp_sample *s, int n);

bool ntp_should_step(int64_t offset_us, bool first_sync);
int ntp_next_poll(int poll, int64_t offset_us); /* after a successful sync */
int ntp_retry_delay(int failures);              /* seconds to wait after the nth consecutive failure */

#endif
