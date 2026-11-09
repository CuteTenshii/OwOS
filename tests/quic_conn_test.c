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

/* ---- 0-RTT and resumption. Expected values come from an independent HKDF/HMAC derivation of RFC 8446 §7.1. */

static const uint8_t PSK_V[32] = {0x8a, 0xa3, 0x2e, 0x6d, 0xc2, 0xb9, 0x39, 0x20, 0x0d, 0x32, 0xaf,
                                  0x6c, 0x17, 0x60, 0xb1, 0x07, 0x57, 0xf5, 0x39, 0x31, 0xc0, 0xb3,
                                  0x48, 0x00, 0x92, 0x16, 0x33, 0x14, 0xa4, 0xae, 0x08, 0xdc};
static const uint8_t EARLY_V[32] = {0x36, 0xb0, 0xeb, 0xee, 0x0c, 0xfe, 0x8e, 0xa4, 0x42, 0xe8, 0xe8,
                                    0xc3, 0xec, 0xfa, 0x1b, 0x9b, 0x7d, 0x4d, 0x0a, 0xb8, 0xd7, 0xdc,
                                    0xd7, 0x1e, 0x2e, 0x92, 0x66, 0x22, 0x63, 0x20, 0xab, 0xf0};
static const uint8_t BINDER_V[32] = {0xf7, 0xc4, 0xfb, 0x71, 0x6e, 0x25, 0x65, 0x8a, 0xf6, 0x48, 0x69,
                                     0xa0, 0x2f, 0x94, 0xc4, 0xa3, 0xf4, 0x6e, 0x97, 0x3d, 0x29, 0xcd,
                                     0x09, 0xff, 0x53, 0xec, 0xbb, 0xed, 0x10, 0x55, 0x7a, 0x5d};
static const uint8_t EKEY_V[16] = {0xf1, 0x87, 0x25, 0x00, 0x32, 0xa4, 0xc9, 0xef,
                                   0x13, 0x74, 0xb9, 0xf3, 0xe3, 0xd1, 0xa6, 0xa0};
static const uint8_t EIV_V[12] = {0xb1, 0x59, 0x07, 0xab, 0x3a, 0x90, 0x65, 0xf9, 0x3f, 0x8c, 0xce, 0x6d};
static const uint8_t EHP_V[16] = {0x46, 0x67, 0xd7, 0xcb, 0xc9, 0xce, 0x3d, 0x93,
                                  0xb0, 0x26, 0x37, 0x00, 0xed, 0xe1, 0xc0, 0xad};

static void resumption_secrets_match_an_independent_derivation(void)
{
  uint8_t res_master[32], nonce[8], psk[32], trunc[100], ch[140], th[32], early[32], binder[32];
  for (int i = 0; i < 32; i++)
    res_master[i] = (uint8_t)(i + 1);
  for (int i = 0; i < 8; i++)
    nonce[i] = (uint8_t)i;
  psk_from_ticket(res_master, nonce, 8, psk);
  expect(memcmp(psk, PSK_V, 32) == 0);

  for (int i = 0; i < 100; i++)
    trunc[i] = (uint8_t)(i * 7 + 3);
  sha256(trunc, sizeof trunc, th);
  compute_binder(psk, th, early, binder);
  expect(memcmp(early, EARLY_V, 32) == 0);
  expect(memcmp(binder, BINDER_V, 32) == 0);

  for (int i = 0; i < 140; i++)
    ch[i] = (uint8_t)(i * 5 + 1);
  sha256(ch, sizeof ch, th);
  struct quic *Q = conn();
  install_early(Q, early, th);
  expect(Q->early_keys);
  expect(memcmp(Q->e_ck, EKEY_V, 16) == 0);
  expect(memcmp(Q->e_ci, EIV_V, 12) == 0);
  expect(memcmp(Q->e_ch, EHP_V, 16) == 0);
  free(Q);
}

static struct ticket sample_ticket(void)
{
  struct ticket t;
  memset(&t, 0, sizeof t);
  strcpy(t.host, "example.com");
  for (int i = 0; i < 50; i++)
    t.data[i] = (uint8_t)(0x40 + i);
  t.len = 50;
  t.age_add = 1000;
  memcpy(t.psk, PSK_V, 32);
  t.issued = g_now - 500;
  t.lifetime_ms = 3600000;
  t.early_ok = 1;
  return t;
}

