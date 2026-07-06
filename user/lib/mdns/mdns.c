/* mdns.c — mDNS (RFC 6762) / DNS-SD (RFC 6763) client over a UDP socket.
 *
 * The kernel only lends a UDP datagram transport (to 224.0.0.251:5353 with the
 * TTL raised to 255 via net_set_ttl, since RFC 6762 responders drop mDNS
 * queries whose IP TTL != 255). Everything else — building the DNS query and
 * parsing the response with name compression — is here, self-contained. Since
 * the kernel delivers datagrams purely by local port (no owner filter), a
 * single response can carry PTR/SRV/TXT/A for several instances; we harvest
 * every record and correlate by owner name. */

#include "mdns.h"
#ifndef MDNS_HOST_TEST
#include "owos.h"
#endif

#define MDNS_MAX_PKT 1500

enum { T_A = 1, T_PTR = 12, T_TXT = 16, T_SRV = 33 };

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* In-stream length of a name at msg[pos]: labels until a 0 byte (+1) or a
 * compression pointer (+2, which ends the name). 0 on malformed. */
static int name_len(const unsigned char *msg, int pos, int end)
{
	int p = pos;
	while (p < end) {
		unsigned b = msg[p];
		if (b == 0) return p + 1 - pos;
		if ((b & 0xC0) == 0xC0) return p + 2 - pos;
		p += 1 + (int)b;
	}
	return 0;
}

/* Decode a (possibly compressed) name at msg[pos] into out (dotted). */
static void name_str(const unsigned char *msg, int end, int pos, char *out, int outsz)
{
	int oi = 0, jumps = 0, p = pos;
	while (p >= 0 && p < end) {
		unsigned b = msg[p];
		if (b == 0) break;
		if ((b & 0xC0) == 0xC0) {              /* compression pointer: jump      */
			if (p + 1 >= end || ++jumps > 16) break;
			p = ((int)(b & 0x3F) << 8) | msg[p + 1];
			continue;
		}
		p++;
		for (unsigned i = 0; i < b && p < end; i++, p++)
			if (oi < outsz - 1) out[oi++] = (char)msg[p];
		if (oi < outsz - 1) out[oi++] = '.';
	}
	if (oi && out[oi - 1] == '.') oi--;        /* drop trailing dot              */
	out[oi] = '\0';
}

/* First dotted label of `name` (the DNS-SD instance) into out. */
static void first_label(const char *name, char *out, int outsz)
{
	int i = 0;
	while (name[i] && name[i] != '.' && i < outsz - 1) { out[i] = name[i]; i++; }
	out[i] = '\0';
}

static int ci_eq(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return 0;
		a++; b++;
	}
	return *a == *b;
}

/* Encode `name` as DNS QNAME labels into q, return bytes written (0 on error). */
static int put_qname(unsigned char *q, const char *name)
{
	int n = 0;
	for (const char *s = name; *s; ) {
		int l = 0;
		while (s[l] && s[l] != '.') l++;
		if (l > 63) return 0;
		q[n++] = (unsigned char)l;
		for (int i = 0; i < l; i++) q[n++] = (unsigned char)s[i];
		s += l;
		if (*s == '.') s++;
	}
	q[n++] = 0;
	return n;
}

/* Build an mDNS query for `name`/`qtype` into q (>=64+len). Returns length. */
static int build_query(unsigned char *q, const char *name, int qtype)
{
	int n = 0;
	q[n++] = 0; q[n++] = 0;                /* id 0 per mDNS convention          */
	q[n++] = 0; q[n++] = 0;                /* flags: standard query             */
	q[n++] = 0; q[n++] = 1;                /* QDCOUNT=1                         */
	q[n++] = 0; q[n++] = 0;                /* ANCOUNT                           */
	q[n++] = 0; q[n++] = 0;                /* NSCOUNT                           */
	q[n++] = 0; q[n++] = 0;                /* ARCOUNT                           */
	int ln = put_qname(q + n, name);
	if (!ln) return 0;
	n += ln;
	q[n++] = (unsigned char)(qtype >> 8); q[n++] = (unsigned char)qtype;
	/* QCLASS=IN with the top QU "unicast-response requested" bit set, so the
	 * responder answers our address directly instead of multicast. */
	q[n++] = 0x80; q[n++] = 1;
	return n;
}

