/* tcp_test.c: host test for kernel/tcp.c's sender (window, ACK advance, retransmission, zero-window probes, close)
 * and its re-ACKing of duplicate segments. Segments are injected through tcp_input and captured at net_ip_send;
 * sched_intr_pending doubles as the hook that runs while a call blocks. Run with `make test`. */
#define OWOS_SUITE_TEST
#define OWOS_ENTROPY_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H
#define OWOS_SCHED_H
#define OWOS_TIMER_H
#define OWOS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "irqflags_host.h"

/* tcp.c's x86 idle/barrier asm has no host equivalent. */
static int tcp_test_asm_sink;
#define __asm__ tcp_test_asm_sink
#define volatile(...) /* only "__asm__ volatile(...)" is called; the keyword is left alone otherwise */

#define TIMER_HZ 250
static uint32_t g_ticks;
static uint32_t timer_ticks(void) { return g_ticks; }
static uint32_t g_isn;
static void entropy_get(void *out, uint32_t n) { memcpy(out, &g_isn, n < 4 ? n : 4); }
static void *kmalloc(uint32_t n) { return malloc(n); }

/* Runs each time a blocked call polls for interruption; returns non-zero to interrupt it. */
static int (*g_idle)(void);
static int sched_intr_pending(void) { return g_idle ? g_idle() : 0; }

static const uint8_t ME[16] = {[10] = 0xFF, [11] = 0xFF, 10, 0, 2, 15};
static const uint8_t PEER[16] = {[10] = 0xFF, [11] = 0xFF, 10, 0, 2, 2};
#define PEER_PORT 40000
#define PEER_ISN 5000u
#define LISTEN_PORT 80

#define MAX_TX 128
static uint8_t g_tx[MAX_TX][1500];
static uint32_t g_txlen[MAX_TX];
static int g_ntx;

static void net_ip_send(const uint8_t dst[16], uint8_t proto, const uint8_t *payload, uint32_t plen)
{
  (void)dst, (void)proto;
  if (g_ntx < MAX_TX) {
    memcpy(g_tx[g_ntx], payload, plen);
    g_txlen[g_ntx++] = plen;
  }
}

static void net_src_addr(const uint8_t dst[16], uint8_t out[16])
{
  (void)dst;
  memcpy(out, ME, 16);
}

static uint32_t net_tcp_mss(const uint8_t dst[16]) { return (void)dst, 1460; }

static uint16_t net_l4_csum(const uint8_t src[16], const uint8_t dst[16], uint8_t proto, const uint8_t *seg,
                            uint32_t len)
{
  uint32_t sum = 0;
  for (int i = 12; i < 16; i += 2) {
    sum += ((uint32_t)src[i] << 8) | src[i + 1];
    sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
  }
  sum += proto;
  sum += len;
  for (uint32_t i = 0; i + 1 < len; i += 2)
    sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
  if (len & 1)
    sum += (uint32_t)seg[len - 1] << 8;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

#include "../kernel/tcp.c"

#undef volatile
#undef __asm__

static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t tx_seq(int i) { return be32(g_tx[i] + 4); }
static uint32_t tx_ack(int i) { return be32(g_tx[i] + 8); }
static uint8_t tx_flags(int i) { return g_tx[i][13]; }
static uint32_t tx_dlen(int i) { return g_txlen[i] - 20; }

static void inject(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t win, const uint8_t *data, uint32_t dlen)
{
  uint8_t seg[20 + 1460] = {0};
  struct tcp_hdr *t = (struct tcp_hdr *)seg;
  t->sport = htons(PEER_PORT);
  t->dport = htons(LISTEN_PORT);
  t->seq = htonl(seq);
  t->ack = htonl(ack);
  t->off = 5 << 4;
  t->flags = flags;
  t->win = htons(win);
  if (dlen)
    memcpy(seg + 20, data, dlen);
  t->csum = htons(net_l4_csum(PEER, ME, IP_PROTO_TCP, seg, 20 + dlen));
  tcp_input(PEER, ME, seg, 20 + dlen);
}

static void reset(void)
{
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    free(conns[i].rxbuf);
    free(conns[i].txbuf);
  }
  memset(conns, 0, sizeof conns);
  memset(listeners, 0, sizeof listeners);
  g_ntx = 0;
  g_idle = NULL;
  g_ticks = 1000;
  g_isn = 0x12345678u;
}

