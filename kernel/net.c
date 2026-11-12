/* net.c: minimal dual-stack IP (Ethernet, ARP, NDP, IPv4/IPv6, ICMP/ICMPv6, UDP, sockets). Addresses are 16 bytes; IPv4
 * is v4-mapped. */

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "entropy.h"
#include "fs.h"
#include "gzip.h"
#include "heap.h"
#include "kstring.h"
#include "loader.h"
#include "log.h"
#include "net.h"
#include "nic.h"
#include "sched.h"
#include "tcp.h"
#include "timer.h"

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t bswap32(uint32_t x) { return (x << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24); }
#define htons(x) bswap16((uint16_t)(x))
#define htonl(x) bswap32((uint32_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))

/* Internet checksum (RFC 1071), host value; the csum field must be zero while computing. Even lengths only. */
static uint16_t inet_csum(const uint8_t *p, uint32_t len)
{
  uint32_t sum = 0;
  while (len > 1) {
    sum += ((uint32_t)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len)
    sum += (uint32_t)p[0] << 8;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static const uint8_t v4_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

static bool addr_is_v4(const uint8_t a[16]) { return memcmp(a, v4_prefix, 12) == 0; }

static void addr_map_v4(uint8_t out[16], const uint8_t v4[4])
{
  memcpy(out, v4_prefix, 12);
  memcpy(out + 12, v4, 4);
}

static bool addr_is_link_local6(const uint8_t a[16]) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }

static bool addr_is_mcast6(const uint8_t a[16]) { return a[0] == 0xFF; }

static bool addr_is_unspec(const uint8_t a[16])
{
  for (int i = 0; i < 16; i++)
    if (a[i])
      return false;
  return true;
}

/* UDP/TCP/ICMPv6 checksum over the family's pseudo-header + segment; host value, 0 when valid on receive. */
uint16_t net_l4_csum(const uint8_t src[16], const uint8_t dst[16], uint8_t proto, const uint8_t *seg, uint32_t len)
{
  uint32_t sum = 0;
  bool v4 = addr_is_v4(src) && addr_is_v4(dst);
  const uint8_t *s = v4 ? src + 12 : src, *d = v4 ? dst + 12 : dst;
  for (int i = 0; i < (v4 ? 4 : 16); i += 2) {
    sum += ((uint32_t)s[i] << 8) | s[i + 1];
    sum += ((uint32_t)d[i] << 8) | d[i + 1];
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

struct eth_hdr {
  uint8_t dst[6];
  uint8_t src[6];
  uint16_t type;
} __attribute__((packed));

struct arp_hdr {
  uint16_t htype, ptype;
  uint8_t hlen, plen;
  uint16_t oper;
  uint8_t sha[6], spa[4];
  uint8_t tha[6], tpa[4];
} __attribute__((packed));

struct ip_hdr {
  uint8_t ver_ihl, tos;
  uint16_t len, id, frag;
  uint8_t ttl, proto;
  uint16_t csum;
  uint8_t src[4], dst[4];
} __attribute__((packed));

struct icmp_hdr {
  uint8_t type, code;
  uint16_t csum, id, seq;
} __attribute__((packed));

struct udp_hdr {
  uint16_t sport, dport, len, csum;
} __attribute__((packed));

struct ip6_hdr {
  uint32_t vtf;
  uint16_t plen;
  uint8_t nh, hlim;
  uint8_t src[16], dst[16];
} __attribute__((packed));

#define ETH_IP 0x0800
#define ETH_ARP 0x0806
#define ETH_IP6 0x86DD
#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17
#define IP_ICMP6 58
#define IP6_HBH 0
#define IP6_FRAG 44
#define IP6_DSTOPTS 60

#define ICMP6_ECHO_REPLY 129
#define MLD_QUERY 130
#define MLD_V2_REPORT 143
#define ICMP6_RS 133
#define ICMP6_RA 134
#define ICMP6_NS 135
#define ICMP6_NA 136
#define ND_OPT_SLLA 1
#define ND_OPT_TLLA 2
#define ND_OPT_PREFIX 3
#define ND_OPT_RDNSS 25

/* Unconfigured (0.0.0.0) until /bin/dhcpcd applies a lease via net_ifconfig(); /24 keeps same_subnet() defined. */
static uint8_t my_ip[4] = {0, 0, 0, 0};
static uint8_t gw_ip[4] = {0, 0, 0, 0};
static uint8_t dns_ip[4] = {0, 0, 0, 0}; /* leased DNS server; no in-kernel reader */
static uint8_t netmask[4] = {255, 255, 255, 0};

/* IPv6: link-local from the MAC at net_init; global address, default router and DNS server from router
 * advertisements. Addresses pass duplicate address detection (tentative) before they are used. */
enum { AS_NONE, AS_NEW, AS_TENTATIVE, AS_PREFERRED, AS_DUP };
static uint8_t ll6[16], gua6[16], prefix6[8], gw6[16], dns6[16];
static uint8_t ll6_state, gua6_state;
static uint32_t ll6_dad_end, gua6_dad_end;            /* tick a tentative address becomes preferred */
static uint32_t gua6_expire, gw6_expire, dns6_expire; /* tick the RA-learned item lapses; 0 = never */
static bool have_gw6, have_dns6;

static bool ll6_ok(void) { return ll6_state == AS_PREFERRED; }

static bool gua6_ok(void) { return gua6_state == AS_PREFERRED; }

static bool tick_passed(uint32_t t) { return (int32_t)(timer_ticks() - t) >= 0; }

/* Tick `sec` seconds from now (capped; never 0, which means no expiry). */
static uint32_t tick_after(uint32_t sec)
{
  uint32_t t = timer_ticks() + (sec > 1000000 ? 1000000 : sec) * TIMER_HZ;
  return t ? t : 1;
}

static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Neighbor cache for both families (ARP for v4, NDP for v6); keys are 16-byte addresses. */
#define NBR_CACHE 16
static struct nbr_entry {
  uint8_t ip[16];
  uint8_t mac[6];
  bool valid;
} nbr_cache[NBR_CACHE];

/* Addresses with an outstanding ARP/NS request; only solicited replies are cached. */
#define NBR_PENDING 8
static struct {
  uint8_t ip[16];
  bool valid;
} nbr_pending[NBR_PENDING];

static void nbr_pending_add(const uint8_t ip[16])
{
  for (int i = 0; i < NBR_PENDING; i++)
    if (nbr_pending[i].valid && memcmp(nbr_pending[i].ip, ip, 16) == 0)
      return;
  for (int i = 0; i < NBR_PENDING; i++)
    if (!nbr_pending[i].valid) {
      memcpy(nbr_pending[i].ip, ip, 16);
      nbr_pending[i].valid = true;
      return;
    }
  memcpy(nbr_pending[0].ip, ip, 16);
  nbr_pending[0].valid = true;
}

/* Consume a pending request for `ip`; 1 if one existed. */
static bool nbr_pending_take(const uint8_t ip[16])
{
  for (int i = 0; i < NBR_PENDING; i++)
    if (nbr_pending[i].valid && memcmp(nbr_pending[i].ip, ip, 16) == 0) {
      nbr_pending[i].valid = false;
      return true;
    }
  return false;
}

/* Apply an IPv4 config (SYS_IFCONFIG); NULL args are skipped. Flushes the neighbor cache. */
void net_ifconfig(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4])
{
  if (ip)
    memcpy(my_ip, ip, 4);
  if (mask)
    memcpy(netmask, mask, 4);
  if (gw)
    memcpy(gw_ip, gw, 4);
  if (dns)
    memcpy(dns_ip, dns, 4);
  for (int i = 0; i < NBR_CACHE; i++)
    nbr_cache[i].valid = false;
  for (int i = 0; i < NBR_PENDING; i++)
    nbr_pending[i].valid = false;
}

/* Reply slots written by the RX interrupt, polled by the command context. */
static volatile int icmp_got;
static volatile uint16_t icmp_id_rx, icmp_seq_rx;
static volatile uint8_t icmp_from[4];
static volatile uint32_t icmp_send_us; /* set just before the echo goes out  */
static volatile uint32_t icmp_rtt_us;  /* recv timestamp - send, set by IRQ  */

static uint16_t ip_ident = 1;

/* Nonzero inside net_rx_frame; stops nbr_resolve from blocking in the IRQ. */
static volatile int net_rx_depth;

/* Assemble an Ethernet frame in a stack buffer (senders and the IRQ ARP reply never share one). */
static void eth_send(const uint8_t dst[6], uint16_t type, const uint8_t *payload, uint32_t plen)
{
  if (plen > 1500)
    return;
  uint8_t frame[1514];
  struct eth_hdr *e = (struct eth_hdr *)frame;
  memcpy(e->dst, dst, 6);
  memcpy(e->src, nic_mac(), 6);
  e->type = htons(type);
  memcpy(frame + 14, payload, plen);
  nic_send(frame, 14 + plen);
}

static bool nbr_lookup(const uint8_t ip[16], uint8_t out[6])
{
  for (int i = 0; i < NBR_CACHE; i++) {
    if (nbr_cache[i].valid && memcmp(nbr_cache[i].ip, ip, 16) == 0) {
      memcpy(out, nbr_cache[i].mac, 6);
      return true;
    }
  }
  return false;
}

static void nbr_cache_put(const uint8_t ip[16], const uint8_t m[6])
{
  int slot = -1;
  for (int i = 0; i < NBR_CACHE; i++) {
    if (nbr_cache[i].valid && memcmp(nbr_cache[i].ip, ip, 16) == 0) {
      slot = i;
      break;
    }
    if (slot < 0 && !nbr_cache[i].valid)
      slot = i;
  }
  if (slot < 0)
    slot = 0;
  memcpy(nbr_cache[slot].ip, ip, 16);
  memcpy(nbr_cache[slot].mac, m, 6);
  nbr_cache[slot].valid = true;
}

static void arp_build(uint8_t *pkt, uint16_t oper, const uint8_t tha[6], const uint8_t tpa[4])
{
  struct arp_hdr *a = (struct arp_hdr *)pkt;
  a->htype = htons(1);
  a->ptype = htons(ETH_IP);
  a->hlen = 6;
  a->plen = 4;
  a->oper = htons(oper);
  memcpy(a->sha, nic_mac(), 6);
  memcpy(a->spa, my_ip, 4);
  memcpy(a->tha, tha, 6);
  memcpy(a->tpa, tpa, 4);
}

static void arp_request(const uint8_t ip[4])
{
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t zero[6] = {0};
  uint8_t pkt[28];
  uint8_t key[16];
  arp_build(pkt, 1, zero, ip);
  addr_map_v4(key, ip);
  nbr_pending_add(key);
  eth_send(bcast, ETH_ARP, pkt, 28);
}

static void arp_recv(const uint8_t *p, uint32_t len)
{
  if (len < 28)
    return;
  const struct arp_hdr *a = (const struct arp_hdr *)p;
  if (ntohs(a->ptype) != ETH_IP)
    return;

  uint8_t spa[16];
  addr_map_v4(spa, a->spa);
  uint16_t op = ntohs(a->oper);
  if (op == 2) { /* reply: trust it only if solicited */
    if (nbr_pending_take(spa))
      nbr_cache_put(spa, a->sha);
  } else if (op == 1 && memcmp(a->tpa, my_ip, 4) == 0) {
    nbr_cache_put(spa, a->sha); /* someone is asking for us: answer */
    uint8_t pkt[28];
    arp_build(pkt, 2, a->sha, a->spa);
    eth_send(a->sha, ETH_ARP, pkt, 28);
  }
}

/* Source address for a packet to `dst`: our v4, a global v6 for off-link targets, else link-local. */
void net_src_addr(const uint8_t dst[16], uint8_t out[16])
{
  if (addr_is_v4(dst))
    addr_map_v4(out, my_ip);
  else if (gua6_ok() && !addr_is_link_local6(dst) && !addr_is_mcast6(dst))
    memcpy(out, gua6, 16);
  else
    memcpy(out, ll6, 16);
}

static void ip6_send_from(const uint8_t src[16], const uint8_t dst[16], uint8_t proto, const uint8_t *payload,
                          uint32_t plen, uint8_t hlim);

/* Fill the ICMPv6 checksum of `msg` and send it from src to dst. */
static void icmp6_send(const uint8_t src[16], const uint8_t dst[16], uint8_t *msg, uint32_t len, uint8_t hlim)
{
  msg[2] = msg[3] = 0;
  uint16_t c = net_l4_csum(src, dst, IP_ICMP6, msg, len);
  msg[2] = (uint8_t)(c >> 8);
  msg[3] = (uint8_t)c;
  ip6_send_from(src, dst, IP_ICMP6, msg, len, hlim);
}

/* Solicited-node multicast address of `target` (ff02::1:ffXX:XXXX). */
static void solicited_node(const uint8_t target[16], uint8_t out[16])
{
  memset(out, 0, 16);
  out[0] = 0xFF;
  out[1] = 0x02;
  out[11] = 0x01;
  out[12] = 0xFF;
  memcpy(out + 13, target + 13, 3);
}

static void ns_send(const uint8_t target[16])
{
  uint8_t dst[16], src[16], msg[32] = {0};
  solicited_node(target, dst);
  net_src_addr(target, src);
  msg[0] = ICMP6_NS;
  memcpy(msg + 8, target, 16);
  msg[24] = ND_OPT_SLLA;
  msg[25] = 1;
  memcpy(msg + 26, nic_mac(), 6);
  nbr_pending_add(target);
  icmp6_send(src, dst, msg, sizeof msg, 255);
}

static void rs_send(void)
{
  static const uint8_t all_routers[16] = {0xFF, 0x02, [15] = 0x02};
  uint8_t msg[16] = {0};
  msg[0] = ICMP6_RS;
  msg[8] = ND_OPT_SLLA;
  msg[9] = 1;
  memcpy(msg + 10, nic_mac(), 6);
  icmp6_send(ll6, all_routers, msg, sizeof msg, 255);
}

static const uint8_t unspec6[16];

/* Duplicate address detection probe: a neighbor solicitation from :: for the tentative address. */
static void dad_probe(const uint8_t addr[16])
{
  uint8_t dst[16], msg[24] = {0};
  solicited_node(addr, dst);
  msg[0] = ICMP6_NS;
  memcpy(msg + 8, addr, 16);
  icmp6_send(unspec6, dst, msg, sizeof msg, 255);
}

/* MLDv2 report (RFC 3810) for our solicited-node group, so snooping switches forward neighbor discovery to us.
 * rtype: 4 = joined, 2 = answer to a query. Rides a hop-by-hop header carrying the router alert option. */
static void mld_report(uint8_t rtype)
{
  static const uint8_t mld_routers[16] = {0xFF, 0x02, [15] = 0x16};
  uint8_t pkt[8 + 28] = {IP_ICMP6, 0, 5, 2, 0, 0, 1, 0};
  uint8_t *m = pkt + 8;
  m[0] = MLD_V2_REPORT;
  m[7] = 1; /* one group record */
  m[8] = rtype;
  solicited_node(ll6, m + 12);
  const uint8_t *src = ll6_ok() ? ll6 : unspec6;
  uint16_t c = net_l4_csum(src, mld_routers, IP_ICMP6, m, 28);
  m[2] = (uint8_t)(c >> 8);
  m[3] = (uint8_t)c;
  ip6_send_from(src, mld_routers, IP6_HBH, pkt, sizeof pkt, 1);
}

static void nbr_request(const uint8_t ip[16])
{
  if (addr_is_v4(ip))
    arp_request(ip + 12);
  else
    ns_send(ip);
}

/* Resolve an address to a MAC, sending requests and waiting on the RX interrupt. */
static bool nbr_resolve(const uint8_t ip[16], uint8_t out[6])
{
  if (nbr_lookup(ip, out))
    return true;
  /* In the RX IRQ never block: send a request and fail; the TCP retransmit resolves later. */
  if (net_rx_depth > 0) {
    nbr_request(ip);
    return false;
  }
  for (int tries = 0; tries < 4; tries++) {
    nbr_request(ip);
    uint32_t deadline = timer_ticks() + TIMER_HZ / 4;
    while (timer_ticks() < deadline) {
      __asm__ volatile("" ::: "memory");
      if (nbr_lookup(ip, out))
        return true;
      if (sched_intr_pending()) /* Ctrl+C: give up resolving */
        return false;
      /* sti before hlt: syscall context has IF=0, a bare hlt would never wake. */
      __asm__ volatile("sti\n\thlt");
    }
  }
  return false;
}

static bool same_subnet(const uint8_t ip[4])
{
  for (int i = 0; i < 4; i++)
    if ((ip[i] & netmask[i]) != (my_ip[i] & netmask[i]))
      return false;
  return true;
}

/* Pick the next hop (host directly, or the gateway) and resolve its MAC. */
static int next_hop_mac(const uint8_t dst[16], uint8_t out[6])
{
  if (addr_is_v4(dst)) {
    const uint8_t *d = dst + 12;
    if (d[0] >= 224 && d[0] <= 239) { /* IPv4 multicast: RFC 1112 MAC map */
      out[0] = 0x01;
      out[1] = 0x00;
      out[2] = 0x5e;
      out[3] = d[1] & 0x7f;
      out[4] = d[2];
      out[5] = d[3];
      return 1;
    }
    uint8_t hop[16];
    addr_map_v4(hop, same_subnet(d) ? d : gw_ip);
    return nbr_resolve(hop, out);
  }
  if (addr_is_mcast6(dst)) { /* RFC 2464: 33:33 + low 32 bits */
    out[0] = out[1] = 0x33;
    memcpy(out + 2, dst + 12, 4);
    return 1;
  }
  bool onlink = addr_is_link_local6(dst) || (gua6_ok() && memcmp(dst, prefix6, 8) == 0);
  if (onlink)
    return nbr_resolve(dst, out);
  return have_gw6 && nbr_resolve(gw6, out);
}

static void ip6_send_from(const uint8_t src[16], const uint8_t dst[16], uint8_t proto, const uint8_t *payload,
                          uint32_t plen, uint8_t hlim)
{
  uint8_t nh[6];
  if (!next_hop_mac(dst, nh))
    return;

  uint8_t buf[1500];
  if (40 + plen > sizeof(buf))
    return;
  struct ip6_hdr *ip = (struct ip6_hdr *)buf;
  ip->vtf = htonl(0x60000000);
  ip->plen = htons(plen);
  ip->nh = proto;
  ip->hlim = hlim;
  memcpy(ip->src, src, 16);
  memcpy(ip->dst, dst, 16);
  memcpy(buf + 40, payload, plen);
  eth_send(nh, ETH_IP6, buf, 40 + plen);
}

static void ip_send_ttl(const uint8_t dst[16], uint8_t proto, const uint8_t *payload, uint32_t plen, uint8_t ttl)
{
  if (!addr_is_v4(dst)) {
    uint8_t src[16];
    net_src_addr(dst, src);
    ip6_send_from(src, dst, proto, payload, plen, ttl);
    return;
  }

  uint8_t nh[6];
  if (!next_hop_mac(dst, nh))
    return;

  /* Stack-local: runs in both thread and RX IRQ context. */
  uint8_t buf[1500];
  if (20 + plen > sizeof(buf))
    return;

  struct ip_hdr *ip = (struct ip_hdr *)buf;
  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->len = htons(20 + plen);
  ip->id = htons(ip_ident++);
  ip->frag = htons(0x4000);
  ip->ttl = ttl;
  ip->proto = proto;
  ip->csum = 0;
  memcpy(ip->src, my_ip, 4);
  memcpy(ip->dst, dst + 12, 4);
  ip->csum = htons(inet_csum(buf, 20));

  memcpy(buf + 20, payload, plen);
  eth_send(nh, ETH_IP, buf, 20 + plen);
}

static void ip_send(const uint8_t dst[16], uint8_t proto, const uint8_t *payload, uint32_t plen)
{
  ip_send_ttl(dst, proto, payload, plen, 64);
}

/* Lower-layer hooks for the transport layer. */
void net_ip_send(const uint8_t dst[16], uint8_t proto, const uint8_t *payload, uint32_t plen)
{
  ip_send(dst, proto, payload, plen);
}

/* Largest TCP payload per segment toward `dst` (MTU 1500 minus IP and TCP headers). */
uint32_t net_tcp_mss(const uint8_t dst[16]) { return addr_is_v4(dst) ? 1460 : 1440; }

const uint8_t *net_my_ip(void) { return my_ip; }

/* True once a real IPv4 address is held (non-zero my_ip). */
bool net_have_ip(void) { return my_ip[0] || my_ip[1] || my_ip[2] || my_ip[3]; }

const uint8_t *net_my_ip6(void) { return gua6_ok() ? gua6 : NULL; }

const uint8_t *net_dns6(void) { return have_dns6 ? dns6 : NULL; }

const uint8_t *net_ll_ip6(void) { return ll6; }

/* Client-socket RX delivery (defined with the usock layer below). */
static bool usock_udp_deliver(const uint8_t src_ip[16], uint16_t sport, uint16_t dport, const uint8_t *data,
                              uint32_t len);
static void usock_icmp_deliver(const uint8_t *msg, uint32_t len);
static void usock_raw_deliver(const uint8_t *frame, uint32_t len);

static void icmp_recv(const uint8_t src[16], const uint8_t *p, uint32_t len)
{
  if (len < 8 || inet_csum(p, len) != 0) /* the checksum covers the whole ICMP message */
    return;
  const struct icmp_hdr *ic = (const struct icmp_hdr *)p;
  if (ic->type == 0) {
    icmp_rtt_us = timer_us() - icmp_send_us;
    icmp_id_rx = ntohs(ic->id);
    icmp_seq_rx = ntohs(ic->seq);
    for (int i = 0; i < 4; i++)
      icmp_from[i] = src[12 + i];
    icmp_got = 1;
    usock_icmp_deliver(p, len);
  }
}

/* Walk an ND option list (type, length in 8-byte units) for `want`; returns its body (6-byte MAC or prefix info). */
static const uint8_t *nd_option(const uint8_t *p, uint32_t len, uint8_t want)
{
  while (len >= 8) {
    uint32_t olen = (uint32_t)p[1] * 8;
    if (olen == 0 || olen > len)
      return NULL;
    if (p[0] == want)
      return p + 2;
    p += olen;
    len -= olen;
  }
  return NULL;
}

/* Prefix information body: plen, flags (A=0x40), valid(4), preferred(4), reserved(4), prefix(16). */
static void prefix_opt(const uint8_t *pi)
{
  if (pi[0] != 64 || !(pi[1] & 0x40) || addr_is_link_local6(pi + 14))
    return;
  uint32_t valid = be32(pi + 2);
  bool same = gua6_state != AS_NONE && memcmp(prefix6, pi + 14, 8) == 0;
  if (!valid) {
    if (same)
      gua6_state = AS_NONE;
    return;
  }
  if (!same) {
    memcpy(prefix6, pi + 14, 8);
    memcpy(gua6, pi + 14, 8);
    memcpy(gua6 + 8, ll6 + 8, 8);
    gua6_state = AS_NEW;
  }
  gua6_expire = valid == 0xFFFFFFFF ? 0 : tick_after(valid);
}

static void ra_recv(const uint8_t src[16], const uint8_t *p, uint32_t len)
{
  if (len < 16 || !addr_is_link_local6(src))
    return;
  uint16_t lifetime = (uint16_t)((p[6] << 8) | p[7]);
  if (lifetime) {
    memcpy(gw6, src, 16);
    have_gw6 = true;
    gw6_expire = tick_after(lifetime);
  } else if (have_gw6 && memcmp(gw6, src, 16) == 0) {
    have_gw6 = false;
  }
  const uint8_t *o = p + 16;
  for (uint32_t rem = len - 16; rem >= 8;) {
    uint32_t olen = (uint32_t)o[1] * 8;
    if (olen == 0 || olen > rem)
      break;
    if (o[0] == ND_OPT_SLLA)
      nbr_cache_put(src, o + 2);
    else if (o[0] == ND_OPT_PREFIX && olen >= 32)
      prefix_opt(o + 2);
    else if (o[0] == ND_OPT_RDNSS && olen >= 24) { /* reserved(2), lifetime(4), addresses */
      uint32_t life = be32(o + 4);
      have_dns6 = life != 0;
      if (have_dns6) {
        memcpy(dns6, o + 8, 16);
        dns6_expire = tick_after(life);
      }
    }
    o += olen;
    rem -= olen;
  }
}

/* A tentative address that someone else defends or probes is a duplicate. */
static void dad_conflict(const uint8_t target[16])
{
  if (ll6_state == AS_TENTATIVE && memcmp(target, ll6, 16) == 0)
    ll6_state = AS_DUP;
  else if (gua6_state == AS_TENTATIVE && memcmp(target, gua6, 16) == 0)
    gua6_state = AS_DUP;
}

static void ns_recv(const uint8_t src[16], const uint8_t *p, uint32_t len)
{
  if (len < 24)
    return;
  const uint8_t *target = p + 8;
  if (addr_is_unspec(src)) { /* another node's DAD probe */
    dad_conflict(target);
    return;
  }
  bool ours = (ll6_ok() && memcmp(target, ll6, 16) == 0) || (gua6_ok() && memcmp(target, gua6, 16) == 0);
  if (!ours)
    return;
  const uint8_t *mac = nd_option(p + 24, len - 24, ND_OPT_SLLA);
  if (!mac)
    return;
  nbr_cache_put(src, mac);
  uint8_t msg[32] = {0};
  msg[0] = ICMP6_NA;
  msg[4] = 0x60; /* solicited | override */
  memcpy(msg + 8, target, 16);
  msg[24] = ND_OPT_TLLA;
  msg[25] = 1;
  memcpy(msg + 26, nic_mac(), 6);
  icmp6_send(target, src, msg, sizeof msg, 255);
}

static void na_recv(const uint8_t *p, uint32_t len)
{
  if (len < 24)
    return;
  dad_conflict(p + 8);
  const uint8_t *mac = nd_option(p + 24, len - 24, ND_OPT_TLLA);
  if (mac && nbr_pending_take(p + 8))
    nbr_cache_put(p + 8, mac);
}

static void icmp6_recv(const uint8_t src[16], const uint8_t dst[16], uint8_t hlim, const uint8_t *p, uint32_t len)
{
  if (len < 8 || net_l4_csum(src, dst, IP_ICMP6, p, len) != 0)
    return;
  switch (p[0]) {
  case ICMP6_ECHO_REPLY:
    icmp_rtt_us = timer_us() - icmp_send_us;
    icmp_id_rx = ntohs(((const struct icmp_hdr *)p)->id);
    icmp_seq_rx = ntohs(((const struct icmp_hdr *)p)->seq);
    icmp_got = 1;
    usock_icmp_deliver(p, len);
    break;
  case ICMP6_RA:
    if (hlim == 255)
      ra_recv(src, p, len);
    break;
  case ICMP6_NS:
    if (hlim == 255)
      ns_recv(src, p, len);
    break;
  case ICMP6_NA:
    if (hlim == 255)
      na_recv(p, len);
    break;
  case MLD_QUERY:
    if (hlim == 1 && addr_is_link_local6(src) && (ll6_state == AS_TENTATIVE || ll6_ok()))
      mld_report(2);
    break;
  }
}

/* Registered in-kernel UDP servers (net_udp_listen). */
#define UDP_LISTENERS 8
static struct {
  uint16_t port;
  udp_handler_t fn;
} udp_listeners[UDP_LISTENERS];

static udp_handler_t udp_lookup(uint16_t port)
{
  for (int i = 0; i < UDP_LISTENERS; i++)
    if (udp_listeners[i].fn && udp_listeners[i].port == port)
      return udp_listeners[i].fn;
  return 0;
}

int net_udp_listen(uint16_t port, udp_handler_t fn)
{
  for (int i = 0; i < UDP_LISTENERS; i++)
    if (udp_listeners[i].fn && udp_listeners[i].port == port) {
      udp_listeners[i].fn = fn;
      return 0;
    }
  for (int i = 0; i < UDP_LISTENERS; i++)
    if (!udp_listeners[i].fn) {
      udp_listeners[i].port = port;
      udp_listeners[i].fn = fn;
      return 0;
    }
  return -1;
}

void net_udp_unlisten(uint16_t port)
{
  for (int i = 0; i < UDP_LISTENERS; i++)
    if (udp_listeners[i].fn && udp_listeners[i].port == port)
      udp_listeners[i].fn = 0;
}

/* Largest UDP payload toward `dst`; QUIC needs >=1200. */
static uint32_t udp_max_payload(const uint8_t dst[16]) { return addr_is_v4(dst) ? 1472 : 1452; }

void net_udp_send_ttl(const uint8_t dst[16], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len,
                      uint8_t ttl)
{
  if (len > udp_max_payload(dst))
    len = udp_max_payload(dst);
  uint8_t pkt[8 + 1472];
  struct udp_hdr *u = (struct udp_hdr *)pkt;
  u->sport = htons(sport);
  u->dport = htons(dport);
  u->len = htons((uint16_t)(8 + len));
  u->csum = 0;
  for (uint32_t i = 0; i < len; i++)
    pkt[8 + i] = data[i];
  uint8_t src[16];
  net_src_addr(dst, src);
  uint16_t csum = net_l4_csum(src, dst, IP_UDP, pkt, 8 + len);
  u->csum = htons(csum ? csum : 0xFFFF);
  ip_send_ttl(dst, IP_UDP, pkt, 8 + len, ttl);
}

void net_udp_send(const uint8_t dst[16], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len)
{
  net_udp_send_ttl(dst, dport, sport, data, len, 64);
}

static void udp_recv(const uint8_t src[16], const uint8_t dst[16], const uint8_t *p, uint32_t len)
{
  if (len < 8)
    return;
  const struct udp_hdr *u = (const struct udp_hdr *)p;
  uint32_t ulen = ntohs(u->len);
  if (ulen < 8 || ulen > len)
    return;
  if (!u->csum && !addr_is_v4(src)) /* mandatory over IPv6 */
    return;
  if (u->csum && net_l4_csum(src, dst, IP_UDP, p, ulen) != 0)
    return;
  /* A bound socket gets the datagram. */
  if (usock_udp_deliver(src, ntohs(u->sport), ntohs(u->dport), p + 8, ulen - 8))
    return;
  udp_handler_t fn = udp_lookup(ntohs(u->dport));
  if (fn) {
    fn(src, ntohs(u->sport), p + 8, ulen - 8);
  }
}

/* Accept `dst` if our unicast, broadcast or multicast; everything while unconfigured (DHCP). */
static bool ip_dst_ok(const uint8_t dst[4])
{
  if (!net_have_ip())
    return true;
  if (memcmp(dst, my_ip, 4) == 0)
    return true;
  if (dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255)
    return true;
  if (dst[0] >= 224 && dst[0] <= 239)
    return true;
  int net_match = 1, host_ones = 1;
  for (int i = 0; i < 4; i++) {
    if ((dst[i] & netmask[i]) != (my_ip[i] & netmask[i]))
      net_match = 0;
    if ((uint8_t)(dst[i] | netmask[i]) != 0xFF)
      host_ones = 0;
  }
  return net_match && host_ones;
}

static void ip_recv(const uint8_t *p, uint32_t len)
{
  if (len < 20)
    return;
  const struct ip_hdr *ip = (const struct ip_hdr *)p;
  if ((ip->ver_ihl >> 4) != 4)
    return;
  uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0F) * 4;
  uint32_t total = ntohs(ip->len);
  if (ihl < 20 || ihl > len)
    return;
  if (inet_csum(p, ihl) != 0)
    return;
  /* No reassembly: drop fragments. */
  uint16_t frag = ntohs(ip->frag);
  if ((frag & 0x2000) || (frag & 0x1FFF))
    return;
  if (!ip_dst_ok(ip->dst))
    return;
  if (total < ihl || total > len)
    return;

  const uint8_t *payload = p + ihl;
  uint32_t plen = total - ihl;
  uint8_t src[16], dst[16];
  addr_map_v4(src, ip->src);
  addr_map_v4(dst, ip->dst);
  if (ip->proto == IP_ICMP)
    icmp_recv(src, payload, plen);
  else if (ip->proto == IP_UDP)
    udp_recv(src, dst, payload, plen);
  else if (ip->proto == IP_TCP)
    tcp_input(src, dst, payload, plen);
}

static bool ip6_dst_ok(const uint8_t dst[16])
{
  return addr_is_mcast6(dst) || (ll6_ok() && memcmp(dst, ll6, 16) == 0) || (gua6_ok() && memcmp(dst, gua6, 16) == 0);
}

/* IPv6 reassembly (RFC 8200): a few datagrams at a time, in 8-byte blocks, overlaps dropped (RFC 5722). */
#define FRAG_SLOTS 2
#define FRAG_MAX 4096
#define FRAG_TIMEOUT (5 * TIMER_HZ)
static struct frag6 {
  bool used, have_first;
  uint8_t src[16], dst[16], nh;
  uint32_t id, total, started; /* total is 0 until the last fragment arrives */
  uint8_t seen[FRAG_MAX / 8 / 8];
  uint8_t buf[FRAG_MAX];
} frags[FRAG_SLOTS];

static void ip6_dispatch(const uint8_t src[16], const uint8_t dst[16], uint8_t hlim, uint8_t nh, const uint8_t *payload,
                         uint32_t plen);

/* `fh` is the 8-byte fragment header, data/dlen the fragment's payload. */
static void frag6_input(const uint8_t src[16], const uint8_t dst[16], uint8_t hlim, const uint8_t *fh,
                        const uint8_t *data, uint32_t dlen)
{
  uint32_t off = (uint32_t)((fh[2] << 8) | (fh[3] & 0xF8));
  bool more = fh[3] & 1;
  uint32_t id = be32(fh + 4);
  if (dlen == 0 || (more && (dlen & 7)) || off + dlen > FRAG_MAX)
    return;

  struct frag6 *f = NULL;
  for (int i = 0; i < FRAG_SLOTS; i++)
    if (frags[i].used && frags[i].id == id && memcmp(frags[i].src, src, 16) == 0 && memcmp(frags[i].dst, dst, 16) == 0)
      f = &frags[i];
  if (!f) { /* a free slot, else evict the oldest */
    f = &frags[0];
    for (int i = 0; i < FRAG_SLOTS; i++) {
      if (!frags[i].used) {
        f = &frags[i];
        break;
      }
      if ((int32_t)(f->started - frags[i].started) > 0)
        f = &frags[i];
    }
    memset(f->seen, 0, sizeof f->seen);
    f->used = true;
    f->have_first = false;
    f->total = 0;
    f->id = id;
    f->started = timer_ticks();
    memcpy(f->src, src, 16);
    memcpy(f->dst, dst, 16);
  }

  uint32_t first = off / 8, last = (off + dlen + 7) / 8;
  for (uint32_t b = first; b < last; b++)
    if (f->seen[b / 8] & (1u << (b % 8))) {
      f->used = false;
      return;
    }
  for (uint32_t b = first; b < last; b++)
    f->seen[b / 8] |= (uint8_t)(1u << (b % 8));
  memcpy(f->buf + off, data, dlen);
  if (off == 0) {
    f->have_first = true;
    f->nh = fh[0];
  }
  if (!more)
    f->total = off + dlen;
  if (!f->total || !f->have_first)
    return;
  for (uint32_t b = 0; b < (f->total + 7) / 8; b++)
    if (!(f->seen[b / 8] & (1u << (b % 8))))
      return;
  f->used = false;
  if (f->nh != IP6_FRAG)
    ip6_dispatch(src, dst, hlim, f->nh, f->buf, f->total);
}

static void ip6_dispatch(const uint8_t src[16], const uint8_t dst[16], uint8_t hlim, uint8_t nh, const uint8_t *payload,
                         uint32_t plen)
{
  if (nh == IP_ICMP6)
    icmp6_recv(src, dst, hlim, payload, plen);
  else if (nh == IP_UDP)
    udp_recv(src, dst, payload, plen);
  else if (nh == IP_TCP)
    tcp_input(src, dst, payload, plen);
  else if (nh == IP6_FRAG && plen > 8)
    frag6_input(src, dst, hlim, payload, payload + 8, plen - 8);
}

static void ip6_recv(const uint8_t *p, uint32_t len)
{
  if (len < 40)
    return;
  const struct ip6_hdr *ip = (const struct ip6_hdr *)p;
  if ((p[0] >> 4) != 6 || !ip6_dst_ok(ip->dst))
    return;
  uint32_t plen = ntohs(ip->plen);
  if (40 + plen > len)
    return;

  const uint8_t *payload = p + 40;
  uint8_t nh = ip->nh;
  while (nh == IP6_HBH || nh == IP6_DSTOPTS) { /* skip option headers */
    if (plen < 8)
      return;
    uint32_t hl = ((uint32_t)payload[1] + 1) * 8;
    if (hl > plen)
      return;
    nh = payload[0];
    payload += hl;
    plen -= hl;
  }
  ip6_dispatch(ip->src, ip->dst, ip->hlim, nh, payload, plen);
}

/* The NIC driver hands every inbound Ethernet frame (FCS stripped) here. */
void net_rx_frame(const uint8_t *f, uint32_t len)
{
  if (len < 14)
    return;
  net_rx_depth++;
  entropy_stir(len);
  usock_raw_deliver(f, len);
  const struct eth_hdr *e = (const struct eth_hdr *)f;
  uint16_t type = ntohs(e->type);
  if (type == ETH_ARP)
    arp_recv(f + 14, len - 14);
  else if (type == ETH_IP)
    ip_recv(f + 14, len - 14);
  else if (type == ETH_IP6)
    ip6_recv(f + 14, len - 14);
  net_rx_depth--;
}

static int parse_ipv4(const char *s, uint8_t out[4])
{
  for (int i = 0; i < 4; i++) {
    if (*s < '0' || *s > '9')
      return 0;
    int v = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (*s++ - '0');
      if (++digits > 3 || v > 255)
        return 0;
    }
    out[i] = (uint8_t)v;
    if (i < 3) {
      if (*s != '.')
        return 0;
      s++;
    }
  }
  return *s == '\0';
}

