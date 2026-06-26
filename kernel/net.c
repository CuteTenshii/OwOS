/* net.c — a minimal IPv4 protocol stack on top of the RTL8139 driver.
 *
 * Just enough plumbing to ping a host through QEMU's user-mode (SLIRP)
 * network: Ethernet -> ARP -> IPv4 -> ICMP echo, plus a tiny UDP+DNS resolver
 * so a name like "google.com" can be looked up. The NIC itself lives in
 * rtl8139.c; this file only assembles frames (nic_send) and parses the ones
 * the driver hands up (net_rx_frame).
 *
 * SLIRP presents a fixed virtual network, so the IP configuration is hardcoded
 * rather than discovered over DHCP:
 *
 *     guest  10.0.2.15      gateway 10.0.2.2      DNS 10.0.2.3      /24
 *
 * Inbound frames are parsed in interrupt context (the RX path fills small
 * reply slots that the command-context senders below poll on).
 */

#include <stdint.h>
#include <stddef.h>

#include "net.h"
#include "nic.h"
#include "tcp.h"
#include "tls.h"
#include "timer.h"
#include "console.h"
#include "log.h"
#include "fs.h"
#include "kstring.h"
#include "url.h"
#include "gzip.h"
#include "heap.h"
#include "sched.h"
#include "rtc.h"

/* ------------------------------------------------------------------ helpers */

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
#define htons(x) bswap16((uint16_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))

/* Internet checksum (RFC 1071). Reads bytes as big-endian 16-bit words and
 * returns the host value of the checksum; store it into a header field with
 * htons() so the bytes land in network order. The csum field must be zero
 * while computing. Callers only ever pass even lengths here. */
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

/* --------------------------------------------------------------- on-the-wire */

struct eth_hdr {
	uint8_t  dst[6];
	uint8_t  src[6];
	uint16_t type;
} __attribute__((packed));

struct arp_hdr {
	uint16_t htype, ptype;
	uint8_t  hlen, plen;
	uint16_t oper;
	uint8_t  sha[6], spa[4];
	uint8_t  tha[6], tpa[4];
} __attribute__((packed));

struct ip_hdr {
	uint8_t  ver_ihl, tos;
	uint16_t len, id, frag;
	uint8_t  ttl, proto;
	uint16_t csum;
	uint8_t  src[4], dst[4];
} __attribute__((packed));

struct icmp_hdr {
	uint8_t  type, code;
	uint16_t csum, id, seq;
} __attribute__((packed));

struct udp_hdr {
	uint16_t sport, dport, len, csum;
} __attribute__((packed));

struct dns_hdr {
	uint16_t id, flags, qd, an, ns, ar;
} __attribute__((packed));

#define ETH_IP   0x0800
#define ETH_ARP  0x0806
#define IP_ICMP  1
#define IP_TCP   6
#define IP_UDP   17

/* ------------------------------------------------------------------- config */

/* Unconfigured until DHCP leases an address: 0.0.0.0 means "no address held", so
 * the taskbar/sysfs don't claim an IP the machine doesn't yet own. The DHCP
 * handshake sources from a literal 0.0.0.0 (see dhcp_xmit) and so doesn't read
 * these; it fills them in on success. Netmask stays /24 to keep same_subnet()
 * well-defined before a lease. */
static uint8_t my_ip[4]   = { 0, 0, 0, 0 };
static uint8_t gw_ip[4]   = { 0, 0, 0, 0 };
static uint8_t dns_ip[4]  = { 0, 0, 0, 0 };
static uint8_t netmask[4] = { 255, 255, 255, 0 };

#define ARP_CACHE 8
static struct arp_entry {
	uint8_t ip[4];
	uint8_t mac[6];
	int     valid;
} arp_cache[ARP_CACHE];

/* Reply slots written by the RX interrupt, polled by the command context. */
static volatile int      icmp_got;
static volatile uint16_t icmp_id_rx, icmp_seq_rx;
static volatile uint8_t  icmp_from[4];
static volatile uint32_t icmp_send_us;  /* set just before the echo goes out  */
static volatile uint32_t icmp_rtt_us;   /* recv timestamp - send, set by IRQ  */

static volatile int      dns_got, dns_ok;             /* dns_ok: saw an A record */
static volatile uint16_t dns_qid;
static volatile uint8_t  dns_result[4];               /* first A record (resolve) */
static volatile uint16_t dns_ans_type[DNS_MAX_ANS];   /* each answer's RR type */
static volatile uint32_t dns_ans_ttl[DNS_MAX_ANS];
static volatile char     dns_ans_text[DNS_MAX_ANS][DNS_TEXT]; /* formatted rdata */
static volatile int      dns_ans_count;
static volatile uint32_t dns_send_us, dns_rtt_us;     /* query timing          */

/* NTP reply slot: the server's 32-bit transmit timestamp (seconds since the
 * 1900 epoch), written by the RX interrupt and polled by net_ntp. */
static volatile int      ntp_got;
static volatile uint32_t ntp_secs;

static uint16_t ip_ident = 1;

/* ----------------------------------------------------------------- transmit */

/* Assemble an Ethernet frame on the stack and hand it to the driver. The
 * stack-local buffer keeps the command-context senders and the IRQ-context
 * ARP reply from ever sharing one. */
static void eth_send(const uint8_t dst[6], uint16_t type,
                     const uint8_t *payload, uint32_t plen)
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

/* ---------------------------------------------------------------------- ARP */

static int arp_lookup(const uint8_t ip[4], uint8_t out[6])
{
	for (int i = 0; i < ARP_CACHE; i++) {
		if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
			memcpy(out, arp_cache[i].mac, 6);
			return 1;
		}
	}
	return 0;
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
	arp_cache[slot].valid = 1;
}

static void arp_build(uint8_t *pkt, uint16_t oper,
                      const uint8_t tha[6], const uint8_t tpa[4])
{
	struct arp_hdr *a = (struct arp_hdr *)pkt;
	a->htype = htons(1);
	a->ptype = htons(ETH_IP);
	a->hlen  = 6;
	a->plen  = 4;
	a->oper  = htons(oper);
	memcpy(a->sha, nic_mac(), 6);
	memcpy(a->spa, my_ip, 4);
	memcpy(a->tha, tha, 6);
	memcpy(a->tpa, tpa, 4);
}

static void arp_request(const uint8_t ip[4])
{
	static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	static const uint8_t zero[6]  = { 0 };
	uint8_t pkt[28];
	arp_build(pkt, 1, zero, ip);
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
	if (op == 2) {                          /* reply: learn the sender         */
		arp_cache_put(a->spa, a->sha);
	} else if (op == 1 && memcmp(a->tpa, my_ip, 4) == 0) {
		arp_cache_put(a->spa, a->sha);  /* someone is asking for us: answer */
		uint8_t pkt[28];
		arp_build(pkt, 2, a->sha, a->spa);
		eth_send(a->sha, ETH_ARP, pkt, 28);
	}
}

/* Resolve an IP to a MAC, sending requests and waiting on the RX interrupt. */
static int arp_resolve(const uint8_t ip[4], uint8_t out[6])
{
	if (arp_lookup(ip, out))
		return 1;
	for (int tries = 0; tries < 4; tries++) {
		arp_request(ip);
		uint32_t deadline = timer_ticks() + TIMER_HZ / 4;   /* ~250 ms        */
		while (timer_ticks() < deadline) {
			__asm__ volatile ("" ::: "memory");
			if (arp_lookup(ip, out))
				return 1;
			if (sched_intr_pending())    /* Ctrl+C: give up resolving */
				return 0;
			/* sti before hlt: this resolve can run in syscall context (a
			 * a program connecting through net_sock_*), where the int
			 * 0x80 gate left IF=0 — a bare hlt would halt the CPU forever. */
			__asm__ volatile ("sti\n\thlt");
		}
	}
	return 0;
}

static int same_subnet(const uint8_t ip[4])
{
	for (int i = 0; i < 4; i++)
		if ((ip[i] & netmask[i]) != (my_ip[i] & netmask[i]))
			return 0;
	return 1;
}

/* Pick the next hop (host directly, or the gateway) and resolve its MAC. */
static int next_hop_mac(const uint8_t dst[4], uint8_t out[6])
{
	return arp_resolve(same_subnet(dst) ? dst : gw_ip, out);
}

/* ----------------------------------------------------------------------- IP */

