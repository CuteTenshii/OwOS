/* dhcpcd.c — the DHCP client, in user space. Autostarted as root at boot.
 *
 * A minimal DISCOVER -> OFFER -> REQUEST -> ACK exchange over a raw Ethernet
 * socket (net_raw_open, the AF_PACKET analog): the kernel has no IP yet, so we
 * build the full L2 frame ourselves (broadcast dst, our MAC as src, IPv4/UDP
 * 68->67, BOOTP+DHCP) rather than going through the IP stack. On ACK we apply
 * the lease with ifconfig() and write /etc/resolv.conf. One-shot at startup. */

#include "owos.h"

/* --- byte helpers --------------------------------------------------------- */
static void put16(unsigned char *p, unsigned v)
{
  p[0] = v >> 8;
  p[1] = v & 0xff;
}
static unsigned get16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static int eq4(const unsigned char *a, const unsigned char *b)
{
  for (int i = 0; i < 4; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

/* Ones-complement Internet checksum over `len` bytes. */
static unsigned short inet_csum(const unsigned char *p, int len)
{
  unsigned sum = 0;
  while (len > 1) {
    sum += (p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len)
    sum += p[0] << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (unsigned short)(~sum & 0xffff);
}

/* Parse "aa:bb:cc:dd:ee:ff" (from /sys/class/net/eth0/address) into mac[6]. */
static int parse_mac(const char *s, unsigned char mac[6])
{
  for (int i = 0; i < 6; i++) {
    int hi = -1, lo = -1;
    for (int k = 0; k < 2; k++) {
      char c = *s++;
      int d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      else
        return 0;
      if (k == 0)
        hi = d;
      else
        lo = d;
    }
    mac[i] = (unsigned char)((hi << 4) | lo);
    if (i < 5 && *s++ != ':')
      return 0;
  }
  return 1;
}

/* --- DHCP ----------------------------------------------------------------- */
#define DHCP_LEN 300 /* BOOTP fixed area + options we emit */

static unsigned char our_mac[6];
static unsigned char xid[4];

/* Build a BOOTP/DHCP message (type 1=DISCOVER, 3=REQUEST) into out[DHCP_LEN].
 * `srv`/`req_ip` carry the REQUEST's chosen server/address. Returns its length.
 * Ported from the kernel's dhcp_build. */
static int dhcp_build(unsigned char msgtype, const unsigned char *srv, const unsigned char *req_ip, unsigned char *out)
{
  memset(out, 0, DHCP_LEN);
  out[0] = 1;
  out[1] = 1;
  out[2] = 6;                   /* op=request, ethernet, hlen 6 */
  memcpy(out + 4, xid, 4);      /* transaction id */
  out[10] = 0x80;               /* flags: broadcast the reply */
  memcpy(out + 28, our_mac, 6); /* chaddr */
  out[236] = 0x63;
  out[237] = 0x82; /* magic cookie 63 82 53 63 */
  out[238] = 0x53;
  out[239] = 0x63;
  int o = 240;
  out[o++] = 53;
  out[o++] = 1;
  out[o++] = msgtype; /* message type */
  if (msgtype == 3) { /* REQUEST carries the choices */
    out[o++] = 50;
    out[o++] = 4;
    memcpy(out + o, req_ip, 4);
    o += 4;
    out[o++] = 54;
    out[o++] = 4;
    memcpy(out + o, srv, 4);
    o += 4;
  }
  out[o++] = 55;
  out[o++] = 3; /* parameter request list */
  out[o++] = 1;
  out[o++] = 3;
  out[o++] = 6;   /* mask, router, DNS */
  out[o++] = 255; /* end */
  return o;
}

/* Wrap a DHCP payload in Ethernet/IPv4/UDP (src 0.0.0.0:68 ->
 * 255.255.255.255:67) into frame[], returns the frame length. */
static int build_frame(const unsigned char *dhcp, int dlen, unsigned char *frame)
{
  static const unsigned char bmac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  int ulen = 8 + dlen;   /* UDP header + payload */
  int iplen = 20 + ulen; /* IP header + UDP */

  /* Ethernet header. */
  memcpy(frame, bmac, 6);
  memcpy(frame + 6, our_mac, 6);
  put16(frame + 12, 0x0800); /* EtherType IPv4 */

  unsigned char *ip = frame + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;          /* ver 4, IHL 5 */
  put16(ip + 2, iplen);  /* total length */
  put16(ip + 6, 0x4000); /* flags: DF */
  ip[8] = 64;            /* TTL */
  ip[9] = 17;            /* proto UDP */
  /* src 0.0.0.0 (already zero), dst 255.255.255.255 */
  for (int i = 0; i < 4; i++)
    ip[16 + i] = 0xff;
  put16(ip + 10, inet_csum(ip, 20));

  unsigned char *udp = ip + 20;
  put16(udp + 0, 68);   /* sport */
  put16(udp + 2, 67);   /* dport */
  put16(udp + 4, ulen); /* length */
  put16(udp + 6, 0);    /* checksum optional for IPv4 UDP */
  memcpy(udp + 8, dhcp, dlen);

  return 14 + iplen;
}

/* Parse a received frame; on a matching DHCP OFFER/ACK, fill the lease fields.
 * Returns the DHCP message type (2=OFFER, 5=ACK) or 0 if it isn't for us. */
static unsigned char parse_reply(const unsigned char *f, int len, unsigned char yiaddr[4], unsigned char mask[4],
                                 unsigned char gw[4], unsigned char dns[4], unsigned char srv[4])
{
  if (len < 14 + 20 + 8 + 240)
    return 0;
  if (get16(f + 12) != 0x0800)
    return 0; /* not IPv4 */
  const unsigned char *ip = f + 14;
  if ((ip[0] >> 4) != 4)
    return 0;
  int ihl = (ip[0] & 0x0f) * 4;
  if (ihl < 20)
    return 0;
  if (ip[9] != 17)
    return 0; /* not UDP */
  const unsigned char *udp = ip + ihl;
  if (get16(udp + 2) != 68)
    return 0;                       /* dport != our BOOTP client */
  const unsigned char *p = udp + 8; /* DHCP message */
  int dlen = len - (int)(p - f);
  if (dlen < 240)
    return 0;
  if (!eq4(p + 4, xid))
    return 0; /* not our transaction */

  unsigned char mtype = 0;
  memcpy(yiaddr, p + 16, 4); /* yiaddr */
  for (int i = 240; i + 1 < dlen;) {
    unsigned char opt = p[i++];
    if (opt == 0)
      continue; /* pad */
    if (opt == 255)
      break; /* end */
    unsigned char olen = p[i++];
    if (i + olen > dlen)
      break;
    if (opt == 53 && olen >= 1)
      mtype = p[i];
    else if (opt == 1 && olen >= 4)
      memcpy(mask, p + i, 4);
    else if (opt == 3 && olen >= 4)
      memcpy(gw, p + i, 4);
    else if (opt == 6 && olen >= 4)
      memcpy(dns, p + i, 4);
    else if (opt == 54 && olen >= 4)
      memcpy(srv, p + i, 4);
    i += olen;
  }
  return mtype;
}

/* Send `frame`, then poll the raw socket for a reply of the wanted DHCP type
 * (2 OFFER / 5 ACK), retrying the send a few times. Returns 1 on match. */
static int exchange(int fd, const unsigned char *frame, int flen, unsigned char want, unsigned char yiaddr[4],
                    unsigned char mask[4], unsigned char gw[4], unsigned char dns[4], unsigned char srv[4])
{
  unsigned char rx[1514];
  for (int t = 0; t < 8; t++) {
    if (net_send(fd, frame, flen) < 0)
      return 0;
    /* ~1.5 s of non-blocking polling per try. */
    for (int w = 0; w < 30; w++) {
      int n = net_recv_nb(fd, rx, sizeof rx);
      if (n == NET_EINTR)
        return 0;
      if (n > 0) {
        unsigned char yi[4], mk[4], g[4], dn[4], sv[4];
        memcpy(yi, yiaddr, 4);
        memcpy(mk, mask, 4);
        memcpy(g, gw, 4);
        memcpy(dn, dns, 4);
        memcpy(sv, srv, 4);
        unsigned char mt = parse_reply(rx, n, yi, mk, g, dn, sv);
        if (mt == want) {
          memcpy(yiaddr, yi, 4);
          memcpy(mask, mk, 4);
          memcpy(gw, g, 4);
          memcpy(dns, dn, 4);
          memcpy(srv, sv, 4);
          return 1;
        }
        continue; /* ARP / unrelated traffic: ignore, keep polling */
      }
      msleep(50);
    }
  }
  return 0;
}

static void log_ip(const char *label, const unsigned char ip[4])
{
  printf("%s%u.%u.%u.%u", label, ip[0], ip[1], ip[2], ip[3]);
}

int main(void)
{
  char macbuf[64];
  int n = readfile("/sys/class/net/eth0/address", macbuf, sizeof macbuf - 1);
  if (n <= 0 || !parse_mac(macbuf, our_mac)) {
    printf("dhcpcd: no NIC MAC\n");
    return 1;
  }

  int fd = net_raw_open();
  if (fd < 0) {
    printf("dhcpcd: raw socket failed (need root + a NIC)\n");
    return 1;
  }

  /* A transaction id: not cryptographic, just unique per boot. */
  unsigned seed = msec() ^ (our_mac[5] << 8) ^ our_mac[4];
  xid[0] = 'O';
  xid[1] = 'w';
  xid[2] = (unsigned char)seed;
  xid[3] = (unsigned char)(seed >> 8);

  unsigned char yiaddr[4] = {0}, mask[4] = {255, 255, 255, 0};
  unsigned char gw[4] = {0}, dns[4] = {0}, srv[4] = {0};
  unsigned char dhcp[DHCP_LEN], frame[1514];

  /* DISCOVER -> OFFER. */
  int flen = build_frame(dhcp, dhcp_build(1, srv, yiaddr, dhcp), frame);
  if (!exchange(fd, frame, flen, 2, yiaddr, mask, gw, dns, srv)) {
    printf("dhcpcd: no OFFER\n");
    net_close(fd);
    return 1;
  }

  /* REQUEST -> ACK. */
  flen = build_frame(dhcp, dhcp_build(3, srv, yiaddr, dhcp), frame);
  if (!exchange(fd, frame, flen, 5, yiaddr, mask, gw, dns, srv)) {
    printf("dhcpcd: no ACK\n");
    net_close(fd);
    return 1;
  }
  net_close(fd);

  if (ifconfig(yiaddr, mask, gw, dns) < 0) {
    printf("dhcpcd: ifconfig failed\n");
    return 1;
  }

  /* Publish the resolver. */
  char resolv[64];
  int rl = snprintf(resolv, sizeof resolv, "nameserver %u.%u.%u.%u\n", dns[0], dns[1], dns[2], dns[3]);
  writefile("/etc/resolv.conf", resolv, rl);

  log_ip("dhcpcd: ip ", yiaddr);
  log_ip(" gw ", gw);
  log_ip(" dns ", dns);
  printf("\n");
  return 0;
}
