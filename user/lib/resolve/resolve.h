/* resolve.h — the getaddrinfo analog: hostname -> IPv4, in user space.
 *
 * The kernel no longer resolves; net_connect takes an IP literal only (the Unix
 * model). This is the libc layer that turns a name into an address:
 *   - a dotted-quad IP literal is parsed directly,
 *   - a ".local" name is delegated to user/lib/mdns (mdns_resolve),
 *   - anything else is a unicast DNS A-record query to the nameserver in
 *     /etc/resolv.conf (a sane default if that's missing/unreadable).
 * The DNS wire build/parse mirrors dig.c (name-compression aware). Links with
 * $(RESOLVE), which pulls in $(MDNS). Interruptible: NET_EINTR (-4) on Ctrl+C. */
#ifndef RESOLVE_H
#define RESOLVE_H

/* Resolve `host` to an IPv4 address. Returns 1 on success (ip filled), 0 on
 * failure/timeout, or NET_EINTR (-4) if a cooperative Ctrl+C arrived. */
int resolve(const char *host, unsigned char ip[4]);

/* Convenience for the raw socket callers (ping's target, dig's @server): resolve
 * `host` then net_connect the resulting IP with (port, proto). Returns the socket
 * handle (>=0), or a negative net_connect-style error (-2 resolve, -4 intr). */
int net_dial(const char *host, int port, int proto);

#endif /* RESOLVE_H */