static int hex_digit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c |= 32;
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Parse an IPv6 literal (optional "%zone" suffix, ignored) into 16 bytes. */
static int parse_ipv6(const char *s, uint8_t out[16])
{
  uint16_t head[8], tail[8];
  int nh = 0, nt = 0;
  bool gap = false;
  if (s[0] == ':') {
    if (s[1] != ':')
      return 0;
    gap = true;
    s += 2;
  }
  while (*s && *s != '%') {
    uint32_t v = 0;
    int digits = 0;
    while (hex_digit(*s) >= 0) {
      v = v * 16 + (uint32_t)hex_digit(*s++);
      if (++digits > 4)
        return 0;
    }
    if (!digits || nh + nt >= 8)
      return 0;
    if (gap)
      tail[nt++] = (uint16_t)v;
    else
      head[nh++] = (uint16_t)v;
    if (*s == ':') {
      s++;
      if (*s == ':') {
        if (gap)
          return 0;
        gap = true;
        s++;
      } else if (!*s || *s == '%') {
        return 0;
      }
    } else if (*s && *s != '%') {
      return 0;
    }
  }
  if (gap ? nh + nt > 7 : nh != 8)
    return 0;
  memset(out, 0, 16);
  for (int i = 0; i < nh; i++) {
    out[2 * i] = (uint8_t)(head[i] >> 8);
    out[2 * i + 1] = (uint8_t)head[i];
  }
  for (int i = 0; i < nt; i++) {
    out[16 - 2 * (nt - i)] = (uint8_t)(tail[i] >> 8);
    out[16 - 2 * (nt - i) + 1] = (uint8_t)tail[i];
  }
  return 1;
}