static void ip_send(const uint8_t dst[4], uint8_t proto,
                    const uint8_t *payload, uint32_t plen)
{
	uint8_t nh[6];
	if (!next_hop_mac(dst, nh))
		return;

	static uint8_t buf[1500];
	if (20 + plen > sizeof(buf))
		return;

	struct ip_hdr *ip = (struct ip_hdr *)buf;
	ip->ver_ihl = 0x45;
	ip->tos     = 0;
	ip->len     = htons(20 + plen);
	ip->id      = htons(ip_ident++);
	ip->frag    = htons(0x4000);            /* don't fragment                  */
	ip->ttl     = 64;
	ip->proto   = proto;
	ip->csum    = 0;
	memcpy(ip->src, my_ip, 4);
	memcpy(ip->dst, dst, 4);
	ip->csum    = htons(inet_csum(buf, 20));

	memcpy(buf + 20, payload, plen);
	eth_send(nh, ETH_IP, buf, 20 + plen);
}

/* Lower-layer hooks for the transport layer. */
void net_ip_send(const uint8_t dst[4], uint8_t proto,
                 const uint8_t *payload, uint32_t plen)
{
	ip_send(dst, proto, payload, plen);
}

const uint8_t *net_my_ip(void)
{
	return my_ip;
}

/* True once we hold a real IPv4 address (a DHCP lease landed, or a static config
 * was applied). 0.0.0.0 is never a valid host address, so a non-zero my_ip is an
 * exact "configured" test -- callers use it to distinguish "link up" from
 * "actually has an address" instead of conflating the two. */
int net_have_ip(void)
{
	return my_ip[0] || my_ip[1] || my_ip[2] || my_ip[3];
}

/* --------------------------------------------------------------------- ICMP */

/* Client-socket RX delivery (defined with the usock layer below). */
static int  usock_udp_deliver(uint16_t dport, const uint8_t *data, uint32_t len);
static void usock_icmp_deliver(const uint8_t *msg, uint32_t len);

static void icmp_send_echo(const uint8_t dst[4], uint16_t id, uint16_t seq)
{
	uint8_t pkt[8 + 32];
	struct icmp_hdr *ic = (struct icmp_hdr *)pkt;
	ic->type = 8;
	ic->code = 0;
	ic->csum = 0;
	ic->id   = htons(id);
	ic->seq  = htons(seq);
	for (int i = 0; i < 32; i++)
		pkt[8 + i] = (uint8_t)('a' + (i % 26));
	ic->csum = htons(inet_csum(pkt, sizeof(pkt)));
	ip_send(dst, IP_ICMP, pkt, sizeof(pkt));
}

static void icmp_recv(const uint8_t src[4], const uint8_t *p, uint32_t len)
{
	if (len < 8)
		return;
	const struct icmp_hdr *ic = (const struct icmp_hdr *)p;
	if (ic->type == 0) {                    /* echo reply                      */
		icmp_rtt_us = timer_us() - icmp_send_us;  /* sample at arrival   */
		icmp_id_rx  = ntohs(ic->id);
		icmp_seq_rx = ntohs(ic->seq);
		for (int i = 0; i < 4; i++)
			icmp_from[i] = src[i];
		icmp_got = 1;
		usock_icmp_deliver(p, len);     /* hand the raw message to a raw socket */
	}
}

/* ---------------------------------------------------------------- UDP / DNS */

/* Walk past a DNS name (label sequence or compression pointer). Returns the
 * position just after the name, or NULL if it runs off the end. */
static const uint8_t *dns_skip_name(const uint8_t *cur, const uint8_t *end)
{
	while (cur < end) {
		uint8_t b = *cur;
		if (b == 0)
			return cur + 1;
		if ((b & 0xC0) == 0xC0)         /* pointer ends the name in 2 bytes */
			return cur + 2 <= end ? cur + 2 : NULL;
		cur += 1 + b;
	}
	return NULL;
}

/* Decode a (possibly compressed) DNS name at `pos` into a dotted string.
 * `msg`..`end` bound the whole message so compression pointers can be chased. */
static void dns_name_to_str(const uint8_t *msg, const uint8_t *end,
                            const uint8_t *pos, char *out, int outsz)
{
	int oi = 0, jumps = 0;
	const uint8_t *cur = pos;
	while (cur < end) {
		uint8_t b = *cur;
		if (b == 0)
			break;
		if ((b & 0xC0) == 0xC0) {               /* compression pointer    */
			if (cur + 1 >= end)
				break;
			uint32_t off = ((uint32_t)(b & 0x3F) << 8) | cur[1];
			if (msg + off >= end || ++jumps > 16)
				break;
			cur = msg + off;
			continue;
		}
		if (oi > 0 && oi < outsz - 1)
			out[oi++] = '.';
		for (int i = 0; i < b && cur + 1 + i < end; i++)
			if (oi < outsz - 1)
				out[oi++] = (char)cur[1 + i];
		cur += 1 + b;
	}
	if (oi == 0 && oi < outsz - 1)
		out[oi++] = '.';                        /* the root name          */
	out[oi] = '\0';
}

static int u32_to_str(uint32_t v, char *out)
{
	char tmp[12];
	int n = 0;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	out[n] = '\0';
	return n;
}

static void dns_recv(const uint8_t *p, uint32_t len)
{
	if (len < 12)
		return;
	const struct dns_hdr *h = (const struct dns_hdr *)p;
	if (ntohs(h->id) != dns_qid)
		return;

	uint16_t qd = ntohs(h->qd), an = ntohs(h->an);
	const uint8_t *cur = p + 12;
	const uint8_t *end = p + len;

	for (int i = 0; i < qd; i++) {          /* skip the echoed questions       */
		cur = dns_skip_name(cur, end);
		if (!cur || cur + 4 > end)
			return;
		cur += 4;
	}
	int count = 0, saw_a = 0;
	for (int i = 0; i < an && count < DNS_MAX_ANS; i++) {
		cur = dns_skip_name(cur, end);
		if (!cur || cur + 10 > end)
			break;
		uint16_t type  = ((uint16_t)cur[0] << 8) | cur[1];
		uint32_t ttl   = ((uint32_t)cur[4] << 24) | ((uint32_t)cur[5] << 16) |
		                 ((uint32_t)cur[6] << 8) | cur[7];
		uint16_t rdlen = ((uint16_t)cur[8] << 8) | cur[9];
		const uint8_t *rd = cur + 10;
		cur = rd;
		if (cur + rdlen > end)
			break;

		/* Format the record data as text according to its type. */
		char txt[DNS_TEXT];
		int t = 0;
		if (type == 1 && rdlen == 4) {                  /* A               */
			t += u32_to_str(rd[0], txt + t); txt[t++] = '.';
			t += u32_to_str(rd[1], txt + t); txt[t++] = '.';
			t += u32_to_str(rd[2], txt + t); txt[t++] = '.';
			t += u32_to_str(rd[3], txt + t);
			if (!saw_a) {
				for (int k = 0; k < 4; k++)
					dns_result[k] = rd[k];
				saw_a = 1;
			}
		} else if (type == 28 && rdlen == 16) {         /* AAAA            */
			static const char hx[] = "0123456789abcdef";
			for (int g = 0; g < 8; g++) {
				if (g) txt[t++] = ':';
				uint8_t hi = rd[g * 2], lo = rd[g * 2 + 1];
				txt[t++] = hx[hi >> 4]; txt[t++] = hx[hi & 15];
				txt[t++] = hx[lo >> 4]; txt[t++] = hx[lo & 15];
			}
			txt[t] = '\0';
		} else if (type == 5 || type == 2 || type == 12) { /* CNAME/NS/PTR */
			dns_name_to_str(p, end, rd, txt, sizeof(txt));
		} else if (type == 15 && rdlen >= 3) {          /* MX              */
			uint32_t pref = ((uint32_t)rd[0] << 8) | rd[1];
			t += u32_to_str(pref, txt + t);
			txt[t++] = ' ';
			dns_name_to_str(p, end, rd + 2, txt + t, (int)sizeof(txt) - t);
		} else if (type == 16) {                        /* TXT             */
			const uint8_t *q = rd, *re = rd + rdlen;
			while (q < re && t < (int)sizeof(txt) - 1) {
				int sl = *q++;
				for (int k = 0; k < sl && q < re && t < (int)sizeof(txt) - 1; k++)
					txt[t++] = (char)*q++;
			}
			txt[t] = '\0';
		} else {                                        /* unknown         */
			t  = (int)(txt[0] = 't', 1);
			txt[t++] = 'y'; txt[t++] = 'p'; txt[t++] = 'e'; txt[t++] = ' ';
			t += u32_to_str(type, txt + t);
		}

		for (int k = 0; k < DNS_TEXT; k++) {
			dns_ans_text[count][k] = txt[k];
			if (!txt[k])
				break;
		}
		dns_ans_text[count][DNS_TEXT - 1] = '\0';
		dns_ans_type[count] = type;
		dns_ans_ttl[count]  = ttl;
		count++;
		cur += rdlen;
	}
	dns_ans_count = count;
	dns_ok      = saw_a;
	dns_rtt_us  = timer_us() - dns_send_us;   /* sample at arrival           */
	dns_got     = 1;
}