/* Passive open with the peer advertising `win`; the returned connection has sent nothing yet. */
static struct tcp_conn *establish(uint16_t win)
{
  expect_eq(tcp_listen(LISTEN_PORT), 0);
  inject(TCP_SYN, PEER_ISN, 0, win, NULL, 0);
  expect_eq(g_ntx, 1);
  expect_eq(tx_flags(0), TCP_SYN | TCP_ACK);
  expect_eq(tx_seq(0), g_isn);
  inject(TCP_ACK, PEER_ISN + 1, g_isn + 1, win, NULL, 0);
  struct tcp_conn *c = tcp_accept(LISTEN_PORT);
  expect(c != NULL);
  g_ntx = 0;
  return c;
}

static void ack(uint32_t a, uint16_t win) { inject(TCP_ACK, PEER_ISN + 1, a, win, NULL, 0); }

static uint8_t g_data[200000];

static void fill_data(void)
{
  for (uint32_t i = 0; i < sizeof g_data; i++)
    g_data[i] = (uint8_t)(i * 7 + (i >> 9));
}

static void test_window_limited_send(void)
{
  reset();
  g_isn = 0xFFFFF000u; /* the stream crosses sequence number 2^32 */
  struct tcp_conn *c = establish(3000);
  uint32_t base = g_isn + 1;
  expect_eq(tcp_write(c, g_data, 5000), 5000);
  expect_eq(g_ntx, 3);
  expect_eq(tx_seq(0), base);
  expect_eq(tx_dlen(0), 1460);
  expect_eq(tx_seq(1), base + 1460);
  expect_eq(tx_dlen(1), 1460);
  expect_eq(tx_seq(2), base + 2920);
  expect_eq(tx_dlen(2), 80);
  expect(memcmp(g_tx[1] + 20, g_data + 1460, 1460) == 0);
  expect_eq(c->snd_nxt - c->snd_una, 3000);

  tcp_tick(); /* the window is full: nothing more goes out */
  expect_eq(g_ntx, 3);
}

static void test_ack_advances_window(void)
{
  reset();
  g_isn = 0xFFFFF000u;
  struct tcp_conn *c = establish(3000);
  uint32_t base = g_isn + 1;
  expect_eq(tcp_write(c, g_data, 5000), 5000);
  g_ntx = 0;

  ack(base + 1460, 3000);
  expect_eq(c->snd_una, base + 1460);
  expect_eq(g_ntx, 0); /* pure ACKs are not answered */
  tcp_tick();
  expect_eq(g_ntx, 1);
  expect_eq(tx_seq(0), base + 3000);
  expect_eq(tx_dlen(0), 1460);
  expect(memcmp(g_tx[0] + 20, g_data + 3000, 1460) == 0);

  ack(base + 4460, 3000);
  tcp_tick();
  expect_eq(g_ntx, 2);
  expect_eq(tx_seq(1), base + 4460);
  expect_eq(tx_dlen(1), 540);

  ack(base + 5000, 3000);
  expect_eq(c->snd_una, c->snd_end);
  expect(!c->timer_on);
  expect(c->srtt8 != 0);

  ack(base + 6000, 3000); /* acknowledges data never sent: answered, ignored */
  expect_eq(c->snd_una, base + 5000);
  expect_eq(g_ntx, 3);
  expect_eq(tx_flags(2), TCP_ACK);
}

static void test_retransmit_on_timeout(void)
{
  reset();
  struct tcp_conn *c = establish(10000);
  uint32_t base = g_isn + 1;
  expect_eq(tcp_write(c, g_data, 2000), 2000);
  expect_eq(g_ntx, 2);
  g_ntx = 0;

  g_ticks += TCP_RTO_INIT - 1;
  tcp_tick();
  expect_eq(g_ntx, 0);
  g_ticks += 1;
  tcp_tick();
  expect_eq(g_ntx, 1); /* only the oldest segment is resent */
  expect_eq(tx_seq(0), base);
  expect_eq(tx_dlen(0), 1460);
  expect(memcmp(g_tx[0] + 20, g_data, 1460) == 0);

  g_ticks += TCP_RTO_INIT; /* backed off to twice the timeout */
  tcp_tick();
  expect_eq(g_ntx, 1);
  g_ticks += TCP_RTO_INIT;
  tcp_tick();
  expect_eq(g_ntx, 2);
  expect_eq(tx_seq(1), base);

  ack(base + 1460, 10000);
  expect_eq(c->retries, 0);
  g_ticks += c->rto;
  tcp_tick();
  expect_eq(g_ntx, 3);
  expect_eq(tx_seq(2), base + 1460);
  expect_eq(tx_dlen(2), 540);
  expect(memcmp(g_tx[2] + 20, g_data + 1460, 540) == 0);
}