/* Parse an IPv4 or IPv6 literal into a 16-byte (v4-mapped) address. */
static int parse_ip(const char *s, uint8_t out[16])
{
  uint8_t v4[4];
  if (parse_ipv4(s, v4)) {
    addr_map_v4(out, v4);
    return 1;
  }
  return parse_ipv6(s, out);
}

/* Format an address into out (>= 40 bytes): dotted quad for v4-mapped, RFC 5952 for v6. Returns the length. */
int net_addr_str(const uint8_t a[16], char *out)
{
  int n = 0;
  if (addr_is_v4(a)) {
    for (int i = 12; i < 16; i++) {
      if (a[i] >= 100)
        out[n++] = (char)('0' + a[i] / 100);
      if (a[i] >= 10)
        out[n++] = (char)('0' + a[i] / 10 % 10);
      out[n++] = (char)('0' + a[i] % 10);
      if (i < 15)
        out[n++] = '.';
    }
    out[n] = '\0';
    return n;
  }
  int best = -1, best_len = 1;
  for (int i = 0; i < 8;) {
    int j = i;
    while (j < 8 && !a[2 * j] && !a[2 * j + 1])
      j++;
    if (j - i > best_len)
      best = i, best_len = j - i;
    i = j > i ? j : i + 1;
  }
  for (int i = 0; i < 8; i++) {
    if (i == best) {
      out[n++] = ':';
      if (i == 0)
        out[n++] = ':';
      i += best_len - 1;
      continue;
    }
    uint32_t g = ((uint32_t)a[2 * i] << 8) | a[2 * i + 1];
    int shift = g > 0xFFF ? 12 : g > 0xFF ? 8 : g > 0xF ? 4 : 0;
    for (; shift >= 0; shift -= 4)
      out[n++] = "0123456789abcdef"[(g >> shift) & 0xF];
    if (i < 7)
      out[n++] = ':';
  }
  out[n] = '\0';
  return n;
}

