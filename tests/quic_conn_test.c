/* Host test for the per-connection state of quic.c: stream slots, server limits from transport parameters, MAX_DATA
 * credit and close detection. quic.c is included so the test can drive its frame handling directly; the socket
 * calls are fakes. */
#define OWOS_SUITE_TEST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define msec quict_msec
#define msleep quict_msleep
#define certs_init quict_certs_init
#define certs_take quict_certs_take
#define certs_check_cv quict_certs_check_cv
#define net_dial quict_dial
#define net_send quict_send
#define net_recv quict_recv
#define net_recv_nb quict_recv_nb
#define net_close quict_close
#define rng_bytes quict_rng

void rng_bytes(void *p, int n);
unsigned msec(void);
void msleep(unsigned ms);

#include "certs.h"
#include "test.h"

void certs_init(struct certs *c);
void certs_take(struct certs *c, const char *host, const uint8_t *body, uint32_t bl, int tls13);
void certs_check_cv(struct certs *c, const uint8_t th[32], const uint8_t *body, uint32_t bl);

#include "../user/lib/curl/quic.c"

void rng_bytes(void *p, int n) { memset(p, 7, (size_t)n); }
int net_dial(const char *host, int port, int proto) { return (void)host, (void)port, (void)proto, 5; }
static unsigned g_now = 1000;
unsigned msec(void) { return g_now; }
void msleep(unsigned ms) { g_now += ms; }
void certs_init(struct certs *c) { c->verify = X509_E_UNTRUSTED; }
void certs_take(struct certs *c, const char *host, const uint8_t *body, uint32_t bl, int tls13)
{
  (void)c, (void)host, (void)body, (void)bl, (void)tls13;
}
void certs_check_cv(struct certs *c, const uint8_t th[32], const uint8_t *body, uint32_t bl)
{
  (void)c, (void)th, (void)body, (void)bl;
}
int net_recv(int h, void *buf, int max) { return (void)h, (void)buf, (void)max, 0; }
int net_recv_nb(int h, void *buf, int max) { return (void)h, (void)buf, (void)max, 0; }
int net_close(int h) { return (void)h, 0; }

/* Every datagram the code under test sends, newest last. */
static int g_sent;
static uint8_t g_dg[64][1500];
static int g_dglen[64];

int net_send(int h, const void *buf, int len)
{
  (void)h;
  if (g_sent < 64) {
    memcpy(g_dg[g_sent], buf, (size_t)len);
    g_dglen[g_sent] = len;
  }
  g_sent++;
  return len;
}

static struct quic *conn(void)
{
  struct quic *Q = calloc(1, sizeof *Q);
  Q->rx_limit = 0x1000000;
  Q->peer_max_bidi = 100;
  Q->peer_max_data = 1 << 20;
  Q->peer_stream_data = 1 << 18;
  struct space *s = &Q->sp[S_APP];
  s->hk = 1;
  for (int i = 0; i < 16; i++)
    s->ck[i] = s->sk[i] = s->ch[i] = s->sh[i] = (uint8_t)(i + 1);
  Q->dcid_len = 8;
  Q->scid_len = 8;
  g_sent = 0;
  g_now = 1000;
  return Q;
}

/* A STREAM frame with offset, length and optional FIN. */
static int stream_frame(uint8_t *f, uint64_t sid, uint64_t off, const char *data, int len, int fin)
{
  int n = 0;
  f[n++] = (uint8_t)(0x08 | 0x04 | 0x02 | (fin ? 1 : 0));
  n += putvar(f + n, sid);
  n += putvar(f + n, off);
  n += putvar(f + n, (uint64_t)len);
  memcpy(f + n, data, (size_t)len);
  return n + len;
}

