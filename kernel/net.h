/* net.h: minimal dual-stack (IPv4 + IPv6) network layer. IP addresses are 16 bytes; IPv4 is v4-mapped (::ffff:a.b.c.d).
 */
#ifndef OWOS_NET_H
#define OWOS_NET_H

#include <stdbool.h>
#include <stdint.h>

/* Socket protocol selector for net_sock_connect (shared with user/lib/owos.h). No TLS in kernel. */
#define SK_TCP 0
#define SK_UDP 2
#define SK_ICMP 3   /* raw ICMP: the program builds/parses the ICMP message */
#define SK_RAWETH 4 /* raw L2: send/recv full Ethernet frames (root-only, AF_PACKET) */

void net_init(void);       /* bring up the NIC and announce the IP config */
void net_start(void);      /* start the deferred receive worker */
void net_dhcp_spawn(void); /* launch /bin/dhcpcd detached (root) to lease a link */

/* Socket handles (see kernel/syscall.c). connect takes an IPv4 or IPv6 literal; recv: >0 bytes, 0 EOF, -1 error.
 * TCP send blocks while the send buffer is full: bytes queued, -1 error, -4 interrupted before any were queued. */
int net_sock_connect(const char *host, uint16_t port, int proto);
int net_sock_send(int h, const void *buf, uint32_t len);
int net_sock_recv(int h, void *buf, uint32_t max);
/* Non-blocking recv: >0 bytes, 0 would-block, -1 error/EOF, -4 intr. */
int net_sock_recv_nb(int h, void *buf, uint32_t max);
int net_sock_close(int h);
void net_sock_close_owner(uint32_t owner);
/* Set the outgoing IP TTL for a UDP socket (IP_MULTICAST_TTL analog). 0/-1. */
int net_sock_set_ttl(int h, int ttl);

/* Raw Ethernet socket via net_sock_connect with SK_RAWETH; send/recv whole L2 frames. */
int net_raw_open(void);
/* Set the kernel IPv4 config (SYS_IFCONFIG); NULL pointers are skipped. */
void net_ifconfig(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]);

/* Server sockets: listen/accept (TCP), bind/recvfrom/sendto (UDP). Negative error: -1 bad, -4 Ctrl+C. */
int net_sock_listen(int port);
int net_sock_accept(int h);
int net_sock_bind(int port);
int net_sock_recvfrom(int h, void *buf, uint32_t max, uint8_t out_ip[16], uint16_t *out_port);
int net_sock_sendto(int h, const uint8_t ip[16], int port, const void *buf, uint32_t len);

/* Called by the NIC driver per inbound frame (FCS stripped), in interrupt context. */
void net_rx_frame(const uint8_t *frame, uint32_t len);

/* Lower-layer hooks used by the transport layer (tcp.c). */
void net_ip_send(const uint8_t dst[16], uint8_t proto, const uint8_t *payload, uint32_t plen);
/* Source address we would use toward dst (v4-mapped, global v6 or link-local v6). */
void net_src_addr(const uint8_t dst[16], uint8_t out[16]);
/* TCP/UDP/ICMPv6 checksum over the pseudo-header of src/dst (same family); 0 on receive means valid. */
uint16_t net_l4_csum(const uint8_t src[16], const uint8_t dst[16], uint8_t proto, const uint8_t *seg, uint32_t len);
/* Largest TCP payload per segment toward dst. */
uint32_t net_tcp_mss(const uint8_t dst[16]);
const uint8_t *net_my_ip(void);
/* 1 once a real IPv4 address is held, else 0. */
bool net_have_ip(void);
/* Global IPv6 address from a router advertisement (NULL until one arrives), and the link-local address. */
const uint8_t *net_my_ip6(void);
const uint8_t *net_ll_ip6(void);
/* First DNS server from a router advertisement (RDNSS); NULL if none. */
const uint8_t *net_dns6(void);
/* Text form of a 16-byte address into out (>= 40 bytes); returns the length. */
int net_addr_str(const uint8_t a[16], char *out);

/* UDP server hook: handler runs in interrupt context per datagram on its port; may reply via net_udp_send. */
typedef void (*udp_handler_t)(const uint8_t src[16], uint16_t sport, const uint8_t *data, uint32_t len);
int net_udp_listen(uint16_t port, udp_handler_t fn); /* 0 ok, -1 table full */
void net_udp_unlisten(uint16_t port);
void net_udp_send(const uint8_t dst[16], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len);
void net_udp_send_ttl(const uint8_t dst[16], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len,
                      uint8_t ttl);

#endif /* OWOS_NET_H */