static void client_hello_offers_the_ticket_with_a_valid_binder(void)
{
  struct quic *Q = conn();
  struct ticket t = sample_ticket();
  static uint8_t ch[1400];
  uint32_t n = build_client_hello(Q, ch, "example.com", "h3", &t);
  expect_eq(ch[0], 1);
  expect_eq(((uint32_t)ch[1] << 16 | ch[2] << 8 | ch[3]) + 4, n);

  uint32_t o = 47, end = 47 + (((uint32_t)ch[45] << 8) | ch[46]); /* the extension list */
  expect_eq(end, n);
  uint16_t last = 0;
  int seen_early = 0, seen_modes = 0;
  uint32_t psk_off = 0;
  while (o + 4 <= end) {
    uint16_t et = (uint16_t)((ch[o] << 8) | ch[o + 1]), el = (uint16_t)((ch[o + 2] << 8) | ch[o + 3]);
    seen_early |= et == 0x002A;
    seen_modes |= et == 0x002D;
    if (et == 0x0029)
      psk_off = o + 4;
    last = et;
    o += 4u + el;
  }
  expect_eq(o, end);
  expect_eq(last, 0x0029); /* pre_shared_key must be last */
  expect(seen_early && seen_modes);

  uint32_t idl = (ch[psk_off] << 8) | ch[psk_off + 1];
  expect_eq(idl, 2 + 50 + 4);
  expect_eq((ch[psk_off + 2] << 8) | ch[psk_off + 3], 50);
  expect(memcmp(ch + psk_off + 4, t.data, 50) == 0);
  const uint8_t *age = ch + psk_off + 4 + 50;
  expect_eq(((uint32_t)age[0] << 24) | (age[1] << 16) | (age[2] << 8) | age[3],
            500 + 1000); /* ms since issue + age_add */

  uint32_t bpos = psk_off + 2 + idl; /* binders<33> */
  expect_eq((ch[bpos] << 8) | ch[bpos + 1], 33);
  expect_eq(ch[bpos + 2], 32);
  expect_eq(bpos + 2 + 33, n);
  uint8_t th[32], early[32], binder[32];
  sha256(ch, bpos, th); /* the hello up to its binders */
  compute_binder(t.psk, th, early, binder);
  expect(memcmp(binder, ch + bpos + 3, 32) == 0);
  expect(Q->psk_offered);
  free(Q);
}

static void a_fresh_hello_carries_no_ticket(void)
{
  struct quic *Q = conn();
  static uint8_t ch[1400];
  uint32_t n = build_client_hello(Q, ch, "example.com", "h3", 0);
  uint32_t o = 47, end = 47 + (((uint32_t)ch[45] << 8) | ch[46]);
  expect_eq(end, n);
  while (o + 4 <= end) {
    uint16_t et = (uint16_t)((ch[o] << 8) | ch[o + 1]);
    expect(et != 0x0029 && et != 0x002A && et != 0x002D);
    o += 4u + ((ch[o + 2] << 8) | ch[o + 3]);
  }
  expect(!Q->psk_offered);
  free(Q);
}

static void new_session_tickets_are_kept_once(void)
{
  struct quic *Q = conn();
  Q->have_res = 1;
  for (int i = 0; i < 32; i++)
    Q->res_master[i] = (uint8_t)(i + 1);
  strcpy(Q->host, "example.com");
  Q->peer_max_data = 111;
  Q->peer_stream_data = 222;
  Q->peer_max_bidi = 33;
  uint8_t b[64];
  int n = 0;
  b[n++] = 0, b[n++] = 0, b[n++] = 0x0e, b[n++] = 0x10;       /* lifetime: 3600 s */
  b[n++] = 0xde, b[n++] = 0xad, b[n++] = 0xbe, b[n++] = 0xef; /* ticket_age_add */
  b[n++] = 8;
  for (int i = 0; i < 8; i++)
    b[n++] = (uint8_t)i; /* nonce */
  b[n++] = 0, b[n++] = 3, b[n++] = 'a', b[n++] = 'b', b[n++] = 'c';
  b[n++] = 0, b[n++] = 8, b[n++] = 0x00, b[n++] = 0x2a, b[n++] = 0, b[n++] = 4, b[n++] = 0xff, b[n++] = 0xff,
  b[n++] = 0xff, b[n++] = 0xff; /* early_data: max_early_data_size */
  on_new_session_ticket(Q, b, (uint32_t)n);

  struct ticket t;
  expect(!ticket_take("other.example", &t));
  expect(ticket_take("example.com", &t));
  expect_eq(t.len, 3);
  expect(memcmp(t.data, "abc", 3) == 0);
  expect_eq(t.age_add, 0xdeadbeef);
  expect_eq(t.lifetime_ms, 3600000);
  expect(t.early_ok);
  expect(memcmp(t.psk, PSK_V, 32) == 0); /* the PSK of this ticket's nonce */
  expect_eq(t.max_data, 111);
  expect_eq(t.stream_data, 222);
  expect_eq(t.max_bidi, 33);
  expect(!ticket_take("example.com", &t)); /* used once */

  on_new_session_ticket(Q, b, (uint32_t)n);
  g_now += 3600001; /* past its lifetime */
  expect(!ticket_take("example.com", &t));

  Q->have_res = 0; /* a ticket before the handshake finished means nothing */
  on_new_session_ticket(Q, b, (uint32_t)n);
  expect(!ticket_take("example.com", &t));
  free(Q);
}