static void test_gives_up_after_retries(void)
{
  reset();
  struct tcp_conn *c = establish(10000);
  expect_eq(tcp_write(c, g_data, 100), 100);
  g_ntx = 0;
  int resent = 0, rst = 0;
  for (int i = 0; i < 64 && !rst; i++) {
    g_ticks += TCP_RTO_MAX;
    tcp_tick();
    for (; resent + rst < g_ntx;) {
      if (tx_flags(resent + rst) & TCP_RST)
        rst++;
      else
        resent++;
    }
  }
  expect_eq(resent, TCP_MAX_RETRIES);
  expect_eq(rst, 1);
  expect(c->reset);
  expect_eq(c->state, ST_DONE);
  expect_eq(tcp_write(c, g_data, 10), -1);
  int closed = 0;
  expect_eq(tcp_read_nb(c, g_data + 100000, 10, &closed), -1);
  tcp_close(c);
  expect_eq(c->state, ST_CLOSED);
}

static void test_zero_window_probe(void)
{
  reset();
  struct tcp_conn *c = establish(0);
  uint32_t base = g_isn + 1;
  expect_eq(tcp_write(c, g_data, 100), 100);
  expect_eq(g_ntx, 0);
  tcp_tick();
  expect_eq(g_ntx, 0);

  /* Probes keep going for as long as the peer answers them, however long the window stays shut. */
  for (int i = 0; i < 3 * TCP_MAX_RETRIES; i++) {
    g_ticks += TCP_RTO_MAX;
    tcp_tick();
    expect_eq(g_ntx, i + 1);
    expect_eq(tx_seq(i), base - 1);
    expect_eq(tx_dlen(i), 0);
    expect_eq(tx_flags(i), TCP_ACK);
    ack(base, 0);
  }
  expect(!c->reset);

  ack(base, 1000);
  int before = g_ntx;
  tcp_tick();
  expect_eq(g_ntx, before + 1);
  expect_eq(tx_seq(before), base);
  expect_eq(tx_dlen(before), 100);
}

static void test_unanswered_probes_reset(void)
{
  reset();
  struct tcp_conn *c = establish(0);
  expect_eq(tcp_write(c, g_data, 100), 100);
  for (int i = 0; i < 64 && !c->reset; i++) {
    g_ticks += TCP_RTO_MAX;
    tcp_tick();
  }
  expect(c->reset);
  expect_eq(tx_flags(g_ntx - 1), TCP_RST | TCP_ACK);
}

static struct tcp_conn *g_conn;
static int g_idle_calls;

/* The peer acknowledges everything sent so far and reopens its window. */
static int idle_ack_all(void)
{
  g_idle_calls++;
  ack(g_conn->snd_nxt, 65535);
  return 0;
}

static int idle_interrupt(void) { return 1; }

static void test_write_blocks_until_acked(void)
{
  reset();
  struct tcp_conn *c = establish(65535);
  g_conn = c;
  g_idle = idle_ack_all;
  g_idle_calls = 0;
  expect_eq(tcp_write(c, g_data, 70000), 70000);
  expect(g_idle_calls >= 1);

  /* Everything handed to the peer is the written stream, in order and without gaps. */
  uint32_t next = g_isn + 1;
  for (int i = 0; i < g_ntx; i++) {
    if (!tx_dlen(i))
      continue;
    expect_eq(tx_seq(i), next);
    expect(memcmp(g_tx[i] + 20, g_data + (tx_seq(i) - (g_isn + 1)), tx_dlen(i)) == 0);
    next += tx_dlen(i);
  }
  expect_eq(next - (g_isn + 1), 70000);
}

static void test_write_interrupted_when_full(void)
{
  reset();
  struct tcp_conn *c = establish(0);
  g_idle = idle_interrupt;
  expect_eq(tcp_write(c, g_data, TCP_TXBUF + 100), TCP_TXBUF);
  expect_eq(tcp_write(c, g_data, 10), TCP_EINTR);
}

