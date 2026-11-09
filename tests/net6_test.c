/* net6_test.c: host test for the IPv6 half of kernel/net.c (DAD, router advertisements, neighbor and MLD replies,
 * fragment reassembly, address parsing) with frames injected through net_rx_frame and transmissions captured at
 * nic_send. Run with `make test`. */
#define OWOS_SUITE_TEST
#define OWOS_CONSOLE_H
#define OWOS_ENTROPY_H
#define OWOS_FS_H
#define OWOS_GZIP_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H
#define OWOS_LOADER_H
#define OWOS_LOG_H
#define OWOS_SCHED_H
#define OWOS_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* net.c's x86 idle/interrupt asm has no host equivalent. */
static int net_test_asm_sink;
#define __asm__ net_test_asm_sink
#define volatile(...) /* only "__asm__ volatile(...)" is called; the keyword is left alone otherwise */

#define TIMER_HZ 250
static uint32_t g_ticks;
static uint32_t timer_ticks(void) { return g_ticks; }
static uint32_t timer_us(void) { return g_ticks * 4000u; }
static void timer_sleep(uint32_t ms) { (void)ms; }
static void con_write(const char *s) { (void)s; }
static void entropy_stir(uint32_t v) { (void)v; }
static int fs_size(const char *p) { return (void)p, -1; }
static int fs_read(const char *p, void *b, uint32_t n) { return (void)p, (void)b, (void)n, -1; }
static void *kmalloc(uint32_t n) { return malloc(n); }
static void kfree(void *p) { free(p); }
static int exec_program(const void *b, uint32_t n, int argc, char **argv)
{
  return (void)b, (void)n, (void)argc, (void)argv, 0;
}
static int sched_create_thread(const char *name, void (*fn)(void)) { return (void)name, (void)fn, 0; }
static uint32_t sched_current_process_id(void) { return 1; }
static int sched_intr_pending(void) { return 0; }

static uint8_t g_tx[32][1514];
static uint32_t g_txlen[32];
static int g_ntx;
static const uint8_t g_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

static void nic_init(void) {}
static int nic_present(void) { return 1; }
static const uint8_t *nic_mac(void) { return g_mac; }
static void nic_poll(void) {}
static void nic_send(const uint8_t *f, uint32_t len)
{
  if (g_ntx < 32) {
    memcpy(g_tx[g_ntx], f, len);
    g_txlen[g_ntx++] = len;
  }
}

typedef struct tcp_conn tcp_conn_t;
static tcp_conn_t *tcp_connect(const uint8_t *ip, uint16_t port) { return (void)ip, (void)port, NULL; }
static int tcp_write(tcp_conn_t *c, const void *d, uint32_t n) { return (void)c, (void)d, (void)n, -1; }
static int tcp_read(tcp_conn_t *c, uint8_t *b, uint32_t m, int *closed)
{
  return (void)c, (void)b, (void)m, *closed = 1, -1;
}
static int tcp_read_nb(tcp_conn_t *c, uint8_t *b, uint32_t m, int *closed)
{
  return (void)c, (void)b, (void)m, *closed = 1, -1;
}
static void tcp_close(tcp_conn_t *c) { (void)c; }
static int tcp_listen(uint16_t p) { return (void)p, 0; }
static void tcp_unlisten(uint16_t p) { (void)p; }
static tcp_conn_t *tcp_accept(uint16_t p) { return (void)p, NULL; }
static int g_tcp_segments;
static void tcp_input(const uint8_t *src, const uint8_t *dst, const uint8_t *seg, uint32_t len)
{
  (void)src, (void)dst, (void)seg, (void)len;
  g_tcp_segments++;
}

#undef htons
#undef htonl
#undef ntohs
#include "../kernel/net.c"

#undef volatile
#undef __asm__

static const uint8_t LL_PEER[16] = {0xfe, 0x80, [15] = 0x02};
static const uint8_t ALL_NODES[16] = {0xff, 0x02, [15] = 0x01};
static const uint8_t PEER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

