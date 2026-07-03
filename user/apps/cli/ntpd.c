/* ntpd.c — the SNTP clock-sync client, in user space. Run on demand (`ntpd`).
 *
 * The kernel only lends a UDP datagram transport and the root-only setrtc()
 * primitive; this program builds the 48-byte SNTP client request, parses the
 * reply's transmit timestamp (offset 40, big-endian NTP seconds since 1900),
 * converts to Unix seconds and sets the wall clock. Root-only in effect
 * (setrtc rejects a non-root caller). Interruptible (net_recv -> NET_EINTR).
 *
 *   ntpd                 sync via the default server
 *   ntpd pool.ntp.org    sync via a named/IP server
 */

#include "owos.h"
#include "resolve.h"

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01). */
#define NTP_UNIX_DELTA 2208988800u

int main(int argc, char **argv)
{
	/* Cloudflare's anycast NTP address is a sane no-config default; a named
	 * server (arg) resolves in user space via resolve(). */
	const char *server = argc > 1 ? argv[1] : "162.159.200.1";

	int h = net_dial(server, 123, SK_UDP);
	if (h < 0) {
		if (h == -2) printf("ntpd: cannot resolve %s\n", server);
		else         puts("ntpd: no network device\n");
		return 1;
	}

	unsigned char pkt[48];
	memset(pkt, 0, sizeof pkt);
	pkt[0] = 0x1B;                   /* LI=0, VN=3, Mode=3 (client)             */

	if (net_send(h, pkt, sizeof pkt) < 0) {
		puts("ntpd: send failed\n");
		net_close(h);
		return 1;
	}

	unsigned char r[48];
	int rn = net_recv(h, r, (int)sizeof r);
	net_close(h);
	if (rn == NET_EINTR) return EXIT_SIGINT;
	if (rn < 48) { printf("ntpd: no reply from %s\n", server); return 1; }

	/* Transmit timestamp: integer seconds since 1900 (offset 40, big-endian);
	 * only the seconds matter for a wall clock. */
	unsigned ntp_secs = ((unsigned)r[40] << 24) | ((unsigned)r[41] << 16) |
	                    ((unsigned)r[42] << 8)  |  (unsigned)r[43];
	if (ntp_secs < NTP_UNIX_DELTA) {     /* pre-1970: bogus / era rollover      */
		puts("ntpd: implausible timestamp\n");
		return 1;
	}

	unsigned unix_secs = ntp_secs - NTP_UNIX_DELTA;
	if (setrtc(unix_secs) < 0) {
		puts("ntpd: setrtc failed (need root)\n");
		return 1;
	}

	printf("ntpd: clock set from %s (%u UTC)\n", server, unix_secs);
	return 0;
}