/* Registered UDP servers (kernel/services.c). Looked up by destination port. */
#define UDP_LISTENERS 8
static struct { uint16_t port; udp_handler_t fn; } udp_listeners[UDP_LISTENERS];

static udp_handler_t udp_lookup(uint16_t port)
{
	for (int i = 0; i < UDP_LISTENERS; i++)
		if (udp_listeners[i].fn && udp_listeners[i].port == port)
			return udp_listeners[i].fn;
	return 0;
}

int net_udp_listen(uint16_t port, udp_handler_t fn)
{
	for (int i = 0; i < UDP_LISTENERS; i++)        /* re-bind an existing port */
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

void net_udp_send(const uint8_t dst[4], uint16_t dport, uint16_t sport,
                  const uint8_t *data, uint32_t len)
{
	/* Cap at the IPv4 payload that ip_send's 1500-byte buffer allows (20 IP +
	 * 8 UDP headers). The old 512 limit predated any large-datagram user; QUIC's
	 * Initial datagram must be >=1200 bytes, so a full-MTU cap is required. */
	if (len > 1472)
		len = 1472;
	uint8_t pkt[8 + 1472];
	struct udp_hdr *u = (struct udp_hdr *)pkt;
	u->sport = htons(sport);
	u->dport = htons(dport);
	u->len   = htons((uint16_t)(8 + len));
	u->csum  = 0;                           /* checksum optional over IPv4      */
	for (uint32_t i = 0; i < len; i++)
		pkt[8 + i] = data[i];
	ip_send(dst, IP_UDP, pkt, 8 + len);
}

/* Pull the transmit timestamp out of a 48-byte NTP/SNTP reply (server -> us).
 * Only the integer seconds (offset 40, big-endian) matter for a wall clock. */
static void ntp_recv(const uint8_t *p, uint32_t len)
{
	if (len < 48)
		return;
	ntp_secs = ((uint32_t)p[40] << 24) | ((uint32_t)p[41] << 16) |
	           ((uint32_t)p[42] << 8)  |  (uint32_t)p[43];
	ntp_got  = 1;
}

static void udp_recv(const uint8_t src[4], const uint8_t *p, uint32_t len)
{
	if (len < 8)
		return;
	const struct udp_hdr *u = (const struct udp_hdr *)p;
	uint32_t ulen = ntohs(u->len);                 /* trust the smaller length     */
	if (ulen < 8 || ulen > len)
		ulen = len;
	/* A client socket (resolver, dig, ...) bound to this local port wins
	 * first — otherwise the kernel's own sport==53 resolver would eat its reply. */
	if (usock_udp_deliver(ntohs(u->dport), p + 8, ulen - 8))
		return;
	if (ntohs(u->sport) == 53) {
		dns_recv(p + 8, len - 8);
		return;
	}
	if (ntohs(u->sport) == 123) {
		ntp_recv(p + 8, len - 8);
		return;
	}
	udp_handler_t fn = udp_lookup(ntohs(u->dport));
	if (fn) {
		fn(src, ntohs(u->sport), p + 8, ulen - 8);
	}
}

#define DNS_SPORT 0xC053

static void dns_send_query(uint16_t id, const char *host, uint16_t qtype)
{
	uint8_t udp[8 + 512];
	struct udp_hdr *u = (struct udp_hdr *)udp;
	uint8_t *dns = udp + 8;

	struct dns_hdr *h = (struct dns_hdr *)dns;
	h->id    = htons(id);
	h->flags = htons(0x0100);               /* standard query, recursion       */
	h->qd    = htons(1);
	h->an = 0; h->ns = 0; h->ar = 0;

	uint8_t *q = dns + 12;
	const char *s = host;
	while (*s) {                            /* encode labels                   */
		int l = 0;
		while (s[l] && s[l] != '.')
			l++;
		if (l > 63)
			return;
		*q++ = (uint8_t)l;
		for (int i = 0; i < l; i++)
			*q++ = (uint8_t)s[i];
		s += l;
		if (*s == '.')
			s++;
	}
	*q++ = 0;                               /* root label                      */
	*q++ = (uint8_t)(qtype >> 8);           /* QTYPE                           */
	*q++ = (uint8_t)qtype;
	*q++ = 0; *q++ = 1;                     /* QCLASS = IN                      */

	uint32_t dlen = (uint32_t)(q - dns);
	u->sport = htons(DNS_SPORT);
	u->dport = htons(53);
	u->len   = htons(8 + dlen);
	u->csum  = 0;                           /* optional for IPv4               */

	ip_send(dns_ip, IP_UDP, udp, 8 + dlen);
}

/* ----------------------------------------------------------- receive bottom */

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
	if (total > len)
		total = len;
	if (total < ihl)
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
	const struct eth_hdr *e = (const struct eth_hdr *)f;
	uint16_t type = ntohs(e->type);
	if (type == ETH_ARP)
		arp_recv(f + 14, len - 14);
	else if (type == ETH_IP)
		ip_recv(f + 14, len - 14);
}

/* ------------------------------------------------------------------ command */

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

static void print_ip(const uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		con_write_dec(ip[i]);
		if (i < 3)
			con_putc('.');
	}
}

/* --------------------------------------------------------------------- DHCP */
/* A minimal DHCP client: DISCOVER -> OFFER -> REQUEST -> ACK, then apply the
 * lease (IP/mask/gateway/DNS). Needed on a real link like a tethered phone,
 * which is on its own subnet (e.g. 192.168.42.x) rather than QEMU's SLIRP. The
 * client runs in its own thread so it can block while the USB/NIC RX path (a
 * different thread) feeds replies in through dhcp_recv. */

static volatile int dhcp_state;        /* 0 init, 2 offered, 4 acked            */
static uint8_t dhcp_xid[4]      = { 'O', 'w', 'O', 'S' };
static uint8_t dhcp_offered[4];
static uint8_t dhcp_server[4];
static uint8_t dhcp_mask[4]     = { 255, 255, 255, 0 };
static uint8_t dhcp_router[4];
static uint8_t dhcp_dns[4];
static int     dhcp_have_router, dhcp_have_dns;

/* Build a BOOTP/DHCP message (type 1=DISCOVER, 3=REQUEST) into out[]. Returns
 * its length. */
static uint32_t dhcp_build(uint8_t msgtype, uint8_t *out)
{
	for (int i = 0; i < 300; i++) out[i] = 0;
	out[0] = 1; out[1] = 1; out[2] = 6;          /* op=request, ethernet, hlen 6 */
	memcpy(out + 4, dhcp_xid, 4);               /* transaction id               */
	out[10] = 0x80;                              /* flags: broadcast the reply   */
	memcpy(out + 28, nic_mac(), 6);             /* chaddr                       */
	out[236] = 0x63; out[237] = 0x82;            /* magic cookie 63 82 53 63     */
	out[238] = 0x53; out[239] = 0x63;
	int o = 240;
	out[o++] = 53; out[o++] = 1; out[o++] = msgtype;          /* message type    */
	if (msgtype == 3) {                          /* REQUEST carries the choices  */
		out[o++] = 50; out[o++] = 4; memcpy(out + o, dhcp_offered, 4); o += 4;
		out[o++] = 54; out[o++] = 4; memcpy(out + o, dhcp_server, 4);  o += 4;
	}
	out[o++] = 55; out[o++] = 3;                 /* parameter request list       */
	out[o++] = 1;  out[o++] = 3; out[o++] = 6;   /* mask, router, DNS            */
	out[o++] = 255;                              /* end                          */
	return (uint32_t)o;
}

/* Broadcast a DHCP message (src 0.0.0.0:68 -> 255.255.255.255:67), built raw
 * because ip_send would stamp our (not-yet-assigned) IP and try to ARP it. */