/* Socket handles: a small connection table user programs drive via connect/send/recv/close. */
#define NET_SOCK_MAX 8
#define SOCK_DGRAM 1500 /* one datagram's worth of rx per UDP/ICMP socket */
#define SOCK_DGRING 16  /* datagrams buffered per UDP/ICMP socket         */
#define RAW_FRAME 1514  /* one full Ethernet frame (SK_RAWETH rx)         */
#define RAW_RING 8      /* frames buffered per raw socket                 */
static struct usock {
  uint8_t used;
  uint8_t proto;         /* SK_TCP / SK_UDP / SK_ICMP                    */
  uint8_t closed;        /* peer FIN already seen (TCP)                  */
  uint8_t listening;     /* TCP passive-open slot (accept()s on lport)   */
  uint32_t owner;        /* task that opened the handle                  */
  tcp_conn_t *conn;      /* bare-TCP connection (TCP only)              */
  uint8_t peer[16];      /* UDP/ICMP destination (v4-mapped for IPv4)   */
  uint16_t lport, rport; /* UDP local / remote ports                    */
  uint8_t ttl;           /* outgoing IP TTL (UDP); mDNS/SSDP need 255   */
  /* Datagram ring (UDP/ICMP): RX IRQ enqueues at dg_tail, recv dequeues at dg_head (empty when equal). */
  volatile uint32_t dg_head, dg_tail;
  uint32_t dg_len[SOCK_DGRING];
  uint8_t dg_src_ip[SOCK_DGRING][16]; /* per-datagram sender, for recvfrom */
  uint16_t dg_src_port[SOCK_DGRING];
  uint8_t dg[SOCK_DGRING][SOCK_DGRAM];
  /* Raw Ethernet ring (SK_RAWETH): separate from dg[] because a frame exceeds SOCK_DGRAM. */
  volatile uint32_t rf_head, rf_tail;
  uint32_t rf_len[RAW_RING];
  uint8_t rf[RAW_RING][RAW_FRAME];
} usocks[NET_SOCK_MAX];

