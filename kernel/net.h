/* net.h — minimal IPv4 stack over an RTL8139 NIC (enough to ping a host). */
#ifndef OWOS_NET_H
#define OWOS_NET_H

#include <stdint.h>

/* Socket protocol selector for net_sock_connect / the SYS_NET_CONNECT `proto`
 * arg (kept wire-compatible with the old tls flag: 0=TCP, 1=TLS). Shared in
 * value with user/lib/owos.h. */
#define SK_TCP   0
#define SK_TLS   1
#define SK_UDP   2
#define SK_ICMP  3   /* raw ICMP: the program builds/parses the ICMP message */
#define SK_TLS_H2 4  /* TLS that also offers ALPN "h2" (else identical to SK_TLS) */

/* DNS answer section, parsed (the IRQ-tied wire parse lives in net.c). The
 * structs are shared in shape with user/lib/owos.h so the dig program
 * receives them straight from SYS_DNS_QUERY. */
#define DNS_MAX_ANS 16
#define DNS_TEXT    128
struct dns_answer { unsigned type; unsigned ttl; char text[DNS_TEXT]; };
struct dns_result { int count; unsigned rtt_us; struct dns_answer ans[DNS_MAX_ANS]; };

void net_init(void);            /* bring up the NIC and announce the IP config */
void net_dhcp_start(void);      /* spawn a DHCP client to lease IP/gw/dns (real links) */
int  net_dhcp_now(void);        /* re-run DHCP synchronously (the `dhcp` command)       */
int  net_ping(const char *host);/* resolve + ICMP-echo a host; returns 0 if any
                                 * reply arrived, -1 otherwise                 */
int  net_resolve(const char *host, uint8_t ip[4]);  /* host -> IPv4; 1 = ok    */
/* One ICMP echo (id,seq), wait ~1s. rtt_us >=0 / -1 timeout / -2 intr / -3 no NIC.
 * Drives the ping program via SYS_PING_ONCE. */
int  net_ping_once(const uint8_t ip[4], uint16_t id, uint16_t seq, uint8_t from[4]);
/* DNS query for the dig program. count >=0 / -1 fail / -2 intr. */
int  net_dig_query(const char *host, uint16_t qtype, struct dns_result *out);
int  net_dig(const char *host); /* DNS A-record lookup; prints the answers     */
int  net_ntp(const char *arg);  /* SNTP-sync the CMOS clock; arg: [host] [+/-tz] */
int  net_curl(const char *arg); /* HTTP/1.0 GET of an http:// URL (-i: headers)*/

/* GET `url`, capturing the response body into buf (cap bytes); returns the
 * body length (capped at cap), or -1 on error. Reuses net_curl()'s framing. */
int  net_http_get(const char *url, uint8_t *buf, uint32_t cap);

/* Socket handles (driven from kernel/syscall.c). The HTTP/protocol
 * logic lives in user space (user/curl.c); these only expose the transport.
 * connect: DNS-resolve + TCP + optional TLS; returns a handle or a negative
 * error. send: bytes or -1. recv: >0 bytes, 0 at EOF, -1 on error. */
int  net_sock_connect(const char *host, uint16_t port, int tls);
int  net_sock_send(int h, const void *buf, uint32_t len);
int  net_sock_recv(int h, void *buf, uint32_t max);
/* Non-blocking recv: >0 bytes, 0 would-block, -1 error/EOF, -4 intr. Used by
 * interactive clients (ssh) that multiplex the socket with stdin. */
int  net_sock_recv_nb(int h, void *buf, uint32_t max);
int  net_sock_close(int h);
/* Negotiated ALPN protocol on a TLS socket: 2=h2, 1=http/1.1/none, 0=not TLS. */
int  net_sock_alpn(int h);

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