static void finished_streams_free_their_slot(void)
{
  struct quic *Q = conn();
  uint8_t f[64];
  char buf[16];
  int fin;
  for (int round = 0; round < 3 * QS_MAX; round++) { /* far more requests than there are slots */
    uint64_t sid;
    expect_eq(quic_open(Q, &sid), 0);
    expect_eq(sid, 4u * (unsigned)round);
    get_stream(Q, sid); /* a request stream gets its slot when the request is sent */
    int n = stream_frame(f, sid, 0, "hello", 5, 1);
    process_frames(Q, S_APP, f, (uint32_t)n, 0);
    expect_eq(quic_read(Q, sid, buf, sizeof buf, &fin), 5);
    expect_eq(fin, 1);
    quic_stream_done(Q, sid);
  }
  free(Q);
}

static void server_limits_come_from_transport_parameters(void)
{
  struct quic *Q = calloc(1, sizeof *Q);
  /* EncryptedExtensions body: extension list holding quic_transport_parameters (0x39) with idle timeout 30 s,
   * initial_max_data 1000, bidi_remote stream data 500 and 3 bidi streams, and one ignored parameter. */
  uint8_t tp[64];
  int n = 0;
  uint64_t ids[] = {0x01, 0x04, 0x06, 0x08, 0x0f}, vals[] = {30000, 1000, 500, 3, 0};
  for (int i = 0; i < 4; i++) {
    n += putvar(tp + n, ids[i]);
    n += putvar(tp + n, (uint64_t)varsize(vals[i]));
    n += putvar(tp + n, vals[i]);
  }
  n += putvar(tp + n, 0x0f);
  n += putvar(tp + n, 4);
  memcpy(tp + n, "cid!", 4);
  n += 4;
  uint8_t ee[80];
  int e = 2;
  ee[e++] = 0x00, ee[e++] = 0x39;
  ee[e++] = 0, ee[e++] = (uint8_t)n;
  memcpy(ee + e, tp, (size_t)n);
  e += n;
  ee[0] = 0, ee[1] = (uint8_t)(e - 2);
  parse_server_tp(Q, ee, (uint32_t)e);
  expect_eq(Q->peer_idle_ms, 30000);
  expect_eq(Q->peer_max_data, 1000);
  expect_eq(Q->peer_stream_data, 500);
  expect_eq(Q->peer_max_bidi, 3);
  expect_eq(quic_idle_ms(Q), 30000);
  expect_eq(quic_streams_left(Q), 3);

  uint64_t sid;
  for (int i = 0; i < 3; i++)
    expect_eq(quic_open(Q, &sid), 0);
  expect_eq(quic_open(Q, &sid), -1); /* out of streams until the server raises the limit */
  uint8_t max_streams[3] = {0x12, 0x40, 0x0a};
  Q->sp[S_APP].hk = 1;
  process_frames(Q, S_APP, max_streams, 3, 0);
  expect_eq(Q->peer_max_bidi, 10);
  expect_eq(quic_open(Q, &sid), 0);
  free(Q);
}

static void sending_obeys_the_servers_limits(void)
{
  struct quic *Q = conn();
  char big[600];
  memset(big, 'x', sizeof big);
  Q->peer_stream_data = 500;
  expect_eq(quic_send(Q, 0, big, 600, 0), -2); /* over the stream limit */
  expect_eq(quic_send(Q, 0, big, 400, 0), 0);
  expect_eq(quic_send(Q, 0, big, 200, 0), -2); /* 400 + 200 > 500 */
  Q->peer_max_data = 450;
  expect_eq(quic_send(Q, 4, big, 100, 0), -2); /* connection limit: 400 sent + 100 > 450 */
  uint8_t max_data[5] = {0x10, 0x80, 0x00, 0x20, 0x00};
  process_frames(Q, S_APP, max_data, 5, 0);
  expect_eq(Q->peer_max_data, 0x2000);
  expect_eq(quic_send(Q, 4, big, 100, 0), 0);
  free(Q);
}