/* Open a long-header packet we sent with the given keys; returns the frames' length, or -1. */
static int open_long(const uint8_t *d, int n, const uint8_t *key, const uint8_t *iv, const uint8_t *hp, uint8_t *b0_out,
                     uint8_t *pt)
{
  uint32_t o = 5;
  o += 1 + d[o]; /* DCID */
  o += 1 + d[o]; /* SCID */
  o += 2;        /* Length (a 2-byte varint) */
  uint32_t pn_off = o;
  uint8_t mask[16], hdr[64], nonce[12];
  hp_compute(hp, d + pn_off + 4, mask);
  memcpy(hdr, d, pn_off + 4);
  hdr[0] ^= mask[0] & 0x0f;
  *b0_out = hdr[0];
  uint64_t pn = 0;
  for (int i = 0; i < 4; i++) {
    hdr[pn_off + i] ^= mask[1 + i];
    pn = (pn << 8) | hdr[pn_off + i];
  }
  memcpy(nonce, iv, 12);
  for (int i = 0; i < 8; i++)
    nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));
  int len = n - (int)pn_off - 4 - 16;
  if (aes128_gcm_open(key, nonce, hdr, pn_off + 4, d + pn_off + 4, (uint32_t)len, pt, d + n - 16) != 0)
    return -1;
  return len;
}

static void early_data_goes_in_0rtt_packets_and_is_resent_if_refused(void)
{
  struct quic *Q = conn();
  Q->sp[S_APP].hk = 0; /* no 1-RTT keys yet */
  memcpy(Q->e_ck, EKEY_V, 16), memcpy(Q->e_ci, EIV_V, 12), memcpy(Q->e_ch, EHP_V, 16);
  Q->early_keys = 1;
  uint8_t f[8] = {0x0e, 0x00, 0x00, 0x03, 'a', 'b', 'c', 0};
  expect_eq(emit(Q, S_APP, f, 7, 1), 0);
  expect_eq(g_sent, 1);
  expect(Q->early_sent);
  expect(Q->sp[S_APP].sent[0].early);

  uint8_t pt[64], b0;
  int n = open_long(g_dg[0], g_dglen[0], EKEY_V, EIV_V, EHP_V, &b0, pt);
  expect_eq(n, 7);
  expect(memcmp(pt, f, 7) == 0);
  expect_eq(b0 & 0xf0, 0xd0); /* long header, packet type 1: 0-RTT */
  expect_eq(g_dg[0][4], 1);   /* version 1 */

  /* The server did not take it: with the 1-RTT keys in place, the same frames go out again as a short packet. */
  Q->sp[S_APP].hk = 1;
  Q->early_keys = 0;
  reject_early(Q);
  expect_eq(g_sent, 2);
  expect_eq(g_dg[1][0] & 0x80, 0);
  uint64_t pn;
  expect_eq(open_sent(Q, g_dg[1], g_dglen[1], pt, &pn), 7);
  expect(memcmp(pt, f, 7) == 0);
  expect_eq(pn, 1);
  expect_eq(packets_in_flight(Q), 1);
  expect(!Q->sp[S_APP].sent[0].early && !Q->sp[S_APP].sent[1].early);
  free(Q);
}

