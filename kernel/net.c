/* net.c: minimal IPv4 stack (Ethernet, ARP, IPv4, ICMP, UDP, sockets) over the RTL8139 driver. */

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
#define htons(x) bswap16((uint16_t)(x))
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

static uint16_t udp_checksum(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint32_t len)
{
  uint32_t sum = 0;
  sum += ((uint32_t)src[0] << 8) | src[1];
  sum += ((uint32_t)src[2] << 8) | src[3];
  sum += ((uint32_t)dst[0] << 8) | dst[1];
  sum += ((uint32_t)dst[2] << 8) | dst[3];
  sum += 17;
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

#define ETH_IP 0x0800
#define ETH_ARP 0x0806
#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17

/* Unconfigured (0.0.0.0) until /bin/dhcpcd applies a lease via net_ifconfig(); /24 keeps same_subnet() defined. */
static uint8_t my_ip[4] = {0, 0, 0, 0};
static uint8_t gw_ip[4] = {0, 0, 0, 0};
static uint8_t dns_ip[4] = {0, 0, 0, 0}; /* leased DNS server; no in-kernel reader */
static uint8_t netmask[4] = {255, 255, 255, 0};

#define ARP_CACHE 8
static struct arp_entry {
  uint8_t ip[4];
  uint8_t mac[6];
  bool valid;
} arp_cache[ARP_CACHE];

/* IPs with an outstanding ARP request; only solicited replies are cached. */
#define ARP_PENDING 8
static struct {
  uint8_t ip[4];
  bool valid;
} arp_pending[ARP_PENDING];

static void arp_pending_add(const uint8_t ip[4])
{
  for (int i = 0; i < ARP_PENDING; i++)
    if (arp_pending[i].valid && memcmp(arp_pending[i].ip, ip, 4) == 0)
      return;
  for (int i = 0; i < ARP_PENDING; i++)
    if (!arp_pending[i].valid) {
      memcpy(arp_pending[i].ip, ip, 4);
      arp_pending[i].valid = true;
      return;
    }
  memcpy(arp_pending[0].ip, ip, 4);
  arp_pending[0].valid = true;
}

/* Consume a pending request for `ip`; 1 if one existed. */
static bool arp_pending_take(const uint8_t ip[4])
{
  for (int i = 0; i < ARP_PENDING; i++)
    if (arp_pending[i].valid && memcmp(arp_pending[i].ip, ip, 4) == 0) {
      arp_pending[i].valid = false;
      return true;
    }
  return false;
}

/* Apply an IPv4 config (SYS_IFCONFIG); NULL args are skipped. Flushes the ARP cache. */
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
  for (int i = 0; i < ARP_CACHE; i++)
    arp_cache[i].valid = false;
  for (int i = 0; i < ARP_PENDING; i++)
    arp_pending[i].valid = false;
}

/* Reply slots written by the RX interrupt, polled by the command context. */
static volatile int icmp_got;
static volatile uint16_t icmp_id_rx, icmp_seq_rx;
static volatile uint8_t icmp_from[4];
static volatile uint32_t icmp_send_us; /* set just before the echo goes out  */
static volatile uint32_t icmp_rtt_us;  /* recv timestamp - send, set by IRQ  */

static uint16_t ip_ident = 1;

/* Nonzero inside net_rx_frame; stops arp_resolve from blocking in the IRQ. */
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

static bool arp_lookup(const uint8_t ip[4], uint8_t out[6])
{
  for (int i = 0; i < ARP_CACHE; i++) {
    if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
      memcpy(out, arp_cache[i].mac, 6);
      return true;
    }
  }
  return false;
}

static void arp_cache_put(const uint8_t ip[4], const uint8_t m[6])
{
  int slot = -1;
  for (int i = 0; i < ARP_CACHE; i++) {
    if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
      slot = i;
      break;
    }
    if (slot < 0 && !arp_cache[i].valid)
      slot = i;
  }
  if (slot < 0)
    slot = 0;
  memcpy(arp_cache[slot].ip, ip, 4);
  memcpy(arp_cache[slot].mac, m, 6);
  arp_cache[slot].valid = true;
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
  arp_build(pkt, 1, zero, ip);
  arp_pending_add(ip);
  eth_send(bcast, ETH_ARP, pkt, 28);
}

static void arp_recv(const uint8_t *p, uint32_t len)
{
  if (len < 28)
    return;
  const struct arp_hdr *a = (const struct arp_hdr *)p;
  if (ntohs(a->ptype) != ETH_IP)
    return;

  uint16_t op = ntohs(a->oper);
  if (op == 2) { /* reply: trust it only if solicited */
    if (arp_pending_take(a->spa))
      arp_cache_put(a->spa, a->sha);
  } else if (op == 1 && memcmp(a->tpa, my_ip, 4) == 0) {
    arp_cache_put(a->spa, a->sha); /* someone is asking for us: answer */
    uint8_t pkt[28];
    arp_build(pkt, 2, a->sha, a->spa);
    eth_send(a->sha, ETH_ARP, pkt, 28);
  }
}