static void receive_credit_is_topped_up(void)
{
  struct quic *Q = conn();
  Q->rx_total = 0x900000;
  expect(Q->rx_total + 0x800000 > Q->rx_limit);
  grant_flow(Q);
  expect_eq(Q->rx_limit, 0x900000 + 0x1000000);
  expect_eq(g_sent, 1);
  grant_flow(Q); /* plenty left now */
  expect_eq(g_sent, 1);
  free(Q);
}

static void early_receive_credit_is_not_wasted(void)
{
  struct quic *Q = conn();
  Q->rx_total = 0x100000;
  grant_flow(Q);
  expect_eq(g_sent, 0);
  expect_eq(Q->rx_limit, 0x1000000);
  free(Q);
}

static void connection_close_is_noticed(void)
{
  struct quic *Q = conn();
  expect_eq(quic_peer_closed(Q), 0);
  uint8_t close[4] = {0x1c, 0, 0, 0};
  process_frames(Q, S_APP, close, 4, 0);
  expect_eq(quic_peer_closed(Q), 1);
  free(Q);
}

/* ---- loss recovery */

/* Decrypt a short-header datagram we sent (the test gives the "server" the same keys); returns the frames' length. */
static int open_sent(struct quic *Q, const uint8_t *d, int n, uint8_t *pt, uint64_t *pn_out)
{
  struct space *s = &Q->sp[S_APP];
  uint32_t pn_off = 1 + (uint32_t)Q->dcid_len;
  uint8_t mask[16], hdr[32], nonce[12];
  hp_compute(s->ch, d + pn_off + 4, mask);
  memcpy(hdr, d, pn_off + 4);
  hdr[0] ^= mask[0] & 0x1f;
  uint64_t pn = 0;
  for (int i = 0; i < 4; i++) {
    hdr[pn_off + i] ^= mask[1 + i];
    pn = (pn << 8) | hdr[pn_off + i];
  }
  memcpy(nonce, s->ci, 12);
  for (int i = 0; i < 8; i++)
    nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));
  int len = n - (int)pn_off - 4 - 16;
  expect(aes128_gcm_open(s->ck, nonce, hdr, pn_off + 4, d + pn_off + 4, (uint32_t)len, pt, d + n - 16) == 0);
  *pn_out = pn;
  return len;
}

/* Hand the connection a packet from the "server": frames in packet number `pn`. */
static void deliver(struct quic *Q, uint64_t pn, const uint8_t *frames, uint32_t len)
{
  struct space *s = &Q->sp[S_APP];
  uint64_t keep = s->spn, ignored;
  uint8_t saved[2048];
  s->spn = pn;
  int before = g_sent;
  expect_eq(emit_packet(Q, S_APP, frames, len, &ignored), 0);
  s->spn = keep;
  memcpy(saved, g_dg[before], (size_t)g_dglen[before]);
  int n = g_dglen[before];
  g_sent = before; /* that datagram was never "sent" */
  process_datagram(Q, saved, (uint32_t)n);
}

static void send_data(struct quic *Q, uint8_t tag)
{
  uint8_t f[8] = {0x0e, 0x00, 0x00, 0x03, tag, tag, tag, 0}; /* STREAM id 0, OFF+LEN, 3 bytes: data for the peer */
  expect_eq(emit(Q, S_APP, f, 7, 1), 0);
}

static int packets_in_flight(struct quic *Q)
{
  int n = 0;
  for (int i = 0; i < SENT_MAX; i++)
    n += Q->sp[S_APP].sent[i].frames != 0;
  return n;
}

