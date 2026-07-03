/* mdns.h — mDNS (RFC 6762) + DNS-SD (RFC 6763) client over a UDP socket.
 *
 * A general link-local resolver: ".local" name -> IPv4 (mdns_resolve), and
 * service discovery (mdns_browse) that returns the instances of a DNS-SD
 * service type ("_googlecast._tcp.local", "_ipp._tcp.local", ...) with their
 * SRV host/port, resolved A record, and raw TXT. No per-consumer logic lives
 * here — the caller interprets the service type and TXT. Queries go to
 * 224.0.0.251:5353 at IP TTL 255 (RFC 6762) with the QU unicast-response bit
 * set, so a device's reply reaches our socket directly. Links with $(MDNS).
 *
 * Interruptible: a negative return means an error, and NET_EINTR (-4)
 * specifically means a cooperative Ctrl+C arrived (tear down and exit). */
#ifndef MDNS_H
#define MDNS_H

#include <stdint.h>

struct mdns_service {
	char     instance[128]; /* DNS-SD instance label (first, ".", of the PTR) */
	char     host[128];     /* SRV target, usually <id>.local                 */
	uint8_t  ip[4];         /* resolved A, 0.0.0.0 if unknown                 */
	uint16_t port;          /* SRV port                                       */
	char     txt[512];      /* raw TXT rdata, entries NUL-joined              */
};

/* Resolve one ".local" name's A record. 1 = ip filled, 0 = miss/timeout,
 * <0 = error (NET_EINTR on Ctrl+C). */
int mdns_resolve(const char *host, uint8_t ip[4]);

/* Discover instances of a DNS-SD service type (e.g. "_ipp._tcp.local").
 * Fills up to `max` entries, correlating SRV/TXT/A found in the responses;
 * returns the count (>=0), or <0 on error (NET_EINTR on Ctrl+C). */
int mdns_browse(const char *service, struct mdns_service *out,
                int max, int timeout_ms);

#endif /* MDNS_H */
