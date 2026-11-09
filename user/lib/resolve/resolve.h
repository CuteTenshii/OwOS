/* resolve.h — the getaddrinfo analog: hostname -> IPv4/IPv6, in user space.
 *
 * The kernel no longer resolves; net_connect takes an IPv4 or IPv6 literal only (the Unix
 * model). This is the libc layer that turns a name into an address:
 *   - an IP literal (dotted quad or IPv6) is parsed directly,
 *   - a ".local" name is delegated to user/lib/mdns (mdns_resolve),
 *   - anything else is a unicast DNS query (A, or AAAA for resolve6) to the nameservers in
 *     /etc/resolv.conf, plus the router-advertised one (a sane default if none).
 * The DNS wire build/parse mirrors dig.c (name-compression aware). Links with
 * $(RESOLVE), which pulls in $(MDNS). Interruptible: NET_EINTR (-4) on Ctrl+C.
 */
#ifndef RESOLVE_H
#define RESOLVE_H

/* Resolve `host` to an IPv4 address. Returns 1 on success (ip filled), 0 on
 * failure/timeout, or NET_EINTR (-4) if a cooperative Ctrl+C arrived. */
int resolve(const char *host, unsigned char ip[4]);

/* IPv6 counterpart of resolve(): an IPv6 literal, or the first AAAA record of `host` via the DNS server in
 * resolv.conf / router-advertised servers. Returns 1 on success (ip filled), 0 on failure/timeout or for IPv4 literals
 * and ".local" names, or NET_EINTR. */
int resolve6(const char *host, unsigned char ip[16]);

/* Look up the HTTPS RR (RFC 9460) for host:port and copy its ECHConfigList (the `ech` SvcParam) into
 * out. Returns the list length, or 0 if there is none, it does not fit in `max`, or the query failed.
 * ServiceMode only; AliasMode targets are not followed. */
int resolve_ech(const char *host, int port, unsigned char *out, int max);

/* Convenience for the raw socket callers (ping's target, dig's @server):
 * resolve `host` then net_connect the resulting IP (v6 literals and AAAA records included; see net_dial in
 * resolve.c for the family order) with (port, proto). Returns
 * the socket handle (>=0), or a negative net_connect-style error (-2 resolve,
 * -4 intr). */
int net_dial(const char *host, int port, int proto);

/* net_dial with a family: *fam (if non-NULL) is 4 or 6 to restrict the lookup, 0 for either, and is set to
 * the family actually used (4 or 6). */
int net_dial_fam(const char *host, int port, int proto, int *fam);

#endif /* RESOLVE_H */