static void received_ranges_merge(void)
{
  struct quic *Q = conn();
  struct space *s = &Q->sp[S_APP];
  uint64_t pns[] = {5, 0, 1, 9, 2, 6, 5, 4};
  for (unsigned i = 0; i < sizeof pns / sizeof pns[0]; i++)
    rr_add(s, pns[i]);
  /* {0,1,2}, {4,5,6}, {9} */
  expect_eq(s->nrr, 3);
  expect(s->rr[0].lo == 9 && s->rr[0].hi == 9);
  expect(s->rr[1].lo == 4 && s->rr[1].hi == 6);
  expect(s->rr[2].lo == 0 && s->rr[2].hi == 2);
  rr_add(s, 3); /* bridges the two lower ranges */
  expect_eq(s->nrr, 2);
  expect(s->rr[1].lo == 0 && s->rr[1].hi == 6);
  rr_add(s, 8);
  rr_add(s, 7);
  expect_eq(s->nrr, 1);
  expect(s->rr[0].lo == 0 && s->rr[0].hi == 9);
  expect(rr_has(s, 7) && !rr_has(s, 10));
  free(Q);
}

static void ack_frame_reports_every_gap(void)
{
  struct quic *Q = conn();
  struct space *s = &Q->sp[S_APP];
  uint64_t pns[] = {0, 1, 2, 5, 6, 9};
  for (unsigned i = 0; i < sizeof pns / sizeof pns[0]; i++)
    rr_add(s, pns[i]);
  send_ack(Q, S_APP);
  uint8_t pt[64];
  uint64_t pn;
  int n = open_sent(Q, g_dg[0], g_dglen[0], pt, &pn);
  const uint8_t want[] = {0x02, 9, 0, 2, 0, /* largest 9, delay 0, two more ranges, first range [9,9] */
                          1,    1,          /* gap 1 (7,8), range [5,6] */
                          1,    2};         /* gap 1 (3,4), range [0,2] */
  expect_eq(n, (int)sizeof want);
  expect(memcmp(pt, want, sizeof want) == 0);
  expect_eq(packets_in_flight(Q), 0); /* an ACK is not itself retransmitted */
  free(Q);
}

static void data_past_a_gap_is_dropped_unacknowledged(void)
{
  struct quic *Q = conn();
  get_stream(Q, 0);
  uint8_t f[32];
  int n = stream_frame(f, 0, 10, "later", 5, 0); /* bytes 10.. while 0..9 are missing */
  deliver(Q, 3, f, (uint32_t)n);
  expect_eq(Q->sp[S_APP].nrr, 0);
  expect_eq(Q->sp[S_APP].ackp, 0);
  char buf[16];
  expect_eq(quic_read(Q, 0, buf, sizeof buf, 0), 0);

  n = stream_frame(f, 0, 0, "0123456789", 10, 0);
  deliver(Q, 4, f, (uint32_t)n); /* the server's retransmission, in order */
  expect_eq(Q->sp[S_APP].nrr, 1);
  expect_eq(Q->sp[S_APP].ackp, 1);
  expect_eq(quic_read(Q, 0, buf, sizeof buf, 0), 10);
  n = stream_frame(f, 0, 10, "later", 5, 0);
  deliver(Q, 5, f, (uint32_t)n);
  expect_eq(quic_read(Q, 0, buf, sizeof buf, 0), 5);
  expect(memcmp(buf, "later", 5) == 0);
  expect(Q->sp[S_APP].rr[0].lo == 4 && Q->sp[S_APP].rr[0].hi == 5);
  free(Q);
}

static void a_duplicate_is_acknowledged_again_but_not_replayed(void)
{
  struct quic *Q = conn();
  get_stream(Q, 0);
  uint8_t f[32];
  int n = stream_frame(f, 0, 0, "abc", 3, 0);
  deliver(Q, 7, f, (uint32_t)n);
  Q->sp[S_APP].ackp = 0;
  deliver(Q, 7, f, (uint32_t)n);
  expect_eq(Q->sp[S_APP].ackp, 1);
  expect_eq(Q->rx_total, 3);
  free(Q);
}