static void dhcp_xmit(const uint8_t *dhcp, uint32_t dlen)
{
	static const uint8_t bmac[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	uint8_t buf[20 + 8 + 300];
	struct ip_hdr  *ip = (struct ip_hdr *)buf;
	struct udp_hdr *u  = (struct udp_hdr *)(buf + 20);
	uint32_t ulen = 8 + dlen;

	ip->ver_ihl = 0x45; ip->tos = 0; ip->len = htons(20 + ulen);
	ip->id = htons(ip_ident++); ip->frag = htons(0x4000);
	ip->ttl = 64; ip->proto = IP_UDP; ip->csum = 0;
	for (int i = 0; i < 4; i++) { ip->src[i] = 0; ip->dst[i] = 0xff; }
	ip->csum = htons(inet_csum(buf, 20));
	u->sport = htons(68); u->dport = htons(67); u->len = htons(ulen); u->csum = 0;
	memcpy(buf + 28, dhcp, dlen);
	eth_send(bmac, ETH_IP, buf, 20 + ulen);
}

/* RX handler bound to UDP port 68: parse an OFFER or ACK and stash the lease. */
static void dhcp_recv(const uint8_t src[4], uint16_t sport,
                      const uint8_t *p, uint32_t len)
{
	(void)src; (void)sport;
	if (len < 240 || memcmp(p + 4, dhcp_xid, 4) != 0)
		return;
	uint8_t mtype = 0;
	for (uint32_t i = 240; i + 1 < len; ) {
		uint8_t opt = p[i++];
		if (opt == 0) continue;                  /* pad                          */
		if (opt == 255) break;                   /* end                          */
		uint8_t olen = p[i++];
		if (i + olen > len) break;
		if (opt == 53 && olen >= 1)      mtype = p[i];
		else if (opt == 1 && olen >= 4)  memcpy(dhcp_mask, p + i, 4);
		else if (opt == 3 && olen >= 4){ memcpy(dhcp_router, p + i, 4); dhcp_have_router = 1; }
		else if (opt == 6 && olen >= 4){ memcpy(dhcp_dns, p + i, 4);    dhcp_have_dns = 1; }
		else if (opt == 54 && olen >= 4) memcpy(dhcp_server, p + i, 4);
		i += olen;
	}
	memcpy(dhcp_offered, p + 16, 4);            /* yiaddr                       */
	if (mtype == 2 && dhcp_state < 2) dhcp_state = 2;       /* OFFER             */
	else if (mtype == 5)              dhcp_state = 4;       /* ACK               */
}

/* Run the DHCP exchange and apply the lease. Returns 0 on success. */
static int dhcp_run(void)
{
	uint8_t pkt[300];

	/* Wait for the physical link before broadcasting: real copper Ethernet takes
	 * a couple of seconds to autonegotiate, and DISCOVERs sent into a down link
	 * are simply lost (the cause of "no lease" on a freshly-woken I219). Bounded
	 * (~8s); virtual NICs report up immediately and fall straight through. */
	for (int w = 0; w < 800 && !nic_link_up(); w++)
		timer_sleep(10);

	dhcp_state = 0;
	net_udp_listen(68, dhcp_recv);

	for (int t = 0; t < 6 && dhcp_state < 2; t++) {        /* DISCOVER          */
		dhcp_xmit(pkt, dhcp_build(1, pkt));
		for (int w = 0; w < 60 && dhcp_state < 2; w++)
			timer_sleep(10);                              /* ~600ms per try    */
	}
	if (dhcp_state < 2) { net_udp_unlisten(68); return -1; }

	for (int t = 0; t < 6 && dhcp_state < 4; t++) {        /* REQUEST           */
		dhcp_xmit(pkt, dhcp_build(3, pkt));
		for (int w = 0; w < 60 && dhcp_state < 4; w++)
			timer_sleep(10);
	}
	net_udp_unlisten(68);
	if (dhcp_state < 4)
		return -1;

	memcpy(my_ip, dhcp_offered, 4);
	memcpy(netmask, dhcp_mask, 4);
	if (dhcp_have_router) memcpy(gw_ip, dhcp_router, 4);
	if (dhcp_have_dns)    memcpy(dns_ip, dhcp_dns, 4);
	for (int i = 0; i < ARP_CACHE; i++)          /* subnet changed              */
		arp_cache[i].valid = 0;
	return 0;
}

/* Format an IP into the journal (not the screen unless verbose) -- the klog
 * counterpart of print_ip, for the async boot path below. */
static void klog_ip(const uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		klog_dec(ip[i]);
		if (i < 3)
			klog(".");
	}
}

static void dhcp_thread(void)
{
	/* This is the boot lease, completing asynchronously in its own thread --
	 * possibly long after the shell owns the console. Log it to the journal
	 * (shown live only under `verbose`) rather than con_write, so it doesn't
	 * land in the middle of the user's terminal. The `dhcp` command path
	 * (net_dhcp_now) still prints normally; the user asked for that one. */
	if (dhcp_run() == 0) {
		klog("dhcp: ip ");  klog_ip(my_ip);
		klog(" gw ");       klog_ip(gw_ip);
		klog(" dns ");      klog_ip(dns_ip);
		klog("\n");
	} else {
		klog("dhcp: no lease\n");
	}
	for (;;)                                     /* threads can't be reaped     */
		timer_sleep(60000);
}

/* Kick off DHCP on a freshly-up link: at boot once a PCI NIC is found, and again
 * when the RNDIS tethering NIC registers. No-op with no NIC so a netless boot
 * doesn't spawn a thread that just times out. On QEMU's SLIRP this re-leases the
 * same 10.0.2.15 the static defaults already use; on a real link (VMware NAT, a
 * tethered phone, bare-metal Ethernet) it's the only way to get a routable IP. */
void net_dhcp_start(void)
{
	if (!nic_present())
		return;
	sched_create_thread("dhcp", dhcp_thread);
}

/* Re-run DHCP synchronously (the `dhcp` shell command): for retrying a lease by
 * hand once the link is known up. Prints the same result line as the boot path
 * and returns 0 on success. */
int net_dhcp_now(void)
{
	if (!nic_present()) {
		con_write("dhcp: no NIC\n");
		return -1;
	}
	if (dhcp_run() == 0) {
		con_write("dhcp: ip ");  print_ip(my_ip);
		con_write(" gw ");       print_ip(gw_ip);
		con_write(" dns ");      print_ip(dns_ip);
		con_putc('\n');
		return 0;
	}
	con_write(nic_link_up() ? "dhcp: no lease (link up)\n"
	                        : "dhcp: no lease (link down)\n");
	return -1;
}

/* Print a microsecond duration as milliseconds with 3 decimals (e.g. 1.234). */
static void print_rtt(uint32_t us)
{
	uint32_t frac = us % 1000;
	con_write_dec(us / 1000);
	con_putc('.');
	con_putc('0' + (char)(frac / 100));
	con_putc('0' + (char)((frac / 10) % 10));
	con_putc('0' + (char)(frac % 10));
}

/* Send a query of the given type for `host` and wait for the reply, retrying a
 * few times. Returns 1 if any response arrived (inspect dns_ans_count / the
 * dns_* answer slots for what it carried). */
static int dns_query(const char *host, uint16_t qtype)
{
	if (!nic_present())
		return 0;

	/* Warm the route to the DNS server so the send isn't ARP-blocked and the
	 * query-time measurement reflects the server round-trip, not ARP. */
	uint8_t warm[6];
	next_hop_mac(dns_ip, warm);

	dns_qid = (uint16_t)(timer_ticks() * 2654435761u) | 1;
	for (int tries = 0; tries < 3; tries++) {
		dns_got = 0;
		dns_ok  = 0;
		dns_ans_count = 0;
		dns_send_us = timer_us();
		dns_send_query(dns_qid, host, qtype);
		uint32_t deadline = timer_ticks() + 2 * TIMER_HZ;   /* ~2 s            */
		while (timer_ticks() < deadline) {
			__asm__ volatile ("" ::: "memory");
			if (dns_got)
				break;
			if (sched_intr_pending())     /* Ctrl+C: give up the lookup */
				return 0;
			__asm__ volatile ("sti\n\thlt");  /* IF=0 in syscall ctx; see arp_resolve */
		}
		if (dns_got)
			return 1;
	}
	return 0;
}

static int dns_resolve(const char *host, uint8_t out[4])
{
	if (!dns_query(host, 1) || !dns_ok)        /* type 1 = A                 */
		return 0;
	for (int i = 0; i < 4; i++)
		out[i] = dns_result[i];
	return 1;
}

static int dns_streq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb)
			return 0;
		a++; b++;
	}
	return *a == *b;
}

static uint16_t dig_type_num(const char *s)
{
	if (dns_streq_ci(s, "A"))     return 1;
	if (dns_streq_ci(s, "NS"))    return 2;
	if (dns_streq_ci(s, "CNAME")) return 5;
	if (dns_streq_ci(s, "SOA"))   return 6;
	if (dns_streq_ci(s, "PTR"))   return 12;
	if (dns_streq_ci(s, "MX"))    return 15;
	if (dns_streq_ci(s, "TXT"))   return 16;
	if (dns_streq_ci(s, "AAAA"))  return 28;
	return 0;
}

