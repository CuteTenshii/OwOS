/* net.h — minimal IPv4 stack over an RTL8139 NIC (enough to ping a host). */
#ifndef OWOS_NET_H
#define OWOS_NET_H

#include <stdint.h>

void net_init(void);            /* bring up the NIC and announce the IP config */
int  net_ping(const char *host);/* resolve + ICMP-echo a host; returns 0 if any
                                 * reply arrived, -1 otherwise                 */
int  net_dig(const char *host); /* DNS A-record lookup; prints the answers     */
int  net_curl(const char *arg); /* HTTP/1.0 GET of an http:// URL (-i: headers)*/

/* GET `url`, capturing the response body into buf (cap bytes); returns the
 * body length (capped at cap), or -1 on error. Reuses net_curl()'s framing. */
int  net_http_get(const char *url, uint8_t *buf, uint32_t cap);

/* Ring-3 socket handles (driven from kernel/syscall.c). The HTTP/protocol
 * logic lives in user space (user/curl.c); these only expose the transport.
 * connect: DNS-resolve + TCP + optional TLS; returns a handle or a negative
 * error. send: bytes or -1. recv: >0 bytes, 0 at EOF, -1 on error. */
int  net_sock_connect(const char *host, uint16_t port, int tls);
int  net_sock_send(int h, const void *buf, uint32_t len);
int  net_sock_recv(int h, void *buf, uint32_t max);
int  net_sock_close(int h);

/* Receive callback invoked by the NIC driver for each inbound Ethernet frame
 * (FCS already stripped). Runs in interrupt context. */
void net_rx_frame(const uint8_t *frame, uint32_t len);

/* Lower-layer hooks used by the transport layer (tcp.c). */
void           net_ip_send(const uint8_t dst[4], uint8_t proto,
                           const uint8_t *payload, uint32_t plen);
const uint8_t *net_my_ip(void);

/* UDP server support, used by headless network services (kernel/services.c).
 * A handler is invoked from interrupt context for every datagram arriving on
 * its bound port, with the sender's IP/port and the UDP payload; it may reply
 * with net_udp_send (the NIC TX path is safe to call from the RX IRQ). */
typedef void (*udp_handler_t)(const uint8_t src[4], uint16_t sport,
                              const uint8_t *data, uint32_t len);
int  net_udp_listen(uint16_t port, udp_handler_t fn);  /* 0 ok, -1 table full */
void net_udp_unlisten(uint16_t port);
void net_udp_send(const uint8_t dst[4], uint16_t dport, uint16_t sport,
                  const uint8_t *data, uint32_t len);

#endif /* OWOS_NET_H */