static void acked_packets_leave_flight_and_time_the_round_trip(void)
{
  struct quic *Q = conn();
  send_data(Q, 1);
  expect_eq(packets_in_flight(Q), 1);
  g_now += 100;
  uint8_t ack[] = {0x02, 0, 0, 0, 0}; /* packet 0 */
  deliver(Q, 0, ack, sizeof ack);
  expect_eq(packets_in_flight(Q), 0);
  expect_eq(Q->srtt, 100);
  expect_eq(Q->rttvar, 50);
  g_now += 40;
  send_data(Q, 2);
  g_now += 140;
  uint8_t ack1[] = {0x02, 1, 0, 0, 0};
  deliver(Q, 1, ack1, sizeof ack1);
  expect_eq(Q->latest_rtt, 140);
  expect_eq(Q->srtt, (7 * 100 + 140) / 8);
  free(Q);
}

static void packets_the_ack_skips_are_resent(void)
{
  struct quic *Q = conn();
  for (uint8_t i = 0; i < 5; i++)
    send_data(Q, i); /* packets 0..4 */
  expect_eq(g_sent, 5);
  g_now += 50;
  uint8_t ack[] = {0x02, 4, 0, 0, 1}; /* 3 and 4 arrived */
  deliver(Q, 0, ack, sizeof ack);
  /* 0 and 1 are three or more behind the largest ACKed: resent as new packets; 2 is not yet */
  expect_eq(g_sent, 7);
  uint8_t pt[64];
  uint64_t pn;
  for (int k = 0; k < 2; k++) {
    int n = open_sent(Q, g_dg[5 + k], g_dglen[5 + k], pt, &pn);
    expect_eq(pn, 5 + k);
    expect_eq(n, 7);
    expect_eq(pt[4], k); /* the data of packet 0, then of packet 1 */
  }
  expect_eq(packets_in_flight(Q), 3); /* 2 and the two new ones */
  free(Q);
}

static void a_packet_that_waits_too_long_is_resent_by_the_timer(void)
{
  struct quic *Q = conn();
  send_data(Q, 0);
  send_data(Q, 1);
  g_now += 100;
  uint8_t ack[] = {0x02, 1, 0, 0, 0}; /* only packet 1 arrived */
  deliver(Q, 0, ack, sizeof ack);
  expect_eq(g_sent, 2); /* packet 0 is only 100 ms old against a 112 ms threshold */
  int space, probe;
  unsigned due = next_deadline(Q, &space, &probe);
  expect(due != 0 && !probe && space == S_APP);
  g_now = due - 1;
  run_timers(Q);
  expect_eq(g_sent, 2);
  g_now = due;
  run_timers(Q);
  expect_eq(g_sent, 3);
  free(Q);
}

static void silence_sends_probes_with_a_growing_timeout(void)
{
  struct quic *Q = conn();
  send_data(Q, 9);
  int space, probe;
  unsigned t0 = g_now, due = next_deadline(Q, &space, &probe);
  expect(probe);
  expect_eq(due - t0, 333 + 4 * 166 + 25); /* before any round-trip sample */
  g_now = due;
  run_timers(Q);
  expect_eq(g_sent, 2);
  expect_eq(Q->pto_count, 1);
  unsigned due2 = next_deadline(Q, &space, &probe);
  expect_eq(due2 - g_now, 2 * (333 + 4 * 166 + 25));
  g_now = due2;
  run_timers(Q);
  expect_eq(Q->pto_count, 2);
  uint8_t ack[] = {0x02, 2, 0, 0, 0}; /* the second probe is acknowledged */
  deliver(Q, 0, ack, sizeof ack);
  expect_eq(Q->pto_count, 0);
  free(Q);
}

static void too_many_unanswered_probes_end_the_connection(void)
{
  struct quic *Q = conn();
  send_data(Q, 9);
  for (int i = 0; i < PTO_MAX + 1 && !Q->closed; i++) {
    int space, probe;
    g_now = next_deadline(Q, &space, &probe);
    run_timers(Q);
  }
  expect(Q->closed);
  expect_eq(poll_once(Q), QUIC_ECLOSED);
  free(Q);
}