/* Find (or create) the out[] slot whose instance matches PTR target `name`.
 * Returns the index, or -1 if full. */
static int slot_for(struct mdns_service *out, int *count, int max, const char *name)
{
	char inst[128];
	first_label(name, inst, sizeof inst);
	for (int i = 0; i < *count; i++)
		if (ci_eq(out[i].instance, inst))
			return i;
	if (*count >= max) return -1;
	int i = (*count)++;
	memset(&out[i], 0, sizeof out[i]);
	strcpy(out[i].instance, inst);
	return i;
}

/* Copy raw TXT rdata (length-prefixed strings) NUL-joined into dst. */
static void copy_txt(const unsigned char *msg, int rd, int rdlen, char *dst, int dstsz)
{
	int i = 0, oi = 0;
	while (i < rdlen && oi < dstsz - 1) {
		int l = msg[rd + i++];
		for (int k = 0; k < l && i < rdlen && oi < dstsz - 1; k++, i++)
			dst[oi++] = (char)msg[rd + i];
		if (oi < dstsz - 1) dst[oi++] = '\0';   /* NUL-join entries              */
	}
	dst[oi] = '\0';
}

/* Correlate answer + additional records of one response datagram into out[],
 * keyed by owner name. `service` (may be 0) restricts which PTR targets we
 * accept. Returns the (possibly grown) instance count. Pure: no syscalls, so
 * host-testable. */
static int harvest_response(const unsigned char *r, int rn,
                            const char *service,
                            struct mdns_service *out, int count, int max)
{
	if (rn < 12) return count;
	unsigned qd = be16(r + 4);
	unsigned total = be16(r + 6) + be16(r + 8) + be16(r + 10); /* an+ns+ar */
	int pos = 12;
	for (unsigned i = 0; i < qd; i++) {        /* skip question(s)              */
		int l = name_len(r, pos, rn);
		if (!l) return count;
		pos += l + 4;
	}
	for (unsigned i = 0; i < total && pos + 10 <= rn; i++) {
		char owner[256];
		int l = name_len(r, pos, rn);
		if (!l) break;
		name_str(r, rn, pos, owner, sizeof owner);
		pos += l;
		if (pos + 10 > rn) break;   /* the 10-byte RR header sits *after* the owner name */
		int type = (int)be16(r + pos);
		int rdlen = (int)be16(r + pos + 8);
		int rd = pos + 10;
		pos = rd + rdlen;
		if (pos > rn) break;

		if (type == T_PTR) {
			char target[256];
			name_str(r, rn, rd, target, sizeof target);
			if (service && !ci_eq(owner, service)) continue;
			slot_for(out, &count, max, target);
		} else if (type == T_SRV && rdlen >= 7) {
			int si = slot_for(out, &count, max, owner);
			if (si < 0) continue;
			out[si].port = (uint16_t)be16(r + rd + 4);
			name_str(r, rn, rd + 6, out[si].host, sizeof out[si].host);
		} else if (type == T_TXT) {
			int si = slot_for(out, &count, max, owner);
			if (si < 0) continue;
			copy_txt(r, rd, rdlen, out[si].txt, sizeof out[si].txt);
		} else if (type == T_A && rdlen >= 4) {
			/* An A record's owner is the SRV target (a host), not an
			 * instance — fill any slot whose SRV host matches. */
			for (int s = 0; s < count; s++)
				if (out[s].host[0] && ci_eq(out[s].host, owner))
					memcpy(out[s].ip, r + rd, 4);
		}
	}
	return count;
}

#ifndef MDNS_HOST_TEST

static const char *MDNS_ADDR = "224.0.0.251";

/* Open a UDP socket to the mDNS group at TTL 255. Returns handle or <0. */
static int mdns_open(void)
{
	int h = net_connect(MDNS_ADDR, 5353, SK_UDP);
	if (h < 0) return h;
	net_set_ttl(h, 255);
	return h;
}

