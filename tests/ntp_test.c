/* Host test for the NTP client math (user/lib/ntp/ntp.c) and the kernel slew arithmetic (kernel/slew.h). */
#define OWOS_SUITE_TEST
#include "../kernel/slew.h"
#include "ntp.h"
#include "test.h"
#include <string.h>

#define MS 1000ll

static struct ntp_packet reply_for(struct ntp_ts sent)
{
  struct ntp_packet p;
  memset(&p, 0, sizeof p);
  p.li = 0;
  p.vn = 4;
  p.mode = 4;
  p.stratum = 2;
  p.orig = sent;
  p.rx = ntp_ts_from_unix_us(1700000000ll * 1000000);
  p.tx = ntp_ts_from_unix_us(1700000000ll * 1000000 + 100);
  return p;
}

static void test_timestamp_conversion_round_trips(void)
{
  struct ntp_ts ts = ntp_ts_from_unix_us(1700000000ll * 1000000 + 250000);
  expect(ts.sec == 1700000000u + NTP_UNIX_DELTA);
  expect(ts.frac == 0x40000000u);
  expect(ntp_ts_to_unix_us(ts) == 1700000000ll * 1000000 + 250000);
  for (int64_t us = 1700000000ll * 1000000; us < 1700000000ll * 1000000 + 3000000; us += 99991) {
    int64_t back = ntp_ts_to_unix_us(ntp_ts_from_unix_us(us));
    expect(back <= us && us - back <= 1);
  }
}

static void test_era_1_timestamps_after(void)
{
  struct ntp_ts era1 = {100, 0};
  expect(ntp_ts_to_unix_us(era1) == ((1ll << 32) + 100 - NTP_UNIX_DELTA) * 1000000);
}

static void test_offset_and_delay_follow(void)
{
  int64_t t1 = 1000 * MS, t2 = 1510 * MS, t3 = 1512 * MS, t4 = 1022 * MS;
  expect(ntp_offset_us(t1, t2, t3, t4) == 500 * MS);
  expect(ntp_delay_us(t1, t2, t3, t4) == 20 * MS);
  expect(ntp_offset_us(1000 * MS, 490 * MS, 492 * MS, 1022 * MS) == -520 * MS);
  expect(ntp_offset_us(0, 5 * MS, 5 * MS, 10 * MS) == 0);
}

static void test_request_packet_carries_version(void)
{
  uint8_t pkt[NTP_PKT_LEN];
  struct ntp_ts sent = {0xAABBCCDDu, 0x11223344u};
  ntp_build_request(pkt, sent);
  expect(pkt[0] == 0x23);
  expect(pkt[40] == 0xAA && pkt[43] == 0xDD && pkt[44] == 0x11 && pkt[47] == 0x44);
  struct ntp_packet parsed;
  expect(ntp_parse(pkt, 47, &parsed) == NTP_BAD_LEN);
  expect(ntp_parse(pkt, NTP_PKT_LEN, &parsed) == NTP_OK);
  expect(parsed.mode == 3 && parsed.vn == 4 && ntp_ts_equal(parsed.tx, sent));
}

static void test_validation_accepts_a_good(void)
{
  struct ntp_ts origin = ntp_ts_from_unix_us(1700000000ll * 1000000 - 5000);
  struct ntp_packet p = reply_for(origin);
  expect(ntp_validate(&p, origin) == NTP_OK);

  p = reply_for(origin);
  p.orig.frac ^= 1;
  expect(ntp_validate(&p, origin) == NTP_BAD_ORIGIN);
  p = reply_for(origin);
  p.stratum = 0;
  expect(ntp_validate(&p, origin) == NTP_KOD);
  p = reply_for(origin);
  p.stratum = 16;
  expect(ntp_validate(&p, origin) == NTP_BAD_STRATUM);
  p = reply_for(origin);
  p.stratum = 15;
  expect(ntp_validate(&p, origin) == NTP_OK);
  p = reply_for(origin);
  p.li = 3;
  expect(ntp_validate(&p, origin) == NTP_UNSYNC);
  p = reply_for(origin);
  p.mode = 3;
  expect(ntp_validate(&p, origin) == NTP_BAD_MODE);
  p = reply_for(origin);
  p.vn = 2;
  expect(ntp_validate(&p, origin) == NTP_BAD_VERSION);
  p = reply_for(origin);
  p.tx = (struct ntp_ts){0, 0};
  expect(ntp_validate(&p, origin) == NTP_BAD_TIME);
  p = reply_for(origin);
  p.rx = ntp_ts_from_unix_us(100ll * 1000000);
  p.tx = p.rx;
  expect(ntp_validate(&p, origin) == NTP_BAD_TIME);
  p = reply_for(origin);
  p.tx = ntp_ts_from_unix_us(1699999999ll * 1000000);
  expect(ntp_validate(&p, origin) == NTP_BAD_TIME);
  p = reply_for(origin);
  p.root_delay = 40u << 16;
  expect(ntp_validate(&p, origin) == NTP_BAD_ROOT);
}