static const char *dig_type_name(uint16_t t)
{
	switch (t) {
	case 1:  return "A";
	case 2:  return "NS";
	case 5:  return "CNAME";
	case 6:  return "SOA";
	case 12: return "PTR";
	case 15: return "MX";
	case 16: return "TXT";
	case 28: return "AAAA";
	default: return "?";
	}
}

/* dig <host> [TYPE] — query DNS and print the answer records. Defaults to A. */
int net_dig(const char *arg)
{
	if (!nic_present()) {
		con_write("dig: no network device\n");
		return -1;
	}

	char host[128];
	int n = 0;
	while (*arg == ' ')
		arg++;
	while (*arg && *arg != ' ' && n < (int)sizeof(host) - 1)
		host[n++] = *arg++;
	host[n] = '\0';
	while (*arg == ' ')
		arg++;

	uint16_t qtype = 1;
	if (*arg) {
		qtype = dig_type_num(arg);
		if (qtype == 0) {
			con_write("dig: unknown type (A, AAAA, NS, CNAME, MX, TXT, PTR, SOA)\n");
			return -1;
		}
	}
	if (host[0] == '\0') {
		con_write("dig: usage: dig <host> [type]\n");
		return -1;
	}

	if (!dns_query(host, qtype)) {
		con_write(";; no response from ");
		print_ip(dns_ip);
		con_write("#53\n");
		return -1;
	}

	con_printf("; OwOS dig  %s  %s\n", host, dig_type_name(qtype));

	if (dns_ans_count == 0) {
		con_write(";; no records\n");
	} else {
		con_write(";; ANSWER SECTION:\n");
		for (int i = 0; i < dns_ans_count; i++) {
			char tx[DNS_TEXT];
			for (int k = 0; k < DNS_TEXT; k++) {
				tx[k] = dns_ans_text[i][k];
				if (!tx[k])
					break;
			}
			tx[DNS_TEXT - 1] = '\0';

			con_printf("%s.  %u  IN  %s  %s\n", host,
				   dns_ans_ttl[i], dig_type_name(dns_ans_type[i]), tx);
		}
	}

	con_write(";; Query time: ");
	print_rtt(dns_rtt_us);
	con_write(" ms   server ");
	print_ip(dns_ip);
	con_write("#53\n");
	return dns_ans_count > 0 ? 0 : -1;
}


static int append(char *dst, int n, int cap, const char *s)
{
	while (*s && n < cap - 1)
		dst[n++] = *s++;
	return n;
}

/* A byte reader buffered over the transport, for parsing the HTTP response.
 * curl_tls picks the TLS stream over the bare TCP one. */
static uint8_t curl_ibuf[2048];
static int     curl_ipos, curl_ilen, curl_iclosed;
static int     curl_tls;
static int     curl_th = -1;            /* TLS session handle when curl_tls    */
static tcp_conn_t *curl_conn;           /* the bare-TCP connection (non-TLS)   */

static int curl_getc(void)              /* next byte; -1 at EOF, -2 on reset   */
{
	if (curl_ipos >= curl_ilen) {
		if (curl_iclosed)
			return -1;
		int r = curl_tls ? tls_read(curl_th, curl_ibuf, sizeof(curl_ibuf), &curl_iclosed)
		                  : tcp_read(curl_conn, curl_ibuf, sizeof(curl_ibuf), &curl_iclosed);
		if (r < 0)
			return -2;
		if (r == 0)
			return -1;
		curl_ipos = 0;
		curl_ilen = r;
	}
	return curl_ibuf[curl_ipos++];
}

static char ci_low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ci_starts(const char *s, const char *pre)
{
	while (*pre) {
		if (ci_low(*s) != ci_low(*pre))
			return 0;
		s++; pre++;
	}
	return 1;
}

/* Case-insensitive substring search, bounded to the current header line. */
static int ci_in_line(const char *hay, const char *needle)
{
	for (; *hay && *hay != '\n'; hay++) {
		const char *h = hay, *n = needle;
		while (*n && *h && *h != '\n' && ci_low(*h) == ci_low(*n)) {
			h++; n++;
		}
		if (!*n)
			return 1;
	}
	return 0;
}

/* When the response is "Content-Encoding: gzip", body bytes are the compressed
 * stream: collect them here and inflate once the body is complete, rather than
 * emitting them straight away. Capped so a runaway response can't exhaust the
 * heap. */
#define CURL_GZ_MAX (1024 * 1024)
static uint8_t *curl_gzbuf;
static uint32_t curl_gzn;
static int      curl_gz;                /* 1 while collecting a gzip body       */
static int      curl_gzof;             /* set if the body overflowed the buffer */

/* When curl_mem is non-NULL the body is captured into it (up to curl_memcap)
 * instead of being printed/saved — this is how net_http_get() reuses all of
 * net_curl()'s response framing to fetch a URL into memory. */
static uint8_t *curl_mem;
static uint32_t curl_memcap, curl_memlen;

/* One body byte: while collecting a gzip body, buffer it; otherwise stream it
 * into the output file when saving, or echo it to the screen (which would be
 * garbage for a binary like an mp3). */
static void curl_emit(int save, int b)
{
	if (curl_gz) {
		if (curl_gzn < CURL_GZ_MAX)
			curl_gzbuf[curl_gzn++] = (uint8_t)b;
		else
			curl_gzof = 1;
		return;
	}
	if (curl_mem) {
		if (curl_memlen < curl_memcap)
			curl_mem[curl_memlen++] = (uint8_t)b;
		return;
	}
	if (save) {
		uint8_t bb = (uint8_t)b;
		fs_stream_write(&bb, 1);
	} else {
		con_putc((char)b);
	}
}