/* Copy an inbound frame into every open raw socket's ring (RX IRQ); drops when full. */
static void usock_raw_deliver(const uint8_t *frame, uint32_t len)
{
  if (len > RAW_FRAME)
    len = RAW_FRAME;
  for (int i = 0; i < NET_SOCK_MAX; i++) {
    struct usock *s = &usocks[i];
    if (!s->used || s->proto != SK_RAWETH)
      continue;
    uint32_t next = (s->rf_tail + 1) % RAW_RING;
    if (next == s->rf_head)
      continue;
    for (uint32_t k = 0; k < len; k++)
      s->rf[s->rf_tail][k] = frame[k];
    s->rf_len[s->rf_tail] = len;
    s->rf_tail = next;
  }
}

/* Deliver a UDP datagram to the socket on its local port (RX IRQ); 1 if claimed. */
static bool usock_udp_deliver(const uint8_t src_ip[16], uint16_t sport, uint16_t dport, const uint8_t *data,
                              uint32_t len)
{
  for (int i = 0; i < NET_SOCK_MAX; i++) {
    struct usock *s = &usocks[i];
    int peer_match = s->rport == 0 || (s->rport == sport && memcmp(s->peer, src_ip, 16) == 0);
    if (s->used && s->proto == SK_UDP && s->lport == dport && peer_match) {
      uint32_t next = (s->dg_tail + 1) % SOCK_DGRING;
      if (next == s->dg_head)
        return true;
      if (len > SOCK_DGRAM)
        len = SOCK_DGRAM;
      for (uint32_t k = 0; k < len; k++)
        s->dg[s->dg_tail][k] = data[k];
      s->dg_len[s->dg_tail] = len;
      memcpy(s->dg_src_ip[s->dg_tail], src_ip, 16);
      s->dg_src_port[s->dg_tail] = sport;
      s->dg_tail = next;
      return true;
    }
  }
  return false;
}