static void reset(void)
{
  g_ticks = 1000;
  g_ntx = 0;
  g_tcp_segments = 0;
  memset(nbr_cache, 0, sizeof nbr_cache);
  memset(nbr_pending, 0, sizeof nbr_pending);
  memset(frags, 0, sizeof frags);
  memset(usocks, 0, sizeof usocks);
  memset(gua6, 0, sizeof gua6);
  gua6_state = AS_NONE;
  gua6_expire = gw6_expire = dns6_expire = 0;
  have_gw6 = have_dns6 = false;
  net_init();
}

/* Run DAD to completion. */
static void settle(void)
{
  net_tick();
  g_ticks += TIMER_HZ + 1;
  net_tick();
}

static void rx_ip6(const uint8_t *src, const uint8_t *dst, uint8_t nh, uint8_t hlim, const uint8_t *payload,
                   uint32_t plen)
{
  uint8_t f[1514] = {0};
  memcpy(f, g_mac, 6);
  memcpy(f + 6, PEER_MAC, 6);
  f[12] = 0x86;
  f[13] = 0xDD;
  struct ip6_hdr *ip = (struct ip6_hdr *)(f + 14);
  ip->vtf = htonl(0x60000000);
  ip->plen = htons(plen);
  ip->nh = nh;
  ip->hlim = hlim;
  memcpy(ip->src, src, 16);
  memcpy(ip->dst, dst, 16);
  memcpy(f + 54, payload, plen);
  net_rx_frame(f, 54 + plen);
}

static void rx_icmp6(const uint8_t *src, const uint8_t *dst, uint8_t hlim, uint8_t *msg, uint32_t len)
{
  msg[2] = msg[3] = 0;
  uint16_t c = net_l4_csum(src, dst, IP_ICMP6, msg, len);
  msg[2] = (uint8_t)(c >> 8);
  msg[3] = (uint8_t)c;
  rx_ip6(src, dst, IP_ICMP6, hlim, msg, len);
}

/* The first transmitted IPv6 frame of upper-layer type `nh` whose ICMPv6 type (if nh is ICMPv6) is `icmp`. */
static const uint8_t *find_tx(uint8_t nh, int icmp, uint32_t *plen)
{
  for (int i = 0; i < g_ntx; i++) {
    const uint8_t *f = g_tx[i];
    if (f[12] != 0x86 || f[13] != 0xDD || f[20] != nh)
      continue;
    if (nh == IP_ICMP6 && f[54] != icmp)
      continue;
    if (plen)
      *plen = ((uint32_t)f[18] << 8) | f[19];
    return f;
  }
  return NULL;
}

static bool icmp6_valid(const uint8_t *f)
{
  uint32_t len = ((uint32_t)f[18] << 8) | f[19];
  return net_l4_csum(f + 22, f + 38, IP_ICMP6, f + 54, len) == 0;
}

static void test_dad_probe_then_preferred(void)
{
  reset();
  expect_eq(ll6_state, AS_NEW);
  net_tick();
  expect_eq(ll6_state, AS_TENTATIVE);
  const uint8_t *ns = find_tx(IP_ICMP6, ICMP6_NS, NULL);
  expect(ns != NULL);
  if (ns) {
    expect(addr_is_unspec(ns + 22)); /* probes come from :: */
    expect(memcmp(ns + 62, ll6, 16) == 0);
    expect(icmp6_valid(ns));
  }
  expect(!ip6_dst_ok(ll6)); /* not usable yet */
  expect(find_tx(IP_ICMP6, ICMP6_RS, NULL) == NULL);

  g_ticks += TIMER_HZ + 1;
  net_tick();
  expect_eq(ll6_state, AS_PREFERRED);
  expect(ip6_dst_ok(ll6));
  expect(find_tx(IP_ICMP6, ICMP6_RS, NULL) != NULL);
}

static void test_dad_conflict_disables_address(void)
{
  uint8_t na[32] = {ICMP6_NA, 0, 0, 0, 0x20};
  reset();
  net_tick();
  memcpy(na + 8, ll6, 16);
  rx_icmp6(LL_PEER, ALL_NODES, 255, na, 24);
  expect_eq(ll6_state, AS_DUP);
  g_ticks += 10 * TIMER_HZ;
  net_tick();
  expect_eq(ll6_state, AS_DUP);
  expect(!ip6_dst_ok(ll6));

  uint8_t ns[24] = {ICMP6_NS}; /* another node probing the same address */
  reset();
  net_tick();
  memcpy(ns + 8, ll6, 16);
  uint8_t sn[16];
  solicited_node(ll6, sn);
  rx_icmp6(unspec6, sn, 255, ns, sizeof ns);
  expect_eq(ll6_state, AS_DUP);
}