/* Drain responses into out[] until the deadline. Returns count, or NET_EINTR. */
static int drain(int h, const char *service, struct mdns_service *out,
                 int count, int max, unsigned deadline)
{
	unsigned char r[MDNS_MAX_PKT];
	while ((int)(deadline - msec()) > 0) {
		int rn = net_recv_nb(h, r, (int)sizeof r);
		if (rn > 0) {
			count = harvest_response(r, rn, service, out, count, max);
		} else if (rn == -1) {
			break;                             /* socket EOF                    */
		} else if (rn == NET_EINTR) {
			return NET_EINTR;
		} else {
			msleep(20);                        /* would-block: yield, don't spin */
		}
	}
	return count;
}

int mdns_resolve(const char *host, uint8_t ip[4])
{
	int h = mdns_open();
	if (h < 0) return h == -2 ? 0 : h;
	unsigned char q[320];
	int n = build_query(q, host, T_A);
	if (!n || net_send(h, q, n) < 0) { net_close(h); return -1; }

	struct mdns_service s;
	memset(&s, 0, sizeof s);
	int hl = (int)strlen(host);
	if (hl >= (int)sizeof s.host) { net_close(h); return 0; }  /* can't be a .local we'd match */
	memcpy(s.host, host, hl + 1);              /* seed so an A owner-matches     */
	int count = 1;
	unsigned deadline = msec() + 1500;
	unsigned char r[MDNS_MAX_PKT];
	int rc = 0;
	while ((int)(deadline - msec()) > 0) {
		int rn = net_recv_nb(h, r, (int)sizeof r);
		if (rn > 0) {
			harvest_response(r, rn, 0, &s, count, 1);
			if (s.ip[0] || s.ip[1] || s.ip[2] || s.ip[3]) { rc = 1; break; }
		} else if (rn == -1) {
			break;
		} else if (rn == NET_EINTR) {
			rc = NET_EINTR; break;
		} else {
			msleep(20);
		}
	}
	net_close(h);
	if (rc == 1) memcpy(ip, s.ip, 4);
	return rc;
}

int mdns_browse(const char *service, struct mdns_service *out,
                int max, int timeout_ms)
{
	if (max <= 0) return 0;
	int h = mdns_open();
	if (h < 0) return h == -2 ? 0 : h;
	unsigned char q[320];
	int n = build_query(q, service, T_PTR);
	if (!n || net_send(h, q, n) < 0) { net_close(h); return -1; }

	unsigned deadline = msec() + (unsigned)timeout_ms;
	int count = drain(h, service, out, 0, max, deadline);
	if (count == NET_EINTR) { net_close(h); return NET_EINTR; }

	/* Follow up only for instances still missing host/port/ip. */
	for (int i = 0; i < count; i++) {
		if ((int)(deadline - msec()) <= 0) break;
		if (!out[i].host[0] || !out[i].port) {
			char qname[256];
			int oi = 0;
			for (const char *s = out[i].instance; *s && oi < 250; s++) qname[oi++] = *s;
			qname[oi] = '.';
			strcpy(qname + oi + 1, service);
			n = build_query(q, qname, T_SRV);
			if (n && net_send(h, q, n) >= 0)
				count = drain(h, 0, out, count, max, msec() + 400);
			if (count == NET_EINTR) { net_close(h); return NET_EINTR; }
		}
	}
	/* Resolve any hosts still without an A record. */
	for (int i = 0; i < count; i++) {
		if ((int)(deadline - msec()) <= 0) break;
		if (out[i].host[0] && !(out[i].ip[0] || out[i].ip[1] || out[i].ip[2] || out[i].ip[3])) {
			n = build_query(q, out[i].host, T_A);
			if (n && net_send(h, q, n) >= 0)
				count = drain(h, 0, out, count, max, msec() + 400);
			if (count == NET_EINTR) { net_close(h); return NET_EINTR; }
		}
	}
	net_close(h);
	return count;
}

#endif /* !MDNS_HOST_TEST */