/* Deliver an ICMP message to a raw ICMP socket (RX IRQ). */
static void usock_icmp_deliver(const uint8_t *msg, uint32_t len)
{
  for (int i = 0; i < NET_SOCK_MAX; i++) {
    struct usock *s = &usocks[i];
    if (s->used && s->proto == SK_ICMP) {
      uint32_t next = (s->dg_tail + 1) % SOCK_DGRING;
      if (next == s->dg_head)
        return;
      if (len > SOCK_DGRAM)
        len = SOCK_DGRAM;
      for (uint32_t k = 0; k < len; k++)
        s->dg[s->dg_tail][k] = msg[k];
      s->dg_len[s->dg_tail] = len;
      s->dg_tail = next;
      return;
    }
  }
}

/* Block ~2 s for a datagram; out_ip/out_port may be NULL. >0 bytes, 0 timeout, -4 intr. */
/* Is a UDP socket other than `except` already on local port `port`? Delivery goes to the first match. */
static bool udp_port_busy(uint16_t port, int except)
{
  for (int i = 0; i < NET_SOCK_MAX; i++)
    if (i != except && usocks[i].used && usocks[i].proto == SK_UDP && usocks[i].lport == port)
      return true;
  return false;
}

/* A random free port in the IANA ephemeral range, so an off-path attacker cannot guess where replies go. */
static uint16_t udp_ephemeral(int h)
{
  for (int tries = 0; tries < 64; tries++) {
    uint16_t r;
    entropy_get(&r, sizeof r);
    uint16_t port = (uint16_t)(0xC000u | (r & 0x3FFFu));
    if (!udp_port_busy(port, h))
      return port;
  }
  for (uint32_t port = 0xC000u; port <= 0xFFFFu; port++)
    if (!udp_port_busy((uint16_t)port, h))
      return (uint16_t)port;
  return 0;
}