static void test_router_advertisement(void)
{
  static const uint8_t prefix[8] = {0x20, 0x01, 0x0d, 0xb8, 0, 1};
  static const uint8_t rdnss[16] = {0x20, 0x01, 0x0d, 0xb8, [15] = 0x53};
  uint8_t ra[16 + 8 + 32 + 24] = {ICMP6_RA, 0, 0, 0, 64, 0, 0x07, 0x08 /* lifetime 1800 s */};
  uint8_t *o = ra + 16;
  o[0] = ND_OPT_SLLA;
  o[1] = 1;
  memcpy(o + 2, PEER_MAC, 6);
  o += 8;
  o[0] = ND_OPT_PREFIX;
  o[1] = 4;
  o[2] = 64;
  o[3] = 0xC0;
  o[6] = 0x0E; /* valid 3600 s */
  o[7] = 0x10;
  memcpy(o + 16, prefix, 8);
  o += 32;
  o[0] = ND_OPT_RDNSS;
  o[1] = 3;
  o[7] = 0x58; /* lifetime 600 s */
  o[6] = 0x02;
  memcpy(o + 8, rdnss, 16);

  reset();
  settle();
  rx_icmp6(LL_PEER, ALL_NODES, 255, ra, sizeof ra);
  expect(have_gw6 && memcmp(gw6, LL_PEER, 16) == 0);
  uint8_t mac[6];
  expect(nbr_lookup(LL_PEER, mac) && memcmp(mac, PEER_MAC, 6) == 0);
  expect(net_my_ip6() == NULL); /* tentative until DAD ends */
  net_tick();
  expect_eq(gua6_state, AS_TENTATIVE);
  g_ticks += TIMER_HZ + 1;
  net_tick();
  const uint8_t *gua = net_my_ip6();
  expect(gua != NULL);
  if (gua) {
    expect(memcmp(gua, prefix, 8) == 0 && memcmp(gua + 8, ll6 + 8, 8) == 0);
    uint8_t far[16] = {0x26, 0x06, [15] = 1}, src[16];
    net_src_addr(far, src);
    expect(memcmp(src, gua, 16) == 0);
    net_src_addr(LL_PEER, src);
    expect(memcmp(src, ll6, 16) == 0);
  }
  expect(net_dns6() && memcmp(net_dns6(), rdnss, 16) == 0);

  g_ticks += 601 * TIMER_HZ;
  net_tick();
  expect(net_dns6() == NULL);
  expect(have_gw6);
  g_ticks += 1200 * TIMER_HZ;
  net_tick();
  expect(!have_gw6);
  g_ticks += 2000 * TIMER_HZ;
  net_tick();
  expect(net_my_ip6() == NULL);
}

static void test_prefix_withdrawn(void)
{
  uint8_t ra[16 + 32] = {ICMP6_RA};
  uint8_t *o = ra + 16;
  o[0] = ND_OPT_PREFIX;
  o[1] = 4;
  o[2] = 64;
  o[3] = 0x40;
  o[6] = 0x0E;
  o[7] = 0x10;
  o[16] = 0x20;
  o[17] = 0x01;
  reset();
  settle();
  rx_icmp6(LL_PEER, ALL_NODES, 255, ra, sizeof ra);
  settle();
  expect(net_my_ip6() != NULL);
  memset(o + 4, 0, 4); /* valid lifetime 0 */
  rx_icmp6(LL_PEER, ALL_NODES, 255, ra, sizeof ra);
  expect(net_my_ip6() == NULL);
}

