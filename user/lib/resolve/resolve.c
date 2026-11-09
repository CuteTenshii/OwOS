/* resolve.c — hostname -> IPv4/IPv6 in user space. See resolve.h. */

#include "resolve.h"
#include "mdns.h"
#include "owos.h"
#include "svcb.h"

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

/* Parse an IPv6 literal (optional "%zone" suffix, ignored) into out. 1 on success, 0 otherwise. */
static int parse_ipv6(const char *s, unsigned char out[16])
{
  unsigned short head[8], tail[8];
  int nh = 0, nt = 0, gap = 0;
  if (s[0] == ':') {
    if (s[1] != ':')
      return 0;
    gap = 1;
    s += 2;
  }
  while (*s && *s != '%') {
    unsigned v = 0;
    int digits = 0;
    for (;; s++, digits++) {
      int d = (*s >= '0' && *s <= '9') ? *s - '0' : ((*s | 32) >= 'a' && (*s | 32) <= 'f') ? (*s | 32) - 'a' + 10 : -1;
      if (d < 0)
        break;
      v = v * 16 + (unsigned)d;
      if (digits >= 4)
        return 0;
    }
    if (!digits || nh + nt >= 8)
      return 0;
    if (gap)
      tail[nt++] = (unsigned short)v;
    else
      head[nh++] = (unsigned short)v;
    if (*s == ':') {
      s++;
      if (*s == ':') {
        if (gap)
          return 0;
        gap = 1;
        s++;
      } else if (!*s || *s == '%') {
        return 0;
      }
    } else if (*s && *s != '%') {
      return 0;
    }
  }
  if (gap ? nh + nt > 7 : nh != 8)
    return 0;
  memset(out, 0, 16);
  for (int i = 0; i < nh; i++) {
    out[2 * i] = (unsigned char)(head[i] >> 8);
    out[2 * i + 1] = (unsigned char)head[i];
  }
  for (int i = 0; i < nt; i++) {
    out[16 - 2 * (nt - i)] = (unsigned char)(tail[i] >> 8);
    out[16 - 2 * (nt - i) + 1] = (unsigned char)tail[i];
  }
  return 1;
}

/* True if the kernel holds a global IPv6 address, i.e. a v6 destination can be reached beyond the link. */
static int have_global_v6(void)
{
  char b[48];
  int n = readfile("/sys/class/net/eth0/ip6addr", b, (int)sizeof b - 1);
  return n > 1; /* just "\n" means none */
}

#define NS_MAX 3
#define NS_LEN 40

/* Collect up to NS_MAX nameservers into list: "nameserver ADDR" lines of /etc/resolv.conf (IPv4 or IPv6),
 * then the router-advertised DNS server. IPv6 servers are skipped without a global address. Returns the count
 * (0 if none is configured). */
static int read_nameservers(char list[NS_MAX][NS_LEN])
{
  int count = 0, v6 = have_global_v6();
  char buf[512];
  int n = readfile("/etc/resolv.conf", buf, (int)sizeof buf - 1);
  if (n < 0)
    n = 0;
  buf[n] = '\0';

  for (const char *p = buf; *p && count < NS_MAX;) {
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
    char tok[NS_LEN];
    int k = 0;
    while (k < NS_LEN - 1 && a[k] && a[k] != ' ' && a[k] != '\t' && a[k] != '\r' && a[k] != '\n')
      tok[k] = a[k], k++;
    tok[k] = '\0';
    unsigned char got[16];
    if (parse_ipv4(tok, got) || (v6 && parse_ipv6(tok, got)))
      memcpy(list[count++], tok, k + 1);
  }

  char ra[48];
  n = v6 ? readfile("/sys/class/net/eth0/dns6", ra, (int)sizeof ra - 1) : 0;
  if (n > 1 && count < NS_MAX) {
    ra[n] = '\0';
    for (int i = 0; i < n; i++)
      if (ra[i] == '\n')
        ra[i] = '\0';
    unsigned char got[16];
    if (parse_ipv6(ra, got))
      memcpy(list[count++], ra, strlen(ra) + 1);
  }
  return count;
}

