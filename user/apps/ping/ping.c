/* ping.c - an ICMP echo client over a raw ICMP socket */

#include "opt.h"
#include "owos.h"
#include "resolve.h"
#include <stdbool.h>

/* Internet checksum (RFC 1071): ones-complement sum of 16-bit big-endian words */
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

/* Ctrl+C arrives as SIGINT (the terminal posts it to us) */
static volatile bool g_interrupted;
static void on_sigint(int s)
{
  (void)s;
  g_interrupted = true;
}

int main(int argc, char **argv)
{
  int count = 0; /* 0 = continuous */
  const char *host = 0;
  int fam = 0; /* 4 / 6 restrict the address family */

  signal(SIGINT, on_sigint);

  static const struct opt opts[] = {
      {'c', "count", OPT_ARG, 'c', "N", "stop after N replies (default: until Ctrl+C)"},
      {'4', 0, OPT_FLAG, '4', 0, "use IPv4 only"},
      {'6', 0, OPT_FLAG, '6', 0, "use IPv6 only"},
      {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
  };
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    unsigned n;
    if (o == 'h') {
      opt_print_help("ping", "HOST", "Send ICMP echo requests to HOST.", opts, OPT_N(opts));
      return 0;
    } else if (o == 'c' && opt_uint(st.arg, &n) == 0) {
      count = (int)n;
    } else if (o == '4' || o == '6') {
      fam = o - '0';
    } else if (o == OPT_POS) {
      host = st.arg;
    } else {
      host = 0;
      break;
    }
  }
  if (!host) {
    opt_print_usage("ping", "HOST", opts, OPT_N(opts));
    return 1;
  }

  int h = net_dial_fam(host, 0, SK_ICMP, &fam); /* resolve in user space, then connect */
  int v6 = fam == 6;
  if (h < 0) {
    if (h == -2) {
      printf("ping: cannot resolve %s\n", host);
    } else
      puts("ping: no network device\n");
    return 1;
  }

  printf("PING %s : 56 data bytes\n", host);

  int id = (int)(msec() & 0xFFFF);
  bool interrupted = false;
  int sent = 0, recv = 0;

  for (int seq = 1; count == 0 || seq <= count; seq++) {
    unsigned char pkt[8 + 56];
    pkt[0] = v6 ? 128 : 8; /* echo request; the kernel fills in the ICMPv6 checksum */
    pkt[1] = 0;
    pkt[2] = 0;
    pkt[3] = 0; /* type/code/csum */
    pkt[4] = (unsigned char)(id >> 8);
    pkt[5] = (unsigned char)id;
    pkt[6] = (unsigned char)(seq >> 8);
    pkt[7] = (unsigned char)seq;
    for (int i = 0; i < 56; i++)
      pkt[8 + i] = (unsigned char)('a' + (i % 26));
    if (!v6) {
      unsigned short c = icmp_csum(pkt, (int)sizeof pkt);
      pkt[2] = (unsigned char)(c >> 8);
      pkt[3] = (unsigned char)c;
    }

    unsigned t0 = msec();
    if (net_send(h, pkt, (int)sizeof pkt) < 0) {
      puts("ping: send failed\n");
      break;
    }
    sent++;

    bool got = false;
    for (;;) { /* wait for our reply */
      unsigned char rb[1500];
      int n = net_recv(h, rb, (int)sizeof rb);
      if (n == NET_EINTR) {
        interrupted = true;
        break;
      }
      if (n <= 0)
        break; /* timeout */
      if (n >= 8 && rb[0] == (v6 ? 129 : 0) && (((int)rb[4] << 8) | rb[5]) == id) {
        got = true;
        break;
      }
      /* not our reply; keep waiting */
    }
    if (interrupted)
      break;

    if (got) {
      recv++;
      unsigned rtt = msec() - t0;
      printf("64 bytes from %s: icmp_seq=%u time=%u ms\n", host, (unsigned)seq, rtt);
    } else {
      printf("request timeout for icmp_seq=%u\n", (unsigned)seq);
    }
    if (g_interrupted) {
      interrupted = true;
      break;
    }
    if (count == 0 || seq < count)
      msleep(800);
  }

  net_close(h);
  printf("--- %s ping statistics ---\n", host);
  printf("%u packets transmitted, %u received\n", (unsigned)sent, (unsigned)recv);
  return interrupted ? EXIT_SIGINT : (recv > 0 ? 0 : 1);
}