static void test_neighbor_solicitation_reply(void)
{
  uint8_t ns[32] = {ICMP6_NS}, sn[16];
  memcpy(ns + 8, ll6, 16);
  ns[24] = ND_OPT_SLLA;
  ns[25] = 1;
  memcpy(ns + 26, PEER_MAC, 6);
  solicited_node(ll6, sn);

  reset();
  net_tick(); /* tentative: must stay silent */
  int before = g_ntx;
  rx_icmp6(LL_PEER, sn, 255, ns, sizeof ns);
  expect_eq(g_ntx, before);

  settle();
  g_ntx = 0;
  rx_icmp6(LL_PEER, sn, 255, ns, sizeof ns);
  const uint8_t *na = find_tx(IP_ICMP6, ICMP6_NA, NULL);
  expect(na != NULL);
  if (na) {
    expect(memcmp(na, PEER_MAC, 6) == 0); /* unicast to the soliciting node */
    expect(na[58] == 0x60 && memcmp(na + 62, ll6, 16) == 0);
    expect(memcmp(na + 62 + 18, g_mac, 6) == 0);
    expect(icmp6_valid(na));
  }

  g_ntx = 0;
  rx_icmp6(LL_PEER, sn, 64, ns, sizeof ns); /* hop limit must be 255 */
  expect_eq(g_ntx, 0);
  uint8_t other[16] = {0xfe, 0x80, [15] = 0x99};
  memcpy(ns + 8, other, 16); /* not our address */
  rx_icmp6(LL_PEER, sn, 255, ns, sizeof ns);
  expect_eq(g_ntx, 0);
}

static void test_mld_query_answered(void)
{
  uint8_t q[28] = {MLD_QUERY};
  reset();
  settle();
  g_ntx = 0;
  rx_icmp6(LL_PEER, ALL_NODES, 1, q, sizeof q);
  const uint8_t *r = find_tx(IP6_HBH, 0, NULL);
  expect(r != NULL);
  if (r) {
    uint8_t group[16];
    solicited_node(ll6, group);
    expect(r[54] == IP_ICMP6 && r[56] == 5); /* router alert option */
    expect(r[62] == MLD_V2_REPORT && r[70] == 2 && memcmp(r + 74, group, 16) == 0);
    expect(net_l4_csum(r + 22, r + 38, IP_ICMP6, r + 62, 28) == 0);
  }
}

#define FRAG_PAYLOAD 1200
#define FRAG_STEP 504 /* multiple of 8 */

/* A UDP datagram to port 5000 split into fragments of FRAG_STEP bytes; send the ones listed in `order`. */
static uint8_t g_udp[8 + FRAG_PAYLOAD];

static void make_datagram(void)
{
  memset(g_udp, 0, 8);
  g_udp[1] = 9; /* sport 9, dport 5000 */
  g_udp[2] = 5000 >> 8;
  g_udp[3] = 5000 & 0xff;
  g_udp[4] = sizeof g_udp >> 8;
  g_udp[5] = sizeof g_udp & 0xff;
  for (int i = 0; i < FRAG_PAYLOAD; i++)
    g_udp[8 + i] = (uint8_t)(i * 7 + 1);
  uint16_t c = net_l4_csum(LL_PEER, ll6, IP_UDP, g_udp, sizeof g_udp);
  g_udp[6] = (uint8_t)(c >> 8);
  g_udp[7] = (uint8_t)c;
}

static void send_fragment(uint32_t id, int idx, uint32_t step)
{
  uint8_t pkt[8 + FRAG_STEP + 16];
  uint32_t off = (uint32_t)idx * step;
  uint32_t n = sizeof g_udp - off < step ? (uint32_t)sizeof g_udp - off : step;
  memset(pkt, 0, 8);
  pkt[0] = IP_UDP;
  uint32_t offlg = off | (off + n < sizeof g_udp ? 1 : 0);
  pkt[2] = (uint8_t)(offlg >> 8);
  pkt[3] = (uint8_t)offlg;
  pkt[4] = (uint8_t)(id >> 24);
  pkt[5] = (uint8_t)(id >> 16);
  pkt[6] = (uint8_t)(id >> 8);
  pkt[7] = (uint8_t)id;
  memcpy(pkt + 8, g_udp + off, n);
  rx_ip6(LL_PEER, ll6, IP6_FRAG, 64, pkt, 8 + n);
}

static bool got_datagram(int sock)
{
  struct usock *s = &usocks[sock];
  return s->dg_head != s->dg_tail && s->dg_len[s->dg_head] == FRAG_PAYLOAD &&
         memcmp(s->dg[s->dg_head], g_udp + 8, FRAG_PAYLOAD) == 0;
}

