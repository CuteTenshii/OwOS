/* Host test for the per-connection state of quic.c: stream slots, server limits from transport parameters, MAX_DATA
 * credit and close detection. quic.c is included so the test can drive its frame handling directly; the socket
 * calls are fakes. */
#define OWOS_SUITE_TEST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define net_dial quict_dial
#define net_send quict_send
#define net_recv quict_recv
#define net_recv_nb quict_recv_nb
#define net_close quict_close
#define rng_bytes quict_rng

void rng_bytes(void *p, int n);

#include "test.h"

#include "../user/lib/curl/quic.c"

void rng_bytes(void *p, int n) { memset(p, 7, (size_t)n); }
int net_dial(const char *host, int port, int proto) { return (void)host, (void)port, (void)proto, 5; }
int net_recv(int h, void *buf, int max) { return (void)h, (void)buf, (void)max, 0; }
int net_recv_nb(int h, void *buf, int max) { return (void)h, (void)buf, (void)max, 0; }
int net_close(int h) { return (void)h, 0; }

static int g_sent;
int net_send(int h, const void *buf, int len)
{
  (void)h, (void)buf;
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
    int n = stream_frame(f, sid, 0, "hello", 5, 1);
    process_frames(Q, S_APP, f, (uint32_t)n);
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
  process_frames(Q, S_APP, max_streams, 3);
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
  process_frames(Q, S_APP, max_data, 5);
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
  process_frames(Q, S_APP, close, 4);
  expect_eq(quic_peer_closed(Q), 1);
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
  return 0;
}