/* Handle `h` if it is open and belongs to the calling process; handles are global, so this is the access check. */
static struct usock *sock_get(int h)
{
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used || usocks[h].owner != sched_current_process_id())
    return NULL;
  return &usocks[h];
}

static int sock_dgram_recvfrom(struct usock *s, void *buf, uint32_t max, uint8_t out_ip[16], uint16_t *out_port)
{
  uint32_t deadline = timer_ticks() + 2 * TIMER_HZ;
  for (;;) {
    if (s->dg_head != s->dg_tail) {
      __asm__ volatile("cli");
      uint32_t idx = s->dg_head;
      uint32_t n = s->dg_len[idx];
      if (n > max)
        n = max;
      for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = s->dg[idx][i];
      if (out_ip)
        memcpy(out_ip, s->dg_src_ip[idx], 16);
      if (out_port)
        *out_port = s->dg_src_port[idx];
      s->dg_head = (idx + 1) % SOCK_DGRING;
      __asm__ volatile("sti");
      return (int)n;
    }
    if (sched_intr_pending())
      return -4;
    if (timer_ticks() > deadline)
      return 0;
    __asm__ volatile("sti\n\thlt");
  }
}

static int sock_dgram_recv(struct usock *s, void *buf, uint32_t max)
{
  return sock_dgram_recvfrom(s, buf, max, NULL, NULL);
}

/* The next queued datagram, or 0 if none: never waits. */
static int sock_dgram_recv_nb(struct usock *s, void *buf, uint32_t max)
{
  if (s->dg_head == s->dg_tail)
    return sched_intr_pending() ? -4 : 0;
  return sock_dgram_recvfrom(s, buf, max, NULL, NULL);
}

/* Dequeue one raw frame, waiting ~2 s if `block`: >0 bytes, 0 timeout or empty, -4 intr. */
static int sock_raw_recv(struct usock *s, void *buf, uint32_t max, int block)
{
  uint32_t deadline = timer_ticks() + 2 * TIMER_HZ;
  for (;;) {
    if (s->rf_head != s->rf_tail) {
      __asm__ volatile("cli");
      uint32_t idx = s->rf_head;
      uint32_t n = s->rf_len[idx];
      if (n > max)
        n = max;
      for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = s->rf[idx][i];
      s->rf_head = (idx + 1) % RAW_RING;
      __asm__ volatile("sti");
      return (int)n;
    }
    if (!block)
      return 0;
    if (sched_intr_pending())
      return -4;
    if (timer_ticks() > deadline)
      return 0;
    __asm__ volatile("sti\n\thlt");
  }
}

/* Open a raw Ethernet socket (root-gated in the syscall layer). Handle or -1. */
int net_raw_open(void)
{
  if (!nic_present())
    return -1;
  int h = -1;
  for (int i = 0; i < NET_SOCK_MAX; i++)
    if (!usocks[i].used) {
      h = i;
      break;
    }
  if (h < 0)
    return -1;
  struct usock *s = &usocks[h];
  s->used = 1;
  s->owner = sched_current_process_id();
  s->proto = SK_RAWETH;
  s->closed = s->listening = 0;
  s->conn = NULL;
  s->rf_head = s->rf_tail = 0;
  return h;
}

/* Open a TCP/UDP/raw-ICMP socket to IP literal `host`: handle, or -1 slot/NIC, -2 bad IP, -3 connect, -4 intr. */
int net_sock_connect(const char *host, uint16_t port, int proto)
{
  if (proto == SK_RAWETH)
    return net_raw_open();
  if (!nic_present())
    return -1;
  if (port == 0 && proto != SK_ICMP)
    return -1;
  int h = -1;
  for (int i = 0; i < NET_SOCK_MAX; i++)
    if (!usocks[i].used) {
      h = i;
      break;
    }
  if (h < 0)
    return -1;

  /* Reserve the slot before the blocking connect so another thread can't claim it. */
  struct usock *s = &usocks[h];
  s->used = 1;
  s->owner = sched_current_process_id();
  s->proto = (uint8_t)proto;
  s->closed = 0;
  s->listening = 0;
  s->conn = NULL;
  s->dg_head = s->dg_tail = 0;
  s->ttl = 64;

  uint8_t ip[16];
  if (!parse_ip(host, ip)) {
    s->used = 0;
    return -2;
  }
  memcpy(s->peer, ip, 16);

  switch (proto) {
  case SK_TCP:
    s->conn = tcp_connect(ip, port);
    if (!s->conn) {
      s->used = 0;
      return sched_intr_pending() ? -4 : -3;
    }
    break;
  case SK_UDP:
    s->rport = port;
    s->lport = udp_ephemeral(h);
    if (!s->lport) {
      s->used = 0;
      return -1;
    }
    break;
  case SK_ICMP:
    s->lport = s->rport = 0;
    break;
  default:
    s->used = 0;
    return -1;
  }
  return h;
}

int net_sock_send(int h, const void *buf, uint32_t len)
{
  struct usock *s = sock_get(h);
  if (!s)
    return -1;
  switch (s->proto) {
  case SK_TCP:
    return tcp_write(s->conn, buf, len);
  case SK_UDP:
    if (len > udp_max_payload(s->peer)) /* one datagram won't hold more */
      return -1;
    net_udp_send_ttl(s->peer, s->rport, s->lport, buf, len, s->ttl);
    return (int)len;
  case SK_ICMP:
    if (addr_is_v4(s->peer)) {
      ip_send(s->peer, IP_ICMP, buf, len);
    } else {
      /* ICMPv6 carries a pseudo-header checksum the program can't know: fill it in. */
      uint8_t msg[1440], src[16];
      if (len < 8 || len > sizeof msg)
        return -1;
      memcpy(msg, buf, len);
      net_src_addr(s->peer, src);
      icmp6_send(src, s->peer, msg, len, 64);
    }
    return (int)len;
  case SK_RAWETH:
    if (len < 14 || len > RAW_FRAME)
      return -1;
    nic_send((const uint8_t *)buf, len);
    return (int)len;
  }
  return -1;
}

/* Block for data. TCP: >0, 0 EOF, -1 error, -4 intr. UDP/ICMP: >0, 0 timeout, -4 intr. */
int net_sock_recv(int h, void *buf, uint32_t max)
{
  struct usock *s = sock_get(h);
  if (!s)
    return -1;
  if (s->proto == SK_UDP || s->proto == SK_ICMP)
    return sock_dgram_recv(s, buf, max);
  if (s->proto == SK_RAWETH)
    return sock_raw_recv(s, buf, max, 1);
  if (s->closed)
    return 0;
  int closed = 0;
  int r = tcp_read(s->conn, buf, max, &closed);
  if (r == TCP_EINTR)
    return -4;
  if (r < 0)
    return -1;
  if (closed)
    s->closed = 1;
  return r;
}