static void a_full_window_waits_for_acknowledgements(void)
{
  struct quic *Q = conn();
  for (int i = 0; i < SENT_MAX; i++)
    send_data(Q, 1);
  expect_eq(packets_in_flight(Q), SENT_MAX);
  /* No room and nothing arrives: the sender gives up rather than overrun the record. */
  uint8_t f[8] = {0x0e, 0x00, 0x00, 0x03, 7, 7, 7, 0};
  Q->closed = 1;
  expect_eq(emit(Q, S_APP, f, 7, 1), -1);
  free(Q);
}

/* ---- handshake certificate verdict */

static void finished_body(struct quic *Q, const uint8_t th_pre[32], uint8_t out[32])
{
  uint8_t fk[32];
  expand_label(Q->s_hs, "finished", 0, 0, fk, 32);
  hmac_sha256(fk, 32, th_pre, 32, out);
}

static void only_a_verified_certificate_completes_the_handshake(void)
{
  uint8_t th[32] = {1, 2, 3}, mac[32];
  struct quic *Q = conn();
  finished_body(Q, th, mac);

  Q->certs.verify = X509_E_HOSTNAME;
  expect_eq(on_server_finished(Q, mac, 32, th), -1);
  expect_eq(Q->cert_fail, 1);
  expect_eq(Q->app_done, 0);

  Q->cert_fail = 0;
  Q->certs.verify = X509_E_UNTRUSTED; /* no Certificate message at all */
  expect_eq(on_server_finished(Q, mac, 32, th), -1);
  expect_eq(Q->cert_fail, 1);

  Q->cert_fail = 0;
  Q->certs.verify = X509_OK;
  mac[0] ^= 1; /* a wrong Finished is refused too */
  expect_eq(on_server_finished(Q, mac, 32, th), -1);
  expect_eq(Q->cert_fail, 0);
  mac[0] ^= 1;
  expect_eq(on_server_finished(Q, mac, 32, th), 0);
  expect_eq(Q->app_done, 1);

  Q->app_done = 0;
  Q->certs.verify = X509_E_NO_STORE; /* no root bundle to check against: allowed, as over TCP */
  expect_eq(on_server_finished(Q, mac, 32, th), 0);
  free(Q);
}

int main(void)
{
  test("finished_streams_free_their_slot", finished_streams_free_their_slot);
  test("server_limits_come_from_transport_parameters", server_limits_come_from_transport_parameters);
  test("sending_obeys_the_servers_limits", sending_obeys_the_servers_limits);
  test("receive_credit_is_topped_up", receive_credit_is_topped_up);
  test("early_receive_credit_is_not_wasted", early_receive_credit_is_not_wasted);
  test("connection_close_is_noticed", connection_close_is_noticed);
  test("received_ranges_merge", received_ranges_merge);
  test("ack_frame_reports_every_gap", ack_frame_reports_every_gap);
  test("data_past_a_gap_is_dropped_unacknowledged", data_past_a_gap_is_dropped_unacknowledged);
  test("a_duplicate_is_acknowledged_again_but_not_replayed", a_duplicate_is_acknowledged_again_but_not_replayed);
  test("acked_packets_leave_flight_and_time_the_round_trip", acked_packets_leave_flight_and_time_the_round_trip);
  test("packets_the_ack_skips_are_resent", packets_the_ack_skips_are_resent);
  test("a_packet_that_waits_too_long_is_resent_by_the_timer", a_packet_that_waits_too_long_is_resent_by_the_timer);
  test("silence_sends_probes_with_a_growing_timeout", silence_sends_probes_with_a_growing_timeout);
  test("too_many_unanswered_probes_end_the_connection", too_many_unanswered_probes_end_the_connection);
  test("a_full_window_waits_for_acknowledgements", a_full_window_waits_for_acknowledgements);
  test("only_a_verified_certificate_completes_the_handshake", only_a_verified_certificate_completes_the_handshake);
  return 0;
}