static void test_duplicate_fin_reacked(void)
{
  reset();
  struct tcp_conn *c = establish(10000);
  uint8_t msg[] = "0123456789";

  inject(TCP_ACK | TCP_PSH, PEER_ISN + 1, g_isn + 1, 10000, msg, 10);
  inject(TCP_ACK | TCP_PSH, PEER_ISN + 1, g_isn + 1, 10000, msg, 10); /* duplicate */
  expect_eq(g_ntx, 2);
  expect_eq(tx_ack(1), PEER_ISN + 11);
  expect_eq(c->wlen, 10);

  inject(TCP_ACK | TCP_FIN, PEER_ISN + 11, g_isn + 1, 10000, NULL, 0);
  expect_eq(c->state, ST_CLOSE_WAIT);
  expect_eq(g_ntx, 3);
  expect_eq(tx_ack(2), PEER_ISN + 12);
  inject(TCP_ACK | TCP_FIN, PEER_ISN + 11, g_isn + 1, 10000, NULL, 0); /* our ACK was lost */
  expect_eq(g_ntx, 4);
  expect_eq(tx_flags(3), TCP_ACK);
  expect_eq(tx_ack(3), PEER_ISN + 12);

  inject(TCP_ACK, PEER_ISN + 200000, g_isn + 1, 10000, NULL, 0); /* out of window, no payload */
  expect_eq(g_ntx, 5);
  inject(TCP_RST, PEER_ISN + 200000, 0, 0, NULL, 0); /* RSTs are never answered */
  expect_eq(g_ntx, 5);
  expect(!c->reset);
}

static void test_close_retransmits_fin(void)
{
  reset();
  struct tcp_conn *c = establish(10000);
  uint32_t base = g_isn + 1;
  expect_eq(tcp_write(c, g_data, 100), 100);
  tcp_close(c);
  expect_eq(c->state, ST_LINGER);
  expect_eq(g_ntx, 2);
  expect_eq(tx_seq(1), base + 100);
  expect_eq(tx_flags(1), TCP_ACK | TCP_FIN);

  g_ticks += TCP_RTO_INIT;
  tcp_tick();
  expect_eq(g_ntx, 3); /* the data and FIN go again together */
  expect_eq(tx_seq(2), base);
  expect_eq(tx_dlen(2), 100);
  expect(tx_flags(2) & TCP_FIN);

  ack(base + 100, 10000);
  g_ticks += c->rto;
  tcp_tick();
  expect_eq(g_ntx, 4);
  expect_eq(tx_seq(3), base + 100);
  expect_eq(tx_dlen(3), 0);
  expect_eq(tx_flags(3), TCP_ACK | TCP_FIN);

  inject(TCP_ACK | TCP_FIN, PEER_ISN + 1, base + 101, 10000, NULL, 0);
  expect_eq(g_ntx, 5);
  expect_eq(tx_ack(4), PEER_ISN + 2);
  expect_eq(c->state, ST_CLOSED);
}

static void test_close_gives_up_after_linger(void)
{
  reset();
  struct tcp_conn *c = establish(0);
  expect_eq(tcp_write(c, g_data, 100), 100);
  tcp_close(c);
  expect_eq(c->state, ST_LINGER);
  for (uint32_t t = 0; t <= TCP_LINGER && c->state == ST_LINGER; t += TIMER_HZ) {
    g_ticks += TIMER_HZ;
    tcp_tick();
    if (c->state == ST_LINGER)
      ack(c->snd_una, 0); /* the peer answers probes but never opens its window */
  }
  expect_eq(c->state, ST_CLOSED);
  expect_eq(tx_flags(g_ntx - 1), TCP_RST | TCP_ACK);
}

static int idle_tick(void)
{
  g_idle_calls++;
  g_ticks++;
  return 0;
}

static void test_read_timeout_across_tick_wrap(void)
{
  reset();
  struct tcp_conn *c = establish(10000);
  g_ticks = 0xFFFFFF00u;
  g_idle = idle_tick;
  g_idle_calls = 0;
  int closed = 0;
  uint8_t buf[16];
  expect_eq(tcp_read(c, buf, sizeof buf, &closed), 0);
  expect_eq(closed, 1);
  expect(g_idle_calls >= 5 * TIMER_HZ - 1);
}

int main(void)
{
  fill_data();
  test("send stays within the peer's window", test_window_limited_send);
  test("ACKs advance the window", test_ack_advances_window);
  test("retransmit the oldest segment on timeout", test_retransmit_on_timeout);
  test("reset after the last retry", test_gives_up_after_retries);
  test("zero window is probed", test_zero_window_probe);
  test("unanswered zero-window probes reset", test_unanswered_probes_reset);
  test("write blocks until the buffer drains", test_write_blocks_until_acked);
  test("write interrupted with a full buffer", test_write_interrupted_when_full);
  test("duplicate data and FIN are re-acknowledged", test_duplicate_fin_reacked);
  test("close retransmits the FIN", test_close_retransmits_fin);
  test("close gives up after lingering", test_close_gives_up_after_linger);
  test("read timeout across tick wraparound", test_read_timeout_across_tick_wrap);
  return 0;
}