/* Resolve an IP to a MAC, sending requests and waiting on the RX interrupt. */
static bool arp_resolve(const uint8_t ip[4], uint8_t out[6])
{
  if (arp_lookup(ip, out))
    return true;
  /* In the RX IRQ never block: send a request and fail; the TCP retransmit resolves later. */
  if (net_rx_depth > 0) {
    arp_request(ip);
    return false;
  }
  for (int tries = 0; tries < 4; tries++) {
    arp_request(ip);
    uint32_t deadline = timer_ticks() + TIMER_HZ / 4;
    while (timer_ticks() < deadline) {
      __asm__ volatile("" ::: "memory");
      if (arp_lookup(ip, out))
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
static int next_hop_mac(const uint8_t dst[4], uint8_t out[6])
{
  if (dst[0] >= 224 && dst[0] <= 239) { /* IPv4 multicast: RFC 1112 MAC map */
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = 0x5e;
    out[3] = dst[1] & 0x7f;
    out[4] = dst[2];
    out[5] = dst[3];
    return 1;
  }
  return arp_resolve(same_subnet(dst) ? dst : gw_ip, out);
}

static void ip_send_ttl(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint32_t plen, uint8_t ttl)
{
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
  memcpy(ip->dst, dst, 4);
  ip->csum = htons(inet_csum(buf, 20));

  memcpy(buf + 20, payload, plen);
  eth_send(nh, ETH_IP, buf, 20 + plen);
}

static void ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint32_t plen)
{
  ip_send_ttl(dst, proto, payload, plen, 64);
}

/* Lower-layer hooks for the transport layer. */
void net_ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint32_t plen)
{
  ip_send(dst, proto, payload, plen);
}

const uint8_t *net_my_ip(void) { return my_ip; }

/* True once a real IPv4 address is held (non-zero my_ip). */
bool net_have_ip(void) { return my_ip[0] || my_ip[1] || my_ip[2] || my_ip[3]; }

/* Client-socket RX delivery (defined with the usock layer below). */
static bool usock_udp_deliver(const uint8_t src_ip[4], uint16_t sport, uint16_t dport, const uint8_t *data,
                              uint32_t len);
static void usock_icmp_deliver(const uint8_t *msg, uint32_t len);
static void usock_raw_deliver(const uint8_t *frame, uint32_t len);

static void icmp_recv(const uint8_t src[4], const uint8_t *p, uint32_t len)
{
  if (len < 8)
    return;
  const struct icmp_hdr *ic = (const struct icmp_hdr *)p;
  if (ic->type == 0) {
    icmp_rtt_us = timer_us() - icmp_send_us;
    icmp_id_rx = ntohs(ic->id);
    icmp_seq_rx = ntohs(ic->seq);
    for (int i = 0; i < 4; i++)
      icmp_from[i] = src[i];
    icmp_got = 1;
    usock_icmp_deliver(p, len);
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

void net_udp_send_ttl(const uint8_t dst[4], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len,
                      uint8_t ttl)
{
  /* Cap at what ip_send's 1500-byte buffer allows (20 IP + 8 UDP headers); QUIC needs >=1200. */
  if (len > 1472)
    len = 1472;
  uint8_t pkt[8 + 1472];
  struct udp_hdr *u = (struct udp_hdr *)pkt;
  u->sport = htons(sport);
  u->dport = htons(dport);
  u->len = htons((uint16_t)(8 + len));
  u->csum = 0;
  for (uint32_t i = 0; i < len; i++)
    pkt[8 + i] = data[i];
  uint16_t csum = udp_checksum(my_ip, dst, pkt, 8 + len);
  u->csum = htons(csum ? csum : 0xFFFF);
  ip_send_ttl(dst, IP_UDP, pkt, 8 + len, ttl);
}

void net_udp_send(const uint8_t dst[4], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len)
{
  net_udp_send_ttl(dst, dport, sport, data, len, 64);
}

static void udp_recv(const uint8_t src[4], const uint8_t *p, uint32_t len)
{
  if (len < 8)
    return;
  const struct udp_hdr *u = (const struct udp_hdr *)p;
  uint32_t ulen = ntohs(u->len);
  if (ulen < 8 || ulen > len)
    return;
  if (u->csum && udp_checksum(src, my_ip, p, ulen) != 0)
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
  if (ip->proto == IP_ICMP)
    icmp_recv(ip->src, payload, plen);
  else if (ip->proto == IP_UDP)
    udp_recv(ip->src, payload, plen);
  else if (ip->proto == IP_TCP)
    tcp_input(ip->src, payload, plen);
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
  uint8_t peer[4];       /* UDP/ICMP destination                        */
  uint16_t lport, rport; /* UDP local / remote ports                    */
  uint8_t ttl;           /* outgoing IP TTL (UDP); mDNS/SSDP need 255   */
  /* Datagram ring (UDP/ICMP): RX IRQ enqueues at dg_tail, recv dequeues at dg_head (empty when equal). */
  volatile uint32_t dg_head, dg_tail;
  uint32_t dg_len[SOCK_DGRING];
  uint8_t dg_src_ip[SOCK_DGRING][4]; /* per-datagram sender, for recvfrom */
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
static bool usock_udp_deliver(const uint8_t src_ip[4], uint16_t sport, uint16_t dport, const uint8_t *data,
                              uint32_t len)
{
  for (int i = 0; i < NET_SOCK_MAX; i++) {
    struct usock *s = &usocks[i];
    int peer_match = s->rport == 0 || (s->rport == sport && memcmp(s->peer, src_ip, 4) == 0);
    if (s->used && s->proto == SK_UDP && s->lport == dport && peer_match) {
      uint32_t next = (s->dg_tail + 1) % SOCK_DGRING;
      if (next == s->dg_head)
        return true;
      if (len > SOCK_DGRAM)
        len = SOCK_DGRAM;
      for (uint32_t k = 0; k < len; k++)
        s->dg[s->dg_tail][k] = data[k];
      s->dg_len[s->dg_tail] = len;
      for (int k = 0; k < 4; k++)
        s->dg_src_ip[s->dg_tail][k] = src_ip[k];
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
static int sock_dgram_recvfrom(struct usock *s, void *buf, uint32_t max, uint8_t *out_ip, uint16_t *out_port)
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
        for (int i = 0; i < 4; i++)
          out_ip[i] = s->dg_src_ip[idx][i];
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
  if (port == 0)
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

  uint8_t ip[4];
  if (!parse_ipv4(host, ip)) {
    s->used = 0;
    return -2;
  }
  for (int i = 0; i < 4; i++)
    s->peer[i] = ip[i];

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
    s->lport = (uint16_t)(0xC000 + h);
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
    return -1;
  struct usock *s = &usocks[h];
  switch (s->proto) {
  case SK_TCP:
    return tcp_write(s->conn, buf, len);
  case SK_UDP:
    if (len > 1472) /* one IPv4/UDP datagram won't hold more */
      return -1;
    net_udp_send_ttl(s->peer, s->rport, s->lport, buf, len, s->ttl);
    return (int)len;
  case SK_ICMP:
    ip_send(s->peer, IP_ICMP, buf, len);
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
    return -1;
  struct usock *s = &usocks[h];
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
    return -1;
  struct usock *s = &usocks[h];
  if (s->proto == SK_UDP || s->proto == SK_ICMP)
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
    return -1;
  struct usock *s = &usocks[h];
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
    return -1;
  if (ttl < 1 || ttl > 255)
    return -1;
  usocks[h].ttl = (uint8_t)ttl;
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
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used || !usocks[h].listening)
    return -1;
  uint16_t port = usocks[h].lport;
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
  int h = usock_alloc();
  if (h < 0)
    return -1;
  usocks[h].proto = SK_UDP;
  usocks[h].lport = (uint16_t)port;
  return h;
}

/* Receive one datagram on a bound UDP socket; out pointers may be NULL. >0, 0 timeout, -1 bad fd, -4 intr. */
int net_sock_recvfrom(int h, void *buf, uint32_t max, uint8_t out_ip[4], uint16_t *out_port)
{
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used || usocks[h].proto != SK_UDP)
    return -1;
  return sock_dgram_recvfrom(&usocks[h], buf, max, out_ip, out_port);
}

/* Send a datagram from a bound UDP socket to ip:port. Returns len or -1. */
int net_sock_sendto(int h, const uint8_t ip[4], int port, const void *buf, uint32_t len)
{
  if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used || usocks[h].proto != SK_UDP)
    return -1;
  if (!ip || (!buf && len) || port < 1 || port > 65535)
    return -1;
  if (len > 1472) /* one IPv4/UDP datagram won't hold more */
    return -1;
  struct usock *s = &usocks[h];
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
  /* No IP is announced here: /bin/dhcpcd applies the lease after sti. */
  con_write("net: requesting address via DHCP\n");
}

static void net_poll_thread(void)
{
  for (;;) {
    nic_poll();
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
