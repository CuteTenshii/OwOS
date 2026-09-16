/* net.h — minimal IPv4 stack over an RTL8139 NIC (enough to ping a host). */
#ifndef OWOS_NET_H
#define OWOS_NET_H

#include <stdint.h>

/* Socket protocol selector for net_sock_connect / the SYS_NET_CONNECT `proto`
 * arg. Shared in value with user/lib/owos.h. TLS is user-space only now, so the
 * kernel has no TLS proto; a connect with a TLS value fails cleanly. */
#define SK_TCP 0
#define SK_UDP 2
#define SK_ICMP 3   /* raw ICMP: the program builds/parses the ICMP message */
#define SK_RAWETH 4 /* raw L2: send/recv full Ethernet frames (root-only, AF_PACKET) */

void net_init(void);       /* bring up the NIC and announce the IP config */
void net_dhcp_spawn(void); /* launch /bin/dhcpcd detached (root) to lease a link */

/* Socket handles (driven from kernel/syscall.c). The HTTP/protocol
 * logic lives in user space (user/curl.c); these only expose the transport.
 * connect: TCP-connect to an IP literal (name resolution is user space now);
 * returns a handle or a negative error. send: bytes or -1. recv: >0 bytes, 0 at
 * EOF, -1 on error. */
int net_sock_connect(const char *host, uint16_t port, int proto);
int net_sock_send(int h, const void *buf, uint32_t len);
int net_sock_recv(int h, void *buf, uint32_t max);
/* Non-blocking recv: >0 bytes, 0 would-block, -1 error/EOF, -4 intr. Used by
 * interactive clients (ssh) that multiplex the socket with stdin. */
int net_sock_recv_nb(int h, void *buf, uint32_t max);
int net_sock_close(int h);
/* Set the outgoing IP TTL for a UDP socket (IP_MULTICAST_TTL analog). 0/-1. */
int net_sock_set_ttl(int h, int ttl);

/* Raw Ethernet socket (the AF_PACKET analog), reached via net_sock_connect with
 * SK_RAWETH. A raw slot in the same handle table: net_sock_send transmits the
 * caller's full L2 frame verbatim, net_sock_recv/_nb dequeues one received
 * frame. Returns a handle or -1 (no slot/NIC). Root-gating lives in the syscall
 * layer. */
int net_raw_open(void);
/* Set the kernel IPv4 config (SYS_IFCONFIG). Any pointer may be NULL to skip.
 */
void net_ifconfig(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]);

/* Server sockets (same handle table). TCP: net_sock_listen opens a passive
 * listener on `port`; net_sock_accept blocks and returns a NEW handle for the
 * established connection (usable with net_sock_send/recv/close). UDP:
 * net_sock_bind binds a datagram socket to `port`; net_sock_recvfrom dequeues
 * one datagram with its sender IP/port (out pointers may be NULL);
 * net_sock_sendto replies to an arbitrary ip:port. Return >=0 handle/ok or a
 * negative error (-1 no NIC/slot/bad fd, -4 Ctrl+C). */
int net_sock_listen(int port);
int net_sock_accept(int h);
int net_sock_bind(int port);
int net_sock_recvfrom(int h, void *buf, uint32_t max, uint8_t out_ip[4], uint16_t *out_port);
int net_sock_sendto(int h, const uint8_t ip[4], int port, const void *buf, uint32_t len);

/* Receive callback invoked by the NIC driver for each inbound Ethernet frame
 * (FCS already stripped). Runs in interrupt context. */
void net_rx_frame(const uint8_t *frame, uint32_t len);

/* Lower-layer hooks used by the transport layer (tcp.c). */
void net_ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint32_t plen);
const uint8_t *net_my_ip(void);
/* 1 once a real IPv4 address is held (DHCP lease / static config), else 0.
 * Distinguishes "link up" from "has an address" -- see net.c. */
int net_have_ip(void);

/* UDP server support, used by headless network daemons (now user-space).
 * A handler is invoked from interrupt context for every datagram arriving on
 * its bound port, with the sender's IP/port and the UDP payload; it may reply
 * with net_udp_send (the NIC TX path is safe to call from the RX IRQ). */
typedef void (*udp_handler_t)(const uint8_t src[4], uint16_t sport, const uint8_t *data, uint32_t len);
int net_udp_listen(uint16_t port, udp_handler_t fn); /* 0 ok, -1 table full */
void net_udp_unlisten(uint16_t port);
void net_udp_send(const uint8_t dst[4], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len);
void net_udp_send_ttl(const uint8_t dst[4], uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len,
                      uint8_t ttl);

#endif /* OWOS_NET_H */