static void a_resumed_handshake_needs_no_certificate_and_binds_the_resumption_secret(void)
{
  struct quic *Q = conn();
  for (int i = 0; i < 32; i++) {
    Q->master[i] = (uint8_t)(64 + i);
    Q->s_hs[i] = (uint8_t)(10 + i);
    Q->c_hs[i] = (uint8_t)(20 + i);
  }
  uint8_t t[120], th_pre[32];
  for (int i = 0; i < 120; i++)
    t[i] = (uint8_t)(i * 3 + 7);
  for (int i = 0; i < 32; i++)
    th_pre[i] = (uint8_t)(100 + i);
  sha256_init(&Q->transcript);
  sha256_update(&Q->transcript, t, sizeof t); /* the transcript through the server's Finished */
  static const uint8_t mac[32] = {0xb8, 0xea, 0xf2, 0xdf, 0x1b, 0x8e, 0x1e, 0x16, 0x94, 0x65, 0x2c,
                                  0xce, 0x3b, 0x02, 0x31, 0x03, 0xa8, 0xf1, 0xae, 0xf6, 0x5b, 0xd4,
                                  0x36, 0x8e, 0xc4, 0xe4, 0x5f, 0x36, 0x4b, 0xe1, 0x27, 0x0f};
  static const uint8_t fin[36] = {0x14, 0x00, 0x00, 0x20, 0xef, 0x25, 0x26, 0xe1, 0x80, 0x95, 0x76, 0xc9,
                                  0x0a, 0x4f, 0x8d, 0x0d, 0x08, 0x6c, 0xe3, 0x77, 0xf7, 0xb7, 0x88, 0xca,
                                  0xae, 0xf1, 0xf3, 0x3f, 0x4e, 0x5a, 0x80, 0xc5, 0x11, 0x55, 0x5e, 0x7e};
  static const uint8_t res[32] = {0xf7, 0x1f, 0xa8, 0x79, 0x52, 0x74, 0x94, 0x53, 0x51, 0x3c, 0xc5,
                                  0x6d, 0x35, 0x1f, 0x9a, 0xb6, 0x8d, 0x2d, 0xc3, 0x2d, 0x3f, 0x5d,
                                  0x58, 0x4e, 0x9b, 0xc3, 0x55, 0x75, 0x09, 0xf8, 0x1e, 0x2c};
  Q->psk_accepted = 1;
  Q->certs.verify = X509_E_UNTRUSTED; /* a PSK handshake has no Certificate message */
  expect_eq(on_server_finished(Q, mac, 32, th_pre), 0);
  expect_eq(Q->cert_fail, 0);
  expect(memcmp(Q->pending_fin, fin, 36) == 0);
  expect(Q->have_res);
  expect(memcmp(Q->res_master, res, 32) == 0); /* bound to the transcript including our own Finished */

  Q->psk_accepted = 0; /* the same, but the server fell back to a full handshake: now it needs a certificate */
  Q->app_done = 0;
  expect_eq(on_server_finished(Q, mac, 32, th_pre), -1);
  expect_eq(Q->cert_fail, 1);
  free(Q);
}

static void the_server_hello_says_whether_the_ticket_was_taken(void)
{
  uint8_t sh[160];
  int n = 0;
  sh[n++] = 3, sh[n++] = 3;
  memset(sh + n, 9, 32);
  n += 32;
  sh[n++] = 0;                 /* session id */
  sh[n++] = 0x13, sh[n++] = 1; /* TLS_AES_128_GCM_SHA256 */
  sh[n++] = 0;                 /* compression */
  int extlen_at = n;
  n += 2;
  sh[n++] = 0x00, sh[n++] = 0x33, sh[n++] = 0, sh[n++] = 36, sh[n++] = 0, sh[n++] = 0x1d, sh[n++] = 0, sh[n++] = 32;
  memset(sh + n, 5, 32);
  n += 32;
  int with_psk = n;
  sh[n++] = 0x00, sh[n++] = 0x29, sh[n++] = 0, sh[n++] = 2, sh[n++] = 0, sh[n++] = 0; /* pre_shared_key: identity 0 */
  sh[extlen_at] = 0, sh[extlen_at + 1] = (uint8_t)(n - extlen_at - 2);
  uint8_t pub[32];
  int sel;
  expect_eq(parse_server_hello(sh, (uint32_t)n, pub, &sel), 0);
  expect_eq(sel, 1);
  expect_eq(pub[0], 5);

  sh[extlen_at + 1] = (uint8_t)(with_psk - extlen_at - 2); /* the same hello cut before the PSK extension */
  expect_eq(parse_server_hello(sh, (uint32_t)with_psk, pub, &sel), 0);
  expect_eq(sel, 0);

  /* ...and an EncryptedExtensions list with or without early_data */
  uint8_t ee[16] = {0, 4, 0x00, 0x2a, 0, 0};
  expect(ext_present(ee, 6, 0x002A));
  uint8_t none[16] = {0, 4, 0x00, 0x10, 0, 0};
  expect(!ext_present(none, 6, 0x002A));
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
  test("resumption_secrets_match_an_independent_derivation", resumption_secrets_match_an_independent_derivation);
  test("client_hello_offers_the_ticket_with_a_valid_binder", client_hello_offers_the_ticket_with_a_valid_binder);
  test("a_fresh_hello_carries_no_ticket", a_fresh_hello_carries_no_ticket);
  test("new_session_tickets_are_kept_once", new_session_tickets_are_kept_once);
  test("early_data_goes_in_0rtt_packets_and_is_resent_if_refused",
       early_data_goes_in_0rtt_packets_and_is_resent_if_refused);
  test("a_resumed_handshake_needs_no_certificate_and_binds_the_resumption_secret",
       a_resumed_handshake_needs_no_certificate_and_binds_the_resumption_secret);
  test("the_server_hello_says_whether_the_ticket_was_taken", the_server_hello_says_whether_the_ticket_was_taken);
  return 0;
}