int resolve_nameserver(char *out, int cap)
{
  char list[NS_MAX][NS_LEN];
  if (read_nameservers(list) == 0 || (int)strlen(list[0]) >= cap)
    return 0;
  memcpy(out, list[0], strlen(list[0]) + 1);
  return 1;
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* Send a `qtype` query for `host` to each nameserver in turn (port 53). Returns the first reply length in r
 * (>= 12), 0 on miss/timeout, or NET_EINTR on Ctrl+C. */
static int dns_exchange(const char *host, int qtype, unsigned char *r, int rmax)
{
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

  char servers[NS_MAX][NS_LEN];
  int ns = read_nameservers(servers);
  for (int i = 0; i < ns; i++) {
    for (int tries = 0; tries < (ns == 1 ? 2 : 1); tries++) {
      int h = net_connect(servers[i], 53, SK_UDP);
      if (h < 0) {
        if (h == NET_EINTR)
          return NET_EINTR;
        break;
      }
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
  }
  return 0;
}

#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28

/* First A (4 bytes) or AAAA (16 bytes) address of `host`. 1 = resolved, 0 = miss/timeout, NET_EINTR on Ctrl+C. */
static int dns_lookup(const char *host, int qtype, unsigned char *ip)
{
  int alen = qtype == DNS_TYPE_A ? 4 : 16;
  unsigned char r[1500];
  int rn = dns_exchange(host, qtype, r, (int)sizeof r);
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
    if (type == qtype && rdlen == alen) { /* first matching record     */
      for (int k = 0; k < alen; k++)
        ip[k] = r[pos + k];
      return 1;
    }
    pos += rdlen; /* CNAME etc.: keep scanning  */
  }
  return 0; /* answered, but no such record */
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
  return dns_lookup(host, DNS_TYPE_A, ip);
}

int resolve6(const char *host, unsigned char ip[16])
{
  unsigned char v4[4];
  if (!host || !*host)
    return 0;
  if (parse_ipv6(host, ip))
    return 1;
  if (parse_ipv4(host, v4) || is_local(host)) /* no AAAA for v4 literals or mDNS names */
    return 0;
  return dns_lookup(host, DNS_TYPE_AAAA, ip);
}

/* Write a dotted quad (v4) or 8 colon-separated hex groups (v6) into out (>= 40 bytes). */
static void format_ip(const unsigned char *a, int v6, char *out)
{
  int n = 0;
  for (int i = 0; i < (v6 ? 8 : 4); i++) {
    unsigned v = v6 ? ((unsigned)a[2 * i] << 8) | a[2 * i + 1] : a[i];
    unsigned base = v6 ? 16 : 10;
    char t[4];
    int k = 0;
    do {
      t[k++] = "0123456789abcdef"[v % base];
      v /= base;
    } while (v);
    while (k)
      out[n++] = t[--k];
    out[n++] = v6 ? ':' : '.';
  }
  out[n - 1] = '\0';
}

static int dial_ip(const unsigned char *ip, int v6, int port, int proto, int *fam)
{
  char s[40];
  format_ip(ip, v6, s);
  if (fam)
    *fam = v6 ? 6 : 4;
  return net_connect(s, port, proto);
}

/* Names: TCP tries IPv6 first when a global address is held, then falls back to IPv4. Datagram
 * sockets have no connect failure to fall back on, so they use IPv6 only for names with no A record.
 * *fam (if given) restricts the family on entry (4 or 6; 0 = either) and reports the one used. */
int net_dial_fam(const char *host, int port, int proto, int *fam)
{
  int want = fam ? *fam : 0;
  unsigned char ip[16], ip4[4];
  if (parse_ipv6(host, ip))
    return want == 4 ? -2 : dial_ip(ip, 1, port, proto, fam);

  int global = want == 0 && have_global_v6();
  int first6 = want == 6 || (global && proto == SK_TCP);
  if (first6) {
    int r = resolve6(host, ip);
    if (r == NET_EINTR)
      return NET_EINTR;
    if (r == 1) {
      int h = dial_ip(ip, 1, port, proto, fam);
      if (h >= 0 || h == NET_EINTR || want == 6)
        return h;
    } else if (want == 6) {
      return -2;
    }
  }
  int r = resolve(host, ip4);
  if (r == NET_EINTR)
    return NET_EINTR;
  if (r == 1)
    return dial_ip(ip4, 0, port, proto, fam);
  if (global && !first6) {
    r = resolve6(host, ip);
    if (r == NET_EINTR)
      return NET_EINTR;
    if (r == 1)
      return dial_ip(ip, 1, port, proto, fam);
  }
  return -2;
}

int net_dial(const char *host, int port, int proto) { return net_dial_fam(host, port, proto, 0); }

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

  unsigned char r[1500];
  int rn = dns_exchange(name, DNS_TYPE_HTTPS, r, (int)sizeof r);
  return rn > 0 ? svcb_find_ech(r, rn, out, max) : 0;
}