static void test_filter_drops_high_delay(void)
{
  struct ntp_sample s[4] = {{10 * MS, 20 * MS}, {12 * MS, 22 * MS}, {900 * MS, 300 * MS}, {14 * MS, 24 * MS}};
  struct ntp_sample out;
  expect(ntp_filter(s, 4, &out));
  expect(out.offset_us == 12 * MS);
  expect(out.delay_us == 20 * MS);
  struct ntp_sample even[4] = {{10 * MS, 20 * MS}, {20 * MS, 21 * MS}, {30 * MS, 22 * MS}, {40 * MS, 23 * MS}};
  expect(ntp_filter(even, 4, &out));
  expect(out.offset_us == 25 * MS);
  struct ntp_sample one = {-7 * MS, 0};
  expect(ntp_filter(&one, 1, &out) && out.offset_us == -7 * MS);
  struct ntp_sample neg[2] = {{5 * MS, -2 * MS}, {80 * MS, 30 * MS}};
  expect(ntp_filter(neg, 2, &out) && out.offset_us == 5 * MS);
  expect(!ntp_filter(s, 0, &out));
}

static void test_best_server_is_the(void)
{
  struct ntp_sample servers[3] = {{0, 30 * MS}, {0, 8 * MS}, {0, 15 * MS}};
  expect(ntp_best(servers, 3) == 1);
  expect(ntp_best(servers, 0) == -1);
}

static void test_step_above_128_ms(void)
{
  expect(!ntp_should_step(128 * MS, false));
  expect(ntp_should_step(128 * MS + 1, false));
  expect(ntp_should_step(-129 * MS, false));
  expect(!ntp_should_step(-50 * MS, false));
  expect(ntp_should_step(1, true));
}

static void test_poll_interval_backs_off(void)
{
  int poll = NTP_POLL_MIN;
  poll = ntp_next_poll(poll, 2 * MS);
  expect(poll == 128);
  for (int i = 0; i < 10; i++)
    poll = ntp_next_poll(poll, -3 * MS);
  expect(poll == NTP_POLL_MAX);
  expect(ntp_next_poll(256, 10 * MS) == 256);
  expect(ntp_next_poll(0, 0) == NTP_POLL_MIN);
}

static void test_retry_delay_grows_to(void)
{
  expect(ntp_retry_delay(0) == 4);
  expect(ntp_retry_delay(1) == 8);
  expect(ntp_retry_delay(3) == 32);
  expect(ntp_retry_delay(4) == NTP_POLL_MIN);
  expect(ntp_retry_delay(100) == NTP_POLL_MIN);
}

static void test_slew_applies_at_most(void)
{
  struct slew sl = {200 * MS, 0};
  expect(slew_step(&sl, 1000000) == 500);
  expect(sl.remaining == 200 * MS - 500);
  struct slew neg_sl = {-200 * MS, 0};
  expect(slew_step(&neg_sl, 2000000) == -1000);
  struct slew small = {300, 0};
  expect(slew_step(&small, 1000000) == 300);
  expect(small.remaining == 0);
  expect(slew_step(&small, 1000000) == 0);
}

static void test_slew_keeps_sub_microsecond(void)
{
  struct slew tiny = {1000, 0};
  int64_t applied = 0;
  for (int i = 0; i < 10000; i++)
    applied += slew_step(&tiny, 100);
  expect(applied == 500);
  expect(slew_step(&tiny, -5) == 0);
}

int main(void)
{
  test("timestamp conversion round-trips with sub-second fractions", test_timestamp_conversion_round_trips);
  test("era 1 timestamps (after 2036) decode past 2^32", test_era_1_timestamps_after);
  test("offset and delay follow RFC 5905", test_offset_and_delay_follow);
  test("request packet carries version 4, client mode and the transmit timestamp", test_request_packet_carries_version);
  test("validation accepts a good reply and rejects the bad ones", test_validation_accepts_a_good);
  test("filter drops high-delay samples and takes the median offset", test_filter_drops_high_delay);
  test("best server is the lowest delay", test_best_server_is_the);
  test("step above 128 ms or on the first sync, slew otherwise", test_step_above_128_ms);
  test("poll interval backs off to 1024 s when stable and holds otherwise", test_poll_interval_backs_off);
  test("retry delay grows to the minimum poll after failures", test_retry_delay_grows_to);
  test("slew applies at most 500 ppm and converges exactly", test_slew_applies_at_most);
  test("slew keeps sub-microsecond budget across tiny steps", test_slew_keeps_sub_microsecond);

  return 0;
}