int net_curl(const char *arg)
{
	if (!nic_present()) {
		con_write("curl: no network device\n");
		return -1;
	}

	int show_headers = 0;
	char method[16] = "GET";
	int save = 0;
	char outname[64];
	while (*arg == ' ')
		arg++;
	while (arg[0] == '-') {
		if (arg[1] == 'i' && (arg[2] == ' ' || arg[2] == '\0')) {
			show_headers = 1;
			arg += 2;
		} else if (arg[1] == 'X' && (arg[2] == ' ' || arg[2] == '\0')) {
			arg += 2;
			while (*arg == ' ')
				arg++;
			int m = 0;
			while (*arg && *arg != ' ' && m < (int)sizeof(method) - 1)
				method[m++] = *arg++;
			method[m] = '\0';
			if (m == 0) {
				con_write("curl: -X needs a method\n");
				return -1;
			}
		} else if (arg[1] == 'o' && (arg[2] == ' ' || arg[2] == '\0')) {
			arg += 2;
			while (*arg == ' ')
				arg++;
			int m = 0;
			while (*arg && *arg != ' ' && m < (int)sizeof(outname) - 1)
				outname[m++] = *arg++;
			outname[m] = '\0';
			if (m == 0) {
				con_write("curl: -o needs a filename\n");
				return -1;
			}
			save = 1;
		} else {
			break;          /* not a known flag; treat as the URL */
		}
		while (*arg == ' ')
			arg++;
	}

	struct url u;
	if (!url_parse(arg, &u)) {
		con_write("curl: could not parse URL\n");
		return -1;
	}
	const char *host = u.host;
	uint16_t port = u.port ? u.port : 80;
	int use_tls = u.is_tls;
	curl_tls = use_tls;

	/* HTTP request-target: path (defaulting to "/") plus any ?query. */
	char path[URL_PATH_MAX + URL_QUERY_MAX + 2];
	int tn = 0;
	tn = append(path, tn, sizeof(path), u.path[0] ? u.path : "/");
	if (u.has_query) {
		tn = append(path, tn, sizeof(path), "?");
		tn = append(path, tn, sizeof(path), u.query);
	}
	path[tn] = '\0';

	uint8_t ip[4];
	if (!parse_ipv4(host, ip) && !dns_resolve(host, ip)) {
		con_printf("curl: could not resolve host: %s\n", host);
		return -1;
	}
	int ok;
	if (use_tls) {
		curl_th = tls_connect(ip, port, host, /*offer_h2=*/0);
		ok = curl_th >= 0;
	} else {
		curl_conn = tcp_connect(ip, port);
		ok = curl_conn != NULL;
	}
	if (!ok) {
		con_write("curl: failed to connect to ");
		print_ip(ip);
		con_putc('\n');
		return -1;
	}

	char req[512];
	int n = 0;
	n = append(req, n, sizeof(req), method);
	n = append(req, n, sizeof(req), " ");
	n = append(req, n, sizeof(req), path);
	n = append(req, n, sizeof(req), " HTTP/1.1\r\nHost: ");
	n = append(req, n, sizeof(req), host);
	n = append(req, n, sizeof(req),
		   "\r\nUser-Agent: owos\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n");
	if (use_tls)
		tls_write(curl_th, req, (uint32_t)n);
	else
		tcp_write(curl_conn, req, (uint32_t)n);

	/* Read the header block (through the blank line) into a buffer. */
	curl_ipos = curl_ilen = 0;
	curl_iclosed = 0;
	char hdr[4096];
	int hlen = 0, match = 0;
	for (;;) {
		int ci = curl_getc();
		if (ci < 0) {
			con_write("curl: no response from server\n");
			if (use_tls)
				tls_close(curl_th);
			else
				tcp_close(curl_conn);
			return -1;
		}
		char c = (char)ci;
		if (hlen < (int)sizeof(hdr) - 1)
			hdr[hlen++] = c;
		if (c == '\r')
			match = (match == 0) ? 1 : (match == 2 ? 3 : 1);
		else if (c == '\n')
			match = (match == 1) ? 2 : (match == 3 ? 4 : 0);
		else
			match = 0;
		if (match == 4)
			break;                        /* end of headers ("\r\n\r\n") */
	}
	hdr[hlen] = '\0';

	if (show_headers)
		con_write(hdr);

	/* HTTP/1.1 body framing: chunked takes precedence over Content-Length,
	 * which takes precedence over read-until-close. */
	int chunked = 0, have_len = 0, gz = 0;
	uint32_t content_len = 0;
	for (char *line = hdr; *line; ) {
		if (ci_starts(line, "transfer-encoding:")) {
			if (ci_in_line(line, "chunked"))
				chunked = 1;
		} else if (ci_starts(line, "content-encoding:")) {
			if (ci_in_line(line, "gzip"))
				gz = 1;
		} else if (ci_starts(line, "content-length:")) {
			const char *v = line + 15;            /* past "content-length:" */
			while (*v == ' ')
				v++;
			uint32_t x = 0;
			while (*v >= '0' && *v <= '9')
				x = x * 10 + (uint32_t)(*v++ - '0');
			content_len = x;
			have_len = 1;
		}
		char *eol = line;
		while (*eol && *eol != '\n')
			eol++;
		if (!*eol)
			break;
		line = eol + 1;
	}

	if (save && fs_stream_begin(outname) != 0) {
		con_printf("curl: cannot open output file %s\n", outname);
		if (use_tls)
			tls_close(curl_th);
		else
			tcp_close(curl_conn);
		return -1;
	}

	/* A gzip body is collected whole, then inflated at body_done. */
	curl_gz = 0;
	curl_gzof = 0;
	curl_gzn = 0;
	curl_gzbuf = NULL;
	if (gz) {
		curl_gzbuf = kmalloc(CURL_GZ_MAX);
		if (!curl_gzbuf) {
			con_write("curl: out of memory for gzip body\n");
			if (use_tls)
				tls_close(curl_th);
			else
				tcp_close(curl_conn);
			if (save)
				fs_stream_end();
			return -1;
		}
		curl_gz = 1;
	}

	if (chunked) {
		for (;;) {
			uint32_t sz = 0;
			int c, any = 0;
			while ((c = curl_getc()) >= 0 && c != '\r' && c != ';') {
				int d;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				sz = sz * 16 + (uint32_t)d;
				any = 1;
			}
			while (c >= 0 && c != '\n')            /* rest of the size line */
				c = curl_getc();
			if (!any || sz == 0)
				break;                        /* last chunk */
			for (uint32_t i = 0; i < sz; i++) {
				int b = curl_getc();
				if (b < 0)
					goto body_done;
				curl_emit(save, b);
			}
			curl_getc();                          /* trailing CR */
			curl_getc();                          /* trailing LF */
		}
	} else if (have_len) {
		for (uint32_t i = 0; i < content_len; i++) {
			int b = curl_getc();
			if (b < 0)
				break;
			curl_emit(save, b);
		}
	} else {
		int b;
		while ((b = curl_getc()) >= 0)
			curl_emit(save, b);
	}
body_done:
	if (use_tls)
		tls_close(curl_th);
	else
		tcp_close(curl_conn);

	/* Inflate the collected gzip body and emit it (to file or screen). */
	if (gz) {
		curl_gz = 0;                   /* curl_emit now really emits      */
		int failed = 0;
		if (curl_gzof) {
			con_write("curl: gzip body exceeds 1 MiB limit\n");
			failed = 1;
		} else {
			int orig = gzip_orig_size(curl_gzbuf, curl_gzn);
			uint8_t *plain = (orig >= 0)
				? kmalloc(orig ? (uint32_t)orig : 1) : NULL;
			int dn = (orig >= 0 && plain)
				? gzip_decompress(curl_gzbuf, curl_gzn, plain,
						  (uint32_t)orig)
				: -1;
			if (dn < 0) {
				con_write("curl: gzip decode failed\n");
				failed = 1;
			} else {
				for (int i = 0; i < dn; i++)
					curl_emit(save, plain[i]);
			}
			kfree(plain);
		}
		kfree(curl_gzbuf);
		curl_gzbuf = NULL;
		if (failed) {
			if (save)
				fs_stream_end();
			return -1;
		}
	}

	if (save) {
		int wrote = fs_stream_end();
		if (wrote < 0) {
			con_printf("curl: failed to save %s (disk full or write error)\n",
				   outname);
			return -1;
		}
		con_printf("curl: saved %u bytes to %s\n", (uint32_t)wrote, outname);
	} else if (!curl_mem) {
		con_putc('\n');
	}
	return 0;
}

/* Fetch `url` with a plain GET, capturing the response body into buf (cap
 * bytes). Returns the body length (truncated to cap), or -1 on error. Reuses
 * net_curl()'s full HTTP/1.1 framing — chunked, Content-Length, and gzip. */
int net_http_get(const char *url, uint8_t *buf, uint32_t cap)
{
	curl_mem = buf;
	curl_memcap = cap;
	curl_memlen = 0;
	int rc = net_curl(url);
	int n = (rc == 0) ? (int)curl_memlen : -1;
	curl_mem = NULL;
	return n;
}

/* ---- socket handles ------------------------------------------------
 * A thin, handle-based connection table so user programs (the curl in
 * user/curl.c) can drive the kernel transport without a built-in doing the
 * HTTP itself. Deliberately tiny: connect (DNS + TCP + optional TLS), send,
 * recv (blocking, EOF-terminated), close. TLS is multi-flow (tls.c keeps a pool
 * of sessions), so several handles can be TLS at once — e.g. a browser fetching
 * images over parallel HTTPS connections. */
#define NET_SOCK_MAX  8
#define SOCK_DGRAM    1500   /* one datagram's worth of rx per UDP/ICMP socket */
#define SOCK_DGRING   16     /* datagrams buffered per UDP/ICMP socket         */
static struct usock {
	uint8_t      used;
	uint8_t      proto;        /* SK_TCP / SK_TLS / SK_UDP / SK_ICMP          */
	uint8_t      closed;       /* peer FIN / close_notify already seen (TCP)  */
	int          tls;          /* TLS session handle (SK_TLS only; else -1)   */
	tcp_conn_t  *conn;         /* bare-TCP connection (TCP only)              */
	uint8_t      peer[4];      /* UDP/ICMP destination                        */
	uint16_t     lport, rport; /* UDP local / remote ports                    */
	/* Datagram receive ring (UDP/ICMP): the RX IRQ enqueues at dg_tail,
	 * net_sock_recv dequeues at dg_head (head==tail => empty). A ring, not a
	 * single slot, so a burst of datagrams — e.g. a QUIC handshake flight
	 * spanning several packets — isn't collapsed down to the latest one. */
	volatile uint32_t dg_head, dg_tail;
	uint32_t     dg_len[SOCK_DGRING];
	uint8_t      dg[SOCK_DGRING][SOCK_DGRAM];
} usocks[NET_SOCK_MAX];

/* Deliver an inbound UDP datagram to a matching client socket (by local port).
 * Called from the RX IRQ; returns 1 if a socket claimed it. */
static int usock_udp_deliver(uint16_t dport, const uint8_t *data, uint32_t len)
{
	for (int i = 0; i < NET_SOCK_MAX; i++) {
		struct usock *s = &usocks[i];
		if (s->used && s->proto == SK_UDP && s->lport == dport) {
			uint32_t next = (s->dg_tail + 1) % SOCK_DGRING;
			if (next == s->dg_head)
				return 1;        /* ring full: drop, but the port is ours */
			if (len > SOCK_DGRAM) len = SOCK_DGRAM;
			for (uint32_t k = 0; k < len; k++) s->dg[s->dg_tail][k] = data[k];
			s->dg_len[s->dg_tail] = len;
			s->dg_tail = next;
			return 1;
		}
	}
	return 0;
}

