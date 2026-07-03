/* ping.c — an ICMP echo client over a raw ICMP socket.
 *
 * The kernel only lends a raw ICMP transport (net_connect(ip, 0, SK_ICMP) +
 * net_send/net_recv): this program resolves the host in user space (net_dial),
 * builds the echo request (type 8, id, seq, checksum), sends it, and matches the
 * reply's id itself. Ctrl+C is cooperative (net_recv -> NET_EINTR).
 *
 *   ping 10.0.2.2        continuous until Ctrl+C
 *   ping -c 4 host       stop after 4
 */

#include "owos.h"
#include "resolve.h"

/* Internet checksum (RFC 1071): ones-complement sum of 16-bit big-endian words. */
static unsigned short icmp_csum(const unsigned char *d, int n)
{
	unsigned long s = 0;
	for (int i = 0; i + 1 < n; i += 2)
		s += ((unsigned)d[i] << 8) | d[i + 1];
	if (n & 1)
		s += (unsigned)d[n - 1] << 8;
	while (s >> 16)
		s = (s & 0xFFFF) + (s >> 16);
	return (unsigned short)(~s & 0xFFFF);
}

/* Ctrl+C arrives as SIGINT (the terminal posts it to us). Catch it so we print
 * the summary and exit cleanly instead of being terminated by the default action;
 * a blocking net_recv also returns NET_EINTR when the signal is pending. */
static volatile int g_interrupted;
static void on_sigint(int s) { (void)s; g_interrupted = 1; }

int main(int argc, char **argv)
{
	int count = 0;                 /* 0 = continuous                          */
	const char *host = 0;

	signal(SIGINT, on_sigint);

	for (int i = 1; i < argc; i++) {
		if (streq(argv[i], "-c") && i + 1 < argc) {
			count = 0;
			for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
				count = count * 10 + (*p - '0');
		} else if (argv[i][0] != '-') {
			host = argv[i];
		}
	}
	if (!host) {
		puts("usage: ping [-c count] <host>\n");
		return 1;
	}

	int h = net_dial(host, 0, SK_ICMP);   /* resolve in user space, then connect */
	if (h < 0) {
		if (h == -2) { printf("ping: cannot resolve %s\n", host); }
		else         puts("ping: no network device\n");
		return 1;
	}

	printf("PING %s : 56 data bytes\n", host);

	int id = (int)(msec() & 0xFFFF);
	int sent = 0, recv = 0, interrupted = 0;

	for (int seq = 1; count == 0 || seq <= count; seq++) {
		unsigned char pkt[8 + 56];
		pkt[0] = 8; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0;      /* type/code/csum  */
		pkt[4] = (unsigned char)(id >> 8); pkt[5] = (unsigned char)id;
		pkt[6] = (unsigned char)(seq >> 8); pkt[7] = (unsigned char)seq;
		for (int i = 0; i < 56; i++)
			pkt[8 + i] = (unsigned char)('a' + (i % 26));
		unsigned short c = icmp_csum(pkt, (int)sizeof pkt);
		pkt[2] = (unsigned char)(c >> 8); pkt[3] = (unsigned char)c;

		unsigned t0 = msec();
		if (net_send(h, pkt, (int)sizeof pkt) < 0) { puts("ping: send failed\n"); break; }
		sent++;

		int got = 0;
		for (;;) {                                   /* wait for our reply      */
			unsigned char rb[1500];
			int n = net_recv(h, rb, (int)sizeof rb);
			if (n == NET_EINTR) { interrupted = 1; break; }
			if (n <= 0) break;                       /* timeout                 */
			if (n >= 8 && rb[0] == 0 &&
			    (((int)rb[4] << 8) | rb[5]) == id) { got = 1; break; }
			/* not our reply; keep waiting */
		}
		if (interrupted) break;

		if (got) {
			recv++;
			unsigned rtt = msec() - t0;
			printf("64 bytes from %s: icmp_seq=%u time=%u ms\n", host, (unsigned)seq, rtt);
		} else {
			printf("request timeout for icmp_seq=%u\n", (unsigned)seq);
		}
		if (g_interrupted) { interrupted = 1; break; }
		if (count == 0 || seq < count)
			msleep(800);
	}

	net_close(h);
	printf("--- %s ping statistics ---\n", host);
	printf("%u packets transmitted, %u received\n", (unsigned)sent, (unsigned)recv);
	return interrupted ? EXIT_SIGINT : (recv > 0 ? 0 : 1);
}
