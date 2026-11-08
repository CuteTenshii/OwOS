/* resolve.c — hostname -> IPv4 in user space. See resolve.h. */

#include "resolve.h"
#include "mdns.h"
#include "owos.h"
#include "svcb.h"

/* Fallback nameserver when /etc/resolv.conf is absent/unreadable (SLIRP's DNS).
 */
static const unsigned char DNS_DEFAULT[4] = {10, 0, 2, 3};

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Parse a dotted-quad IP literal into out. 1 on success, 0 otherwise. */
static int parse_ipv4(const char *s, unsigned char out[4])
{
  for (int i = 0; i < 4; i++) {
    if (!is_digit(*s))
      return 0;
    int v = 0, digits = 0;
    while (is_digit(*s)) {
      v = v * 10 + (*s++ - '0');
      if (++digits > 3 || v > 255)
        return 0;
    }
    out[i] = (unsigned char)v;
    if (i < 3) {
      if (*s != '.')
        return 0;
      s++;
    }
  }
  return *s == '\0';
}

/* True if `host` ends in ".local" (case-insensitive) — the mDNS namespace. */
static int is_local(const char *host)
{
  int n = 0;
  while (host[n])
    n++;
  if (n < 6)
    return 0;
  const char *s = host + n - 6;
  return s[0] == '.' && (s[1] | 32) == 'l' && (s[2] | 32) == 'o' && (s[3] | 32) == 'c' && (s[4] | 32) == 'a' &&
         (s[5] | 32) == 'l';
}

/* Read the first "nameserver A.B.C.D" line of /etc/resolv.conf into ns. Falls
 * back to DNS_DEFAULT when the file is missing/unreadable or has no directive.
 */