/* Deliver an inbound ICMP message to a raw ICMP socket (the program matches the
 * id/seq itself). Called from the RX IRQ. */
static void usock_icmp_deliver(const uint8_t *msg, uint32_t len)
{
	for (int i = 0; i < NET_SOCK_MAX; i++) {
		struct usock *s = &usocks[i];
		if (s->used && s->proto == SK_ICMP) {
			uint32_t next = (s->dg_tail + 1) % SOCK_DGRING;
			if (next == s->dg_head)
				return;          /* ring full: drop the datagram */
			if (len > SOCK_DGRAM) len = SOCK_DGRAM;
			for (uint32_t k = 0; k < len; k++) s->dg[s->dg_tail][k] = msg[k];
			s->dg_len[s->dg_tail] = len;
			s->dg_tail = next;
			return;
		}
	}
}

/* Block ~2 s for a datagram on a UDP/ICMP socket. >0 bytes / 0 timeout / -4 intr. */
static int sock_dgram_recv(struct usock *s, void *buf, uint32_t max)
{
	uint32_t deadline = timer_ticks() + 2 * TIMER_HZ;
	for (;;) {
		if (s->dg_head != s->dg_tail) {
			__asm__ volatile ("cli");
			uint32_t idx = s->dg_head;
			uint32_t n = s->dg_len[idx];
			if (n > max) n = max;
			for (uint32_t i = 0; i < n; i++)
				((uint8_t *)buf)[i] = s->dg[idx][i];
			s->dg_head = (idx + 1) % SOCK_DGRING;
			__asm__ volatile ("sti");
			return (int)n;
		}
		if (sched_intr_pending())
			return -4;
		if (timer_ticks() > deadline)
			return 0;
		__asm__ volatile ("sti\n\thlt");
	}
}

/* Open a socket: TCP/TLS connect, or a UDP/raw-ICMP datagram socket bound to
 * `peer`. Returns a handle 0..NET_SOCK_MAX-1, or -1 no slot/NIC, -2 resolve, -3
 * connect/handshake, -4 Ctrl+C. */
int net_sock_connect(const char *host, uint16_t port, int proto)
{
	if (!nic_present())
		return -1;
	/* SK_TLS_H2 is TLS that also advertises ALPN "h2"; store it as a normal TLS
	 * socket and remember to offer h2 in the handshake. */
	int offer_h2 = (proto == SK_TLS_H2);
	if (proto == SK_TLS_H2)
		proto = SK_TLS;
	int h = -1;
	for (int i = 0; i < NET_SOCK_MAX; i++)
		if (!usocks[i].used) { h = i; break; }
	if (h < 0)
		return -1;

	/* Reserve the slot NOW, before any blocking call (DNS/handshake re-enable
	 * interrupts and yield), so a concurrent connect on another thread — now
	 * possible with several TLS sessions in flight — can't claim the same slot.
	 * Every failure path below releases it. */
	struct usock *s = &usocks[h];
	s->used = 1;
	s->proto = (uint8_t)proto;
	s->closed = 0;
	s->conn = NULL;
	s->tls = -1;
	s->dg_head = s->dg_tail = 0;

	uint8_t ip[4];
	if (!parse_ipv4(host, ip) && !dns_resolve(host, ip)) {
		s->used = 0;
		return sched_intr_pending() ? -4 : -2;
	}
	for (int i = 0; i < 4; i++) s->peer[i] = ip[i];

	switch (proto) {
	case SK_TLS:
		s->tls = tls_connect(ip, port, host, offer_h2);
		if (s->tls < 0) {
			s->used = 0;
			return sched_intr_pending() ? -4 : -3;
		}
		break;
	case SK_TCP:
		s->conn = tcp_connect(ip, port);
		if (!s->conn) {
			s->used = 0;
			return sched_intr_pending() ? -4 : -3;
		}
		break;
	case SK_UDP:
		s->rport = port;
		s->lport = (uint16_t)(0xC000 + h);   /* unique per slot for rx demux  */
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
	case SK_TLS:  return tls_write(s->tls, buf, len);
	case SK_TCP:  return tcp_write(s->conn, buf, len);
	case SK_UDP:
		net_udp_send(s->peer, s->rport, s->lport, buf, len);
		return (int)len;
	case SK_ICMP:                            /* program built the ICMP message */
		ip_send(s->peer, IP_ICMP, buf, len);
		return (int)len;
	}
	return -1;
}

/* Block for data. TCP/TLS: >0 bytes, 0 EOF, -1 error, -4 intr. UDP/ICMP: >0
 * datagram bytes, 0 timeout, -4 intr. */
int net_sock_recv(int h, void *buf, uint32_t max)
{
	if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
		return -1;
	struct usock *s = &usocks[h];
	if (s->proto == SK_UDP || s->proto == SK_ICMP)
		return sock_dgram_recv(s, buf, max);
	if (s->closed)
		return 0;
	int closed = 0;
	int r = s->proto == SK_TLS ? tls_read(s->tls, buf, max, &closed)
	                           : tcp_read(s->conn, buf, max, &closed);
	if (r == TCP_EINTR)
		return -4;
	if (r < 0)
		return -1;
	if (closed)
		s->closed = 1;
	return r;
}

/* Non-blocking recv. TCP/TLS: >0 bytes, 0 would-block (no data yet, connection
 * still open), -1 error/EOF, -4 intr. UDP/ICMP: same as the blocking path
 * (it already returns 0 on timeout). Lets an interactive client (ssh) poll the
 * socket alongside stdin without the blocking path's 5 s idle auto-close
 * tearing down a session that's merely waiting for the user to type. */
int net_sock_recv_nb(int h, void *buf, uint32_t max)
{
	if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
		return -1;
	struct usock *s = &usocks[h];
	if (s->proto == SK_UDP || s->proto == SK_ICMP)
		return sock_dgram_recv(s, buf, max);
	if (s->closed)
		return -1;
	if (sched_intr_pending())
		return -4;
	if (s->proto == SK_TLS)
		return -1;                 /* TLS has no non-blocking read path here */
	int closed = 0;
	int r = tcp_read_nb(s->conn, buf, max, &closed);
	if (r < 0) { s->closed = 1; return -1; }
	if (closed) {
		s->closed = 1;
		return r > 0 ? r : -1;     /* hand back the last bytes, then EOF      */
	}
	return r;                          /* >0 data, or 0 = would-block          */
}

int net_sock_close(int h)
{
	if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
		return -1;
	struct usock *s = &usocks[h];
	if (s->proto == SK_TLS)
		tls_close(s->tls);
	else if (s->proto == SK_TCP && s->conn)
		tcp_close(s->conn);
	s->used = s->closed = 0;
	s->conn = NULL;
	s->tls = -1;
	return 0;
}

/* Negotiated ALPN application protocol on a TLS socket: 2 = h2, 1 = http/1.1
 * (or no ALPN), 0 = not a TLS socket. Lets the client pick HTTP/2 vs HTTP/1.1. */
int net_sock_alpn(int h)
{
	if (h < 0 || h >= NET_SOCK_MAX || !usocks[h].used)
		return -1;
	if (usocks[h].proto != SK_TLS)
		return 0;
	return tls_alpn_is_h2(usocks[h].tls) ? 2 : 1;
}

int net_ping(const char *host)
{
	if (!nic_present()) {
		con_write("ping: no network device\n");
		return -1;
	}

	uint8_t ip[4];
	if (!parse_ipv4(host, ip)) {
		if (!dns_resolve(host, ip)) {
			con_write("ping: cannot resolve ");
			con_write(host);
			con_putc('\n');
			return -1;
		}
	}

	con_write("PING ");
	con_write(host);
	con_write(" (");
	print_ip(ip);
	con_write("): 32 data bytes\n");

	uint16_t id = (uint16_t)(timer_ticks() ^ 0xBEEF);
	int recv = 0;

	/* Resolve the next hop up front so the per-packet send below isn't
	 * blocked waiting on ARP — otherwise the first RTT would absorb the
	 * ARP round-trip. */
	uint8_t warm[6];
	next_hop_mac(ip, warm);

	for (int seq = 1; seq <= 4; seq++) {
		icmp_got = 0;
		uint32_t deadline = timer_ticks() + TIMER_HZ;   /* ~1 s                */
		icmp_send_us = timer_us();          /* stamp immediately before TX     */
		icmp_send_echo(ip, id, (uint16_t)seq);

		int got = 0;
		while (timer_ticks() < deadline) {
			__asm__ volatile ("" ::: "memory");
			if (icmp_got && icmp_id_rx == id) {
				got = 1;
				break;
			}
			__asm__ volatile ("hlt");
		}

		if (got) {
			recv++;
			con_write("64 bytes from ");
			print_ip((const uint8_t *)icmp_from);
			con_write(": icmp_seq=");
			con_write_dec((uint32_t)icmp_seq_rx);
			con_write(" time=");
			print_rtt(icmp_rtt_us);
			con_write(" ms\n");
		} else {
			con_printf("request timeout for icmp_seq=%u\n", (uint32_t)seq);
		}

		if (seq < 4)
			timer_sleep(300);
	}

	con_printf("--- %s ping statistics ---\n%u packets transmitted, %u received\n",
		   host, 4, (uint32_t)recv);
	return recv > 0 ? 0 : -1;
}