/* Non-blocking recv. TCP: >0, 0 would-block, -1 error/EOF, -4 intr. Skips the blocking path's idle auto-close. */
int net_sock_recv_nb(int h, void *buf, uint32_t max)
{
  struct usock *s = sock_get(h);
  if (!s)
    return -1;
  if (s->proto == SK_UDP)
    return sock_dgram_recv_nb(s, buf, max);
  if (s->proto == SK_ICMP)
    return sock_dgram_recv(s, buf, max);
  if (s->proto == SK_RAWETH)
    return sock_raw_recv(s, buf, max, 0);
  if (s->closed)
    return -1;
  if (sched_intr_pending())
    return -4;
  int closed = 0;
  int r = tcp_read_nb(s->conn, buf, max, &closed);
  if (r < 0) {
    s->closed = 1;
    return -1;
  }
  if (closed) {
    s->closed = 1;
    return r > 0 ? r : -1; /* hand back the last bytes, then EOF      */
  }
  return r; /* >0 data, or 0 = would-block          */
}

int net_sock_close(int h)
{
  struct usock *s = sock_get(h);
  if (!s)
    return -1;
  if (s->listening)
    tcp_unlisten(s->lport);
  else if (s->proto == SK_TCP && s->conn)
    tcp_close(s->conn);
  s->used = s->closed = s->listening = 0;
  s->owner = 0;
  s->conn = NULL;
  return 0;
}

/* Set the outgoing IP TTL for a UDP socket (the IP_MULTICAST_TTL analog). */
int net_sock_set_ttl(int h, int ttl)
{
  struct usock *s = sock_get(h);
  if (!s || ttl < 1 || ttl > 255)
    return -1;
  s->ttl = (uint8_t)ttl;
  return 0;
}

/* Server side of the handle table: TCP listen/accept and bound UDP sockets. */

static int usock_alloc(void)
{
  for (int i = 0; i < NET_SOCK_MAX; i++)
    if (!usocks[i].used) {
      usocks[i].used = 1;
      usocks[i].owner = sched_current_process_id();
      usocks[i].closed = usocks[i].listening = 0;
      usocks[i].conn = NULL;
      usocks[i].dg_head = usocks[i].dg_tail = 0;
      usocks[i].ttl = 64;
      return i;
    }
  return -1;
}

/* Open a passive TCP listener on `port`. Handle or -1 (no NIC/slot). */
int net_sock_listen(int port)
{
  if (!nic_present() || port < 1 || port > 65535)
    return -1;
  int h = usock_alloc();
  if (h < 0)
    return -1;
  struct usock *s = &usocks[h];
  s->proto = SK_TCP;
  s->listening = 1;
  s->lport = (uint16_t)port;
  if (tcp_listen((uint16_t)port) < 0) {
    s->used = s->listening = 0;
    return -1;
  }
  return h;
}

/* Block until a client connects or Ctrl+C. New handle, or -1 bad fd, -4 interrupted. */
int net_sock_accept(int h)
{
  struct usock *ls = sock_get(h);
  if (!ls || !ls->listening)
    return -1;
  uint16_t port = ls->lport;
  tcp_conn_t *c = tcp_accept(port);
  if (!c)
    return -4;
  int nh = usock_alloc();
  if (nh < 0) {
    tcp_close(c);
    return -1;
  }
  usocks[nh].proto = SK_TCP;
  usocks[nh].owner = sched_current_process_id();
  usocks[nh].conn = c;
  return nh;
}

/* Bind a UDP socket to `port`. Handle or -1 (no NIC/slot). */
int net_sock_bind(int port)
{
  if (!nic_present() || port < 1 || port > 65535)
    return -1;
  if (udp_port_busy((uint16_t)port, -1)) /* a second binder would silently steal the first's datagrams */
    return -1;
  int h = usock_alloc();
  if (h < 0)
    return -1;
  usocks[h].proto = SK_UDP;
  usocks[h].lport = (uint16_t)port;
  return h;
}

/* Receive one datagram on a bound UDP socket; out pointers may be NULL. >0, 0 timeout, -1 bad fd, -4 intr. */
int net_sock_recvfrom(int h, void *buf, uint32_t max, uint8_t out_ip[16], uint16_t *out_port)
{
  struct usock *s = sock_get(h);
  if (!s || s->proto != SK_UDP)
    return -1;
  return sock_dgram_recvfrom(s, buf, max, out_ip, out_port);
}

/* Send a datagram from a bound UDP socket to ip:port. Returns len or -1. */
int net_sock_sendto(int h, const uint8_t ip[16], int port, const void *buf, uint32_t len)
{
  struct usock *s = sock_get(h);
  if (!s || s->proto != SK_UDP)
    return -1;
  if (!ip || (!buf && len) || port < 1 || port > 65535)
    return -1;
  if (len > udp_max_payload(ip)) /* one datagram won't hold more */
    return -1;
  net_udp_send_ttl(ip, (uint16_t)port, s->lport, buf, len, s->ttl);
  return (int)len;
}

void net_sock_close_owner(uint32_t owner)
{
  for (int i = 0; i < NET_SOCK_MAX; i++)
    if (usocks[i].used && usocks[i].owner == owner)
      net_sock_close(i);
}

void net_init(void)
{
  nic_init();
  if (!nic_present())
    return;
  ll6[0] = 0xFE;
  ll6[1] = 0x80;
  memcpy(ll6 + 8, nic_mac(), 3);
  memcpy(ll6 + 13, nic_mac() + 3, 3);
  ll6[8] ^= 0x02; /* EUI-64: flip the universal/local bit, insert ff:fe */
  ll6[11] = 0xFF;
  ll6[12] = 0xFE;
  ll6_state = AS_NEW;
  /* No IP is announced here: /bin/dhcpcd applies the lease after sti. */
  con_write("net: requesting address via DHCP\n");
}

/* Advance one address through duplicate address detection; true if its state changed. */
static bool dad_step(uint8_t *state, uint32_t *end, const uint8_t addr[16])
{
  if (*state == AS_NEW) {
    *state = AS_TENTATIVE;
    *end = timer_ticks() + TIMER_HZ;
    dad_probe(addr);
    return true;
  }
  if (*state == AS_TENTATIVE && tick_passed(*end)) {
    *state = AS_PREFERRED;
    return true;
  }
  return false;
}

/* Once a second or so from the receive thread: DAD, lifetimes, router solicitations, reassembly timeouts. */
static void net_tick(void)
{
  static uint32_t rs_next;
  static int rs_sent;
  if (dad_step(&ll6_state, &ll6_dad_end, ll6))
    mld_report(4);
  dad_step(&gua6_state, &gua6_dad_end, gua6);

  if (gua6_state != AS_NONE && gua6_expire && tick_passed(gua6_expire))
    gua6_state = AS_NONE;
  if (have_dns6 && tick_passed(dns6_expire))
    have_dns6 = false;
  if (have_gw6 && tick_passed(gw6_expire)) {
    have_gw6 = false;
    rs_sent = 0;
  }
  for (int i = 0; i < FRAG_SLOTS; i++)
    if (frags[i].used && (int32_t)(timer_ticks() - frags[i].started) > FRAG_TIMEOUT)
      frags[i].used = false;
  tcp_tick();

  /* Router solicitations every 4 s until a router answers (max 5). */
  if (ll6_ok() && !have_gw6 && rs_sent < 5 && tick_passed(rs_next)) {
    rs_send();
    rs_sent++;
    rs_next = timer_ticks() + 4 * TIMER_HZ;
  }
}

static void net_poll_thread(void)
{
  for (;;) {
    nic_poll();
    net_tick();
    timer_sleep(1);
  }
}

void net_start(void)
{
  if (nic_present())
    sched_create_thread("netrx", net_poll_thread);
}

/* Launch /bin/dhcpcd detached as root (post-sti, IF=1). No-op without a NIC or the program. */
void net_dhcp_spawn(void)
{
  if (!nic_present())
    return;
  int sz = fs_size("/bin/dhcpcd");
  uint8_t *buf = sz > 0 ? kmalloc((uint32_t)sz) : 0;
  if (!buf)
    return;
  int n = fs_read("/bin/dhcpcd", buf, (uint32_t)sz);
  char *argv[1] = {"dhcpcd"};
  if (n > 0)
    exec_program(buf, (uint32_t)n, 1, argv);
  kfree(buf);
}