static void read_nameserver(unsigned char ns[4])
{
  for (int i = 0; i < 4; i++)
    ns[i] = DNS_DEFAULT[i];

  char buf[512];
  int n = readfile("/etc/resolv.conf", buf, (int)sizeof buf - 1);
  if (n <= 0)
    return;
  buf[n] = '\0';

  for (const char *p = buf; *p;) {
    const char *line = p;
    while (*p && *p != '\n')
      p++;
    int len = (int)(p - line);
    if (*p)
      p++;
    /* "nameserver" (10) + at least one space + an address. */
    if (len < 12 || line[0] != 'n' || line[1] != 'a' || line[2] != 'm' || line[3] != 'e' || line[4] != 's' ||
        line[5] != 'e' || line[6] != 'r' || line[7] != 'v' || line[8] != 'e' || line[9] != 'r' || line[10] != ' ')
      continue;
    const char *a = line + 11;
    while (*a == ' ' || *a == '\t')
      a++;
    char ips[24];
    int k = 0;
    while (k < (int)sizeof ips - 1 && (is_digit(a[k]) || a[k] == '.'))
      ips[k] = a[k], k++;
    ips[k] = '\0';
    unsigned char got[4];
    if (parse_ipv4(ips, got)) {
      for (int i = 0; i < 4; i++)
        ns[i] = got[i];
      return;
    }
  }
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* Send a `qtype` query for `host` to `ns`:53. Returns the reply length in r (>= 12), 0 on
 * miss/timeout, or NET_EINTR on Ctrl+C. */
static int dns_exchange(const char *host, int qtype, const unsigned char ns[4], unsigned char *r, int rmax)
{
  char server[16];
  int si = 0;
  for (int i = 0; i < 4; i++) {
    unsigned v = ns[i], d = 100;
    int started = 0;
    while (d) {
      unsigned q = v / d;
      if (q || started || d == 1) {
        server[si++] = (char)('0' + q);
        started = 1;
      }
      v %= d;
      d /= 10;
    }
    if (i < 3)
      server[si++] = '.';
  }
  server[si] = '\0';

  /* Build the query. */
  unsigned char q[512];
  int n = 0;
  int id = (int)(msec() & 0xFFFF);
  q[n++] = id >> 8;
  q[n++] = id;
  q[n++] = 0x01;
  q[n++] = 0x00; /* RD                                  */
  q[n++] = 0;
  q[n++] = 1; /* QDCOUNT=1                           */
  q[n++] = 0;
  q[n++] = 0;
  q[n++] = 0;
  q[n++] = 0;
  q[n++] = 0;
  q[n++] = 0;
  for (const char *s = host; *s;) { /* QNAME labels                        */
    int l = 0;
    while (s[l] && s[l] != '.')
      l++;
    if (l > 63)
      return 0;
    q[n++] = (unsigned char)l;
    for (int i = 0; i < l; i++)
      q[n++] = (unsigned char)s[i];
    s += l;
    if (*s == '.')
      s++;
  }
  q[n++] = 0;
  q[n++] = (unsigned char)(qtype >> 8);
  q[n++] = (unsigned char)qtype;
  q[n++] = 0;
  q[n++] = 1; /* QCLASS=IN                           */

  for (int tries = 0; tries < 2; tries++) {
    int h = net_connect(server, 53, SK_UDP);
    if (h < 0)
      return h == NET_EINTR ? NET_EINTR : 0;
    if (net_send(h, q, n) < 0) {
      net_close(h);
      continue;
    }

    int rn = net_recv(h, r, rmax); /* ~2 s socket timeout, then 0 */
    net_close(h);
    if (rn == NET_EINTR)
      return NET_EINTR;
    if (rn >= 12)
      return rn;
  }
  return 0;
}

/* First A address of `host` via `ns`. 1 = resolved, 0 = miss/timeout, NET_EINTR on Ctrl+C. */
static int dns_lookup(const char *host, const unsigned char ns[4], unsigned char ip[4])
{
  unsigned char r[1500];
  int rn = dns_exchange(host, 1, ns, r, (int)sizeof r);
  if (rn <= 0)
    return rn;

  unsigned qd = be16(r + 4), an = be16(r + 6);
  int pos = 12;
  for (unsigned i = 0; i < qd; i++) { /* skip question(s)           */
    int l = dns_name_len(r, pos, rn);
    if (!l) {
      an = 0;
      break;
    }
    pos += l + 4;
  }
  for (unsigned i = 0; i < an && pos + 10 <= rn; i++) {
    int l = dns_name_len(r, pos, rn);
    if (!l)
      break;
    pos += l;
    if (pos + 10 > rn)
      break; /* RR header follows the name */
    int type = (int)be16(r + pos);
    int rdlen = (int)be16(r + pos + 8);
    pos += 10;
    if (pos + rdlen > rn)
      break;
    if (type == 1 && rdlen == 4) { /* first A record             */
      for (int k = 0; k < 4; k++)
        ip[k] = r[pos + k];
      return 1;
    }
    pos += rdlen; /* CNAME etc.: keep scanning  */
  }
  return 0; /* answered, but no A          */
}

int resolve(const char *host, unsigned char ip[4])
{
  if (!host || !*host)
    return 0;
  if (parse_ipv4(host, ip))
    return 1;
  if (is_local(host)) {
    int r = mdns_resolve(host, ip); /* 1 ok, 0 miss, <0 err/intr  */
    return r == NET_EINTR ? NET_EINTR : (r > 0 ? 1 : 0);
  }
  unsigned char ns[4];
  read_nameserver(ns);
  return dns_lookup(host, ns, ip);
}

int net_dial(const char *host, int port, int proto)
{
  unsigned char ip[4];
  int r = resolve(host, ip);
  if (r == NET_EINTR)
    return NET_EINTR;
  if (r != 1)
    return -2;
  char dotted[16];
  int di = 0;
  for (int i = 0; i < 4; i++) {
    unsigned v = ip[i], d = 100;
    int started = 0;
    while (d) {
      unsigned q = v / d;
      if (q || started || d == 1) {
        dotted[di++] = (char)('0' + q);
        started = 1;
      }
      v %= d;
      d /= 10;
    }
    if (i < 3)
      dotted[di++] = '.';
  }
  dotted[di] = '\0';
  return net_connect(dotted, port, proto);
}

int resolve_ech(const char *host, int port, unsigned char *out, int max)
{
  char name[260];
  int n = 0;
  if (!host || !*host || is_local(host))
    return 0;
  if (port != 443) { /* RFC 9460 §9.1: _<port>._https.<host> */
    name[n++] = '_';
    char digits[6];
    int dn = 0;
    for (unsigned v = (unsigned)port; v; v /= 10)
      digits[dn++] = (char)('0' + v % 10);
    while (dn)
      name[n++] = digits[--dn];
    memcpy(name + n, "._https.", 8);
    n += 8;
  }
  int hl = 0;
  while (host[hl])
    hl++;
  if (n + hl >= (int)sizeof name)
    return 0;
  memcpy(name + n, host, hl + 1);

  unsigned char ns[4], r[1500];
  read_nameserver(ns);
  int rn = dns_exchange(name, DNS_TYPE_HTTPS, ns, r, (int)sizeof r);
  return rn > 0 ? svcb_find_ech(r, rn, out, max) : 0;
}