/* Resolve a host (dotted-quad or DNS name) to an IPv4 address. 1 on success.
 * Exposed for the ping/dig programs via SYS_RESOLVE. */
int net_resolve(const char *host, uint8_t ip[4])
{
	if (parse_ipv4(host, ip))
		return 1;
	return dns_resolve(host, ip);
}

/* Run a DNS query and hand the parsed answers back to the dig program
 * (the wire parse already happened in dns_recv off the RX IRQ). Returns the
 * answer count (>=0), -2 if Ctrl+C interrupted the wait, -1 on no response/no
 * NIC. Fills *out (count, rtt, answers). */
int net_dig_query(const char *host, uint16_t qtype, struct dns_result *out)
{
	out->count = 0;
	out->rtt_us = 0;
	if (!nic_present())
		return -1;
	if (!dns_query(host, qtype))
		return sched_intr_pending() ? -2 : -1;

	int c = dns_ans_count;
	if (c > DNS_MAX_ANS)
		c = DNS_MAX_ANS;
	out->count = c;
	out->rtt_us = dns_rtt_us;
	for (int i = 0; i < c; i++) {
		out->ans[i].type = dns_ans_type[i];
		out->ans[i].ttl  = dns_ans_ttl[i];
		int k = 0;
		for (; k < DNS_TEXT - 1 && dns_ans_text[i][k]; k++)
			out->ans[i].text[k] = dns_ans_text[i][k];
		out->ans[i].text[k] = '\0';
	}
	return c;
}

/* Send one ICMP echo to `ip` with (id,seq) and wait up to ~1 s for the matching
 * reply. Returns the round-trip time in microseconds (>=0), or -1 timeout, -2
 * Ctrl+C, -3 no NIC. `from` (if non-NULL) gets the responder's IP. The
 * ping program drives the loop/printing/stats over SYS_PING_ONCE; the kernel
 * just does one exchange so it stays interruptible and out of ring 0. */
int net_ping_once(const uint8_t ip[4], uint16_t id, uint16_t seq, uint8_t from[4])
{
	if (!nic_present())
		return -3;
	uint8_t warm[6];
	next_hop_mac(ip, warm);             /* warm ARP so the RTT isn't skewed     */

	icmp_got = 0;
	uint32_t deadline = timer_ticks() + TIMER_HZ;       /* ~1 s                 */
	icmp_send_us = timer_us();
	icmp_send_echo(ip, id, seq);

	while (timer_ticks() < deadline) {
		__asm__ volatile ("" ::: "memory");
		if (icmp_got && icmp_id_rx == id) {
			if (from)
				for (int i = 0; i < 4; i++)
					from[i] = ((const uint8_t *)icmp_from)[i];
			return (int)icmp_rtt_us;
		}
		if (sched_intr_pending())       /* Ctrl+C from the ping                */
			return -2;
		__asm__ volatile ("sti\n\thlt");  /* IF=0 in syscall ctx               */
	}
	return -1;
}

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01). */
#define NTP_UNIX_DELTA 2208988800u

/* Convert a Unix timestamp (UTC) to a civil date/time, after Howard Hinnant's
 * days-from-civil inverse. Valid for any non-negative day count. */
static void unix_to_rtc(uint32_t secs, struct rtc_time *t)
{
	uint32_t days = secs / 86400u;
	uint32_t rem  = secs % 86400u;
	t->hour = (uint8_t)(rem / 3600u);
	t->min  = (uint8_t)((rem % 3600u) / 60u);
	t->sec  = (uint8_t)(rem % 60u);

	uint32_t z   = days + 719468u;          /* shift epoch to 0000-03-01       */
	uint32_t era = z / 146097u;
	uint32_t doe = z - era * 146097u;                       /* [0, 146096]     */
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t y   = yoe + era * 400u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);   /* [0, 365]    */
	uint32_t mp  = (5u * doy + 2u) / 153u;                       /* [0, 11]     */
	uint32_t d   = doy - (153u * mp + 2u) / 5u + 1u;            /* [1, 31]      */
	uint32_t m   = mp < 10u ? mp + 3u : mp - 9u;               /* [1, 12]      */

	t->day   = (uint8_t)d;
	t->month = (uint8_t)m;
	t->year  = (uint16_t)(y + (m <= 2u ? 1u : 0u));
}

/* Print a number as two zero-padded digits (for the HH:MM:SS clock readout). */
static void ntp_print2(uint8_t v)
{
	if (v < 10)
		con_putc('0');
	con_write_dec(v);
}

/* Query an SNTP server, then set the CMOS clock from its reply. `arg` may carry
 * a server host/IP and/or a timezone offset token like "+2" or "-5" (whole
 * hours); both are optional. Defaults: pool.ntp.org, UTC. */
int net_ntp(const char *arg)
{
	if (!nic_present()) {
		con_write("ntp: no network device\n");
		return -1;
	}

	const char *host = "pool.ntp.org";
	char        hostbuf[64];
	int         tz_secs = 0;

	/* Split arg into whitespace tokens: a leading +/- token is a TZ offset in
	 * hours, anything else is the server name/IP. */
	const char *s = arg;
	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		const char *start = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
		uint32_t tlen = (uint32_t)(s - start);

		if (start[0] == '+' || start[0] == '-') {
			int neg = start[0] == '-';
			int h = 0;
			for (uint32_t i = 1; i < tlen; i++)
				if (start[i] >= '0' && start[i] <= '9')
					h = h * 10 + (start[i] - '0');
			tz_secs = (neg ? -h : h) * 3600;
		} else if (tlen < sizeof(hostbuf)) {
			for (uint32_t i = 0; i < tlen; i++)
				hostbuf[i] = start[i];
			hostbuf[tlen] = '\0';
			host = hostbuf;
		}
	}

	uint8_t ip[4];
	if (!parse_ipv4(host, ip)) {
		if (!dns_resolve(host, ip)) {
			con_printf("ntp: cannot resolve %s\n", host);
			return -1;
		}
	}

	/* Warm the next-hop ARP entry so the first request isn't ARP-blocked. */
	uint8_t warm[6];
	next_hop_mac(ip, warm);

	uint8_t pkt[48];
	for (int i = 0; i < 48; i++)
		pkt[i] = 0;
	pkt[0] = 0x1B;                    /* LI=0, VN=3, Mode=3 (client)          */

	for (int tries = 0; tries < 3; tries++) {
		ntp_got = 0;
		net_udp_send(ip, 123, 0xC123, pkt, sizeof(pkt));
		uint32_t deadline = timer_ticks() + 2 * TIMER_HZ;   /* ~2 s            */
		while (timer_ticks() < deadline) {
			__asm__ volatile ("" ::: "memory");
			if (ntp_got)
				break;
			__asm__ volatile ("sti\n\thlt");   /* IF=0 in syscall ctx        */
		}
		if (ntp_got)
			break;
	}

	if (!ntp_got) {
		con_printf("ntp: no reply from %s\n", host);
		return -1;
	}

	if (ntp_secs < NTP_UNIX_DELTA) {     /* pre-1970: bogus / era-rollover     */
		con_write("ntp: implausible timestamp\n");
		return -1;
	}

	struct rtc_time t;
	unix_to_rtc(ntp_secs - NTP_UNIX_DELTA + (uint32_t)tz_secs, &t);
	rtc_set(&t);

	con_printf("clock set: %u-", t.year);
	ntp_print2(t.month);    con_putc('-');
	ntp_print2(t.day);      con_putc(' ');
	ntp_print2(t.hour);     con_putc(':');
	ntp_print2(t.min);      con_putc(':');
	ntp_print2(t.sec);
	con_write(tz_secs ? " (local)\n" : " UTC\n");
	return 0;
}

void net_init(void)
{
	nic_init();
	if (!nic_present())
		return;
	/* The NIC driver already logged its name/MAC/link. Don't announce an IP here:
	 * none is leased yet (the `dhcp` service runs after sti and applies the real
	 * lease, logged via klog under `verbose`). The live IP shows in the taskbar
	 * network indicator and `dhcp`/`ping` report it on demand. */
	con_write("net: requesting address via DHCP\n");
}
