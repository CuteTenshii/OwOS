/* dig.c — a DNS lookup tool over a UDP socket.
 *
 * The kernel only lends a UDP datagram transport; this program builds the DNS
 * query and parses the response (header, question, answer RRs with name
 * compression) itself. The resolver defaults to 8.8.8.8; override with @server.
 * Interruptible (net_recv -> NET_EINTR).
 *
 *   dig example.com            A record via 8.8.8.8
 *   dig example.com MX
 *   dig @10.0.2.3 example.com  use a specific resolver
 */

#include "owos.h"
#include "resolve.h"

static int eq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
		a++; b++;
	}
	return *a == *b;
}

static const struct { const char *name; int num; } types[] = {
	{ "A", 1 }, { "NS", 2 }, { "CNAME", 5 }, { "SOA", 6 },
	{ "PTR", 12 }, { "MX", 15 }, { "TXT", 16 }, { "AAAA", 28 }, { "SRV", 33 },
};
static int type_num(const char *s)
{
	for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
		if (eq_ci(s, types[i].name)) return types[i].num;
	return 0;
}
static const char *type_name(int t)
{
	for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
		if (types[i].num == t) return types[i].name;
	return "?";
}

/* In-stream length of a name at msg[pos]: labels until a 0 byte (+1) or a
 * compression pointer (+2). 0 on malformed. */
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
		if ((b & 0xC0) == 0xC0) {
			if (p + 1 >= end || ++jumps > 16) break;
			p = ((int)(b & 0x3F) << 8) | msg[p + 1];
			continue;
		}
		p++;
		for (unsigned i = 0; i < b && p < end; i++, p++)
			if (oi < outsz - 1) out[oi++] = (char)msg[p];
		if (oi < outsz - 1) out[oi++] = '.';
	}
	if (oi && out[oi - 1] == '.') oi--;     /* drop trailing dot               */
	out[oi] = '\0';
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }

static void put_rdata(const unsigned char *msg, int end, int type, int rd, int rdlen)
{
	char nm[256];
	switch (type) {
	case 1:                                          /* A                        */
		if (rdlen >= 4) {
			for (int i = 0; i < 4; i++) printf(i < 3 ? "%u." : "%u", msg[rd+i]);
		}
		break;
	case 28:                                         /* AAAA                     */
		for (int i = 0; i < 16 && i + 1 < rdlen + 1; i += 2) {
			printf("%x", be16(msg + rd + i));
			if (i < 14) puts(":");
		}
		break;
	case 2: case 5: case 12:                          /* NS / CNAME / PTR        */
		name_str(msg, end, rd, nm, sizeof nm); puts(nm);
		break;
	case 15:                                          /* MX: pref + name         */
		printf("%u ", be16(msg + rd));
		name_str(msg, end, rd + 2, nm, sizeof nm); puts(nm);
		break;
	case 16:                                          /* TXT: length-prefixed    */
		{
			int i = 0;
			while (i < rdlen) {
				int l = msg[rd + i++];
				for (int k = 0; k < l && i < rdlen; k++, i++)
					printf("%c", (char)msg[rd + i]);
			}
		}
		break;
	case 33:                                          /* SRV: prio wt port target */
		if (rdlen >= 7) {
			printf("%u %u %u ", be16(msg + rd), be16(msg + rd + 2), be16(msg + rd + 4));
			name_str(msg, end, rd + 6, nm, sizeof nm); puts(nm);
		}
		break;
	case 6:                                           /* SOA: mname rname + 5 u32 */
		name_str(msg, end, rd, nm, sizeof nm); puts(nm); puts(" ");
		{
			int l = name_len(msg, rd, end);
			name_str(msg, end, rd + l, nm, sizeof nm); puts(nm);
		}
		break;
	default:
		puts("(rdata)");
		break;
	}
}

int main(int argc, char **argv)
{
	const char *server = "8.8.8.8", *host = 0, *typestr = 0;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '@')      server = argv[i] + 1;
		else if (argv[i][0] == '-') continue;
		else if (!host)             host = argv[i];
		else if (!typestr)          typestr = argv[i];
	}
	if (!host) {
		puts("usage: dig [@server] <host> [type]   (A AAAA NS CNAME MX TXT PTR SOA SRV)\n");
		return 1;
	}
	int qtype = 1;
	if (typestr && !(qtype = type_num(typestr))) {
		puts("dig: unknown type\n");
		return 1;
	}

	/* Build the query. */
	unsigned char q[512];
	int n = 0;
	int id = (int)(msec() & 0xFFFF);
	q[n++] = id >> 8; q[n++] = id;
	q[n++] = 0x01; q[n++] = 0x00;        /* RD                                  */
	q[n++] = 0; q[n++] = 1;              /* QDCOUNT=1                           */
	q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
	for (const char *s = host; *s; ) {  /* QNAME labels                        */
		int l = 0; while (s[l] && s[l] != '.') l++;
		if (l > 63) { puts("dig: label too long\n"); return 1; }
		q[n++] = (unsigned char)l;
		for (int i = 0; i < l; i++) q[n++] = (unsigned char)s[i];
		s += l; if (*s == '.') s++;
	}
	q[n++] = 0;
	q[n++] = qtype >> 8; q[n++] = qtype; /* QTYPE                               */
	q[n++] = 0; q[n++] = 1;              /* QCLASS=IN                           */

	int h = net_dial(server, 53, SK_UDP);   /* @server may be a name; resolve it */
	if (h < 0) {
		if (h == -2) { printf("dig: cannot resolve resolver %s\n", server); }
		else         puts("dig: no network device\n");
		return 1;
	}

	unsigned t0 = msec();
	if (net_send(h, q, n) < 0) { puts("dig: send failed\n"); net_close(h); return 1; }

	unsigned char r[1500];
	int rn = net_recv(h, r, (int)sizeof r);
	net_close(h);
	if (rn == NET_EINTR) return EXIT_SIGINT;
	if (rn < 12) { printf(";; no response from %s\n", server); return 1; }

	unsigned qd = be16(r + 4), an = be16(r + 6);
	int pos = 12;
	for (unsigned i = 0; i < qd; i++) {     /* skip question(s)                */
		int l = name_len(r, pos, rn); if (!l) { an = 0; break; }
		pos += l + 4;
	}

	printf("; OwOS dig  %s  %s\n", host, type_name(qtype));
	if (an == 0) {
		puts(";; no records\n");
	} else {
		puts(";; ANSWER SECTION:\n");
		for (unsigned i = 0; i < an && pos + 10 <= rn; i++) {
			int l = name_len(r, pos, rn); if (!l) break;
			pos += l;
			int type = (int)be16(r + pos);
			unsigned ttl = be32(r + pos + 4);
			int rdlen = (int)be16(r + pos + 8);
			pos += 10;
			if (pos + rdlen > rn) break;
			char nm[256];
			name_str(r, rn, 12, nm, sizeof nm);   /* the queried name           */
			printf("%s.  %u  IN  %s  ", host, ttl, type_name(type));
			put_rdata(r, rn, type, pos, rdlen);
			puts("\n");
			pos += rdlen;
		}
	}
	printf(";; Query time: %u ms   server %s#53\n", msec() - t0, server);
	return an > 0 ? 0 : 1;
}