static int frag_setup(void)
{
  reset();
  settle();
  make_datagram();
  return net_sock_bind(5000);
}

static void test_fragments_in_order(void)
{
  int s = frag_setup();
  for (int i = 0; i < 3; i++) {
    expect(!got_datagram(s));
    send_fragment(1, i, FRAG_STEP);
  }
  expect(got_datagram(s));
  expect(!frags[0].used && !frags[1].used);
}

static void test_fragments_out_of_order(void)
{
  int s = frag_setup();
  send_fragment(2, 2, FRAG_STEP);
  send_fragment(2, 0, FRAG_STEP);
  expect(!got_datagram(s));
  send_fragment(2, 1, FRAG_STEP);
  expect(got_datagram(s));
}

static void test_fragments_interleaved(void)
{
  int s = frag_setup();
  send_fragment(10, 0, FRAG_STEP);
  send_fragment(11, 0, FRAG_STEP);
  send_fragment(10, 1, FRAG_STEP);
  send_fragment(11, 1, FRAG_STEP);
  send_fragment(10, 2, FRAG_STEP);
  expect(got_datagram(s));
  usocks[s].dg_head = usocks[s].dg_tail;
  send_fragment(11, 2, FRAG_STEP);
  expect(got_datagram(s));
}

static void test_fragment_overlap_dropped(void)
{
  int s = frag_setup();
  send_fragment(3, 0, FRAG_STEP);
  send_fragment(3, 0, FRAG_STEP); /* duplicate: RFC 5722 drops the whole datagram */
  send_fragment(3, 1, FRAG_STEP);
  send_fragment(3, 2, FRAG_STEP);
  expect(!got_datagram(s));
}

static void test_fragment_timeout_and_bad_length(void)
{
  frag_setup();
  send_fragment(4, 0, FRAG_STEP);
  expect(frags[0].used || frags[1].used);
  g_ticks += FRAG_TIMEOUT + 1;
  net_tick();
  expect(!frags[0].used && !frags[1].used);

  send_fragment(5, 0, 500); /* non-final fragment, length not a multiple of 8 */
  expect(!frags[0].used && !frags[1].used);
}

static void test_addresses_round_trip(void)
{
  static const char *const cases[][2] = {
      {"::", "::"},
      {"::1", "::1"},
      {"2001:db8::1", "2001:db8::1"},
      {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
      {"fe80::5054:ff:fe12:3456", "fe80::5054:ff:fe12:3456"},
      {"2001:db8:0:1:0:0:0:1", "2001:db8:0:1::1"},
      {"1:0:0:2:0:0:0:3", "1:0:0:2::3"},
      {"0:0:0:0:0:0:0:0", "::"},
      {"1:0:0:0:0:0:0:0", "1::"},
      {"2001:DB8::A", "2001:db8::a"},
      {"fe80::1%eth0", "fe80::1"},
      {"10.0.2.15", "10.0.2.15"},
  };
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    uint8_t a[16];
    char out[40];
    expect(parse_ip(cases[i][0], a));
    net_addr_str(a, out);
    expect_string(out, cases[i][1]);
  }
  static const char *const bad[] = {
      ":::", "1::2::3", "12345::1", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", "::1::", "1:", ":1", "g::1", "", "1.2.3"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    uint8_t a[16];
    expect(!parse_ip(bad[i], a));
  }
}

int main(void)
{
  test("DAD probe, then preferred", test_dad_probe_then_preferred);
  test("DAD conflict disables the address", test_dad_conflict_disables_address);
  test("router advertisement: SLAAC, router, DNS, lifetimes", test_router_advertisement);
  test("prefix with zero lifetime is withdrawn", test_prefix_withdrawn);
  test("neighbor solicitation reply", test_neighbor_solicitation_reply);
  test("MLD query answered", test_mld_query_answered);
  test("fragments in order", test_fragments_in_order);
  test("fragments out of order", test_fragments_out_of_order);
  test("interleaved fragmented datagrams", test_fragments_interleaved);
  test("overlapping fragments are dropped", test_fragment_overlap_dropped);
  test("fragment timeout and bad length", test_fragment_timeout_and_bad_length);
  test("address text round trips", test_addresses_round_trip);
  return 0;
}
