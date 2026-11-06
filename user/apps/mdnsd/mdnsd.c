/* mdnsd.c, the mDNS (RFC 6762) responder / DNS-SD (RFC 6763) advertiser, in user space.
 * Autostarted detached at boot */

#include "owos.h"
#include <stdbool.h>
#include <stdint.h>

enum { MDNS_A = 1, MDNS_PTR = 12, MDNS_TXT = 16, MDNS_SRV = 33, MDNS_ANY = 255 };

#define MDNS_NAME 96         /* longest owner/target name we store (dotted) */
#define MDNS_TXTMAX 192      /* longest TXT rdata (length-prefixed strings) */
#define MDNS_MAXREC 32       /* records in the published set */
#define MDNS_TTL_UNIQUE 120  /* A/SRV/TXT: host-owned, short-lived */
#define MDNS_TTL_SHARED 4500 /* PTR: shared, long-lived (RFC 6762 §10) */
#define MDNS_PORT 5353

static const unsigned char mdns_group[4] = {224, 0, 0, 251};

/* One published resource record */
struct mdns_record {
  char name[MDNS_NAME]; /* owner name (dotted, no trailing dot) */
  uint16_t type;
  uint16_t port;            /* SRV */
  char rname[MDNS_NAME];    /* PTR/SRV target (dotted) */
  uint8_t txt[MDNS_TXTMAX]; /* TXT rdata */
  uint16_t txtlen;
};

/* pure wire logic */

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* Case-insensitive name compare, tolerating one optional trailing dot on either side */
static bool name_eq(const char *a, const char *b)
{
  for (;;) {
    char ca = *a, cb = *b;
    if ((ca == '\0' || (ca == '.' && a[1] == '\0')) && (cb == '\0' || (cb == '.' && b[1] == '\0')))
      return true;
    if (lower(ca) != lower(cb))
      return false;
    a++;
    b++;
  }
}

/* In-stream length of a name at msg[pos]: labels until a 0 byte or a compression pointer */
static int name_ilen(const unsigned char *msg, int pos, int end)
{
  int p = pos;
  while (p < end) {
    unsigned b = msg[p];
    if (b == 0)
      return p + 1 - pos;
    if ((b & 0xC0) == 0xC0)
      return p + 2 - pos;
    p += 1 + (int)b;
  }
  return 0;
}

/* Decode a (possibly compressed) name at msg[pos] into out (dotted, no trailing dot) */
static void name_decode(const unsigned char *msg, int end, int pos, char *out, int outsz)
{
  int oi = 0, jumps = 0, p = pos;
  while (p >= 0 && p < end) {
    unsigned b = msg[p];
    if (b == 0)
      break;
    if ((b & 0xC0) == 0xC0) {
      if (p + 1 >= end || ++jumps > 16)
        break;
      p = ((int)(b & 0x3F) << 8) | msg[p + 1];
      continue;
    }
    p++;
    for (unsigned i = 0; i < b && p < end; i++, p++)
      if (oi < outsz - 1)
        out[oi++] = (char)msg[p];
    if (oi < outsz - 1)
      out[oi++] = '.';
  }
  if (oi && out[oi - 1] == '.')
    oi--;
  out[oi] = '\0';
}

/* Encode dotted `name` as DNS labels into out[*o..cap] */
static int put_name(unsigned char *out, int *o, int cap, const char *name)
{
  for (const char *s = name; *s;) {
    int l = 0;
    while (s[l] && s[l] != '.')
      l++;
    if (l > 63 || *o + 1 + l >= cap)
      return -1;
    out[(*o)++] = (unsigned char)l;
    for (int i = 0; i < l; i++)
      out[(*o)++] = (unsigned char)s[i];
    s += l;
    if (*s == '.')
      s++;
  }
  if (*o + 1 > cap)
    return -1;
  out[(*o)++] = 0; /* root label */
  return 0;
}

static int put_u16(unsigned char *out, int *o, int cap, unsigned v)
{
  if (*o + 2 > cap)
    return -1;
  out[(*o)++] = (unsigned char)(v >> 8);
  out[(*o)++] = (unsigned char)v;
  return 0;
}

static bool ip_zero(const unsigned char ip[4]) { return !(ip[0] | ip[1] | ip[2] | ip[3]); }

/* Encode one record (owner + type/class/ttl + rdata) at out[*o] */
static int put_record(unsigned char *out, int *o, int cap, const struct mdns_record *r, const unsigned char myip[4],
                      long ttl_override)
{
  if (r->type == MDNS_A && ip_zero(myip))
    return -1;
  if (put_name(out, o, cap, r->name) < 0)
    return -1;
  if (put_u16(out, o, cap, r->type) < 0)
    return -1;
  /* Class IN; A/SRV/TXT are host-unique so set the cache-flush bit (0x8000) */
  if (put_u16(out, o, cap, r->type == MDNS_PTR ? 0x0001 : 0x8001) < 0)
    return -1;
  unsigned ttl =
      (ttl_override >= 0) ? (unsigned)ttl_override : (r->type == MDNS_PTR ? MDNS_TTL_SHARED : MDNS_TTL_UNIQUE);
  if (put_u16(out, o, cap, ttl >> 16) < 0)
    return -1;
  if (put_u16(out, o, cap, ttl & 0xFFFF) < 0)
    return -1;

  int rdlen_at = *o; /* backpatch rdlen after the rdata */
  if (put_u16(out, o, cap, 0) < 0)
    return -1;
  int rd = *o;
  switch (r->type) {
  case MDNS_A:
    if (*o + 4 > cap)
      return -1;
    for (int i = 0; i < 4; i++)
      out[(*o)++] = myip[i];
    break;
  case MDNS_PTR:
    if (put_name(out, o, cap, r->rname) < 0)
      return -1;
    break;
  case MDNS_SRV:
    if (put_u16(out, o, cap, 0) < 0)
      return -1; /* priority */
    if (put_u16(out, o, cap, 0) < 0)
      return -1; /* weight */
    if (put_u16(out, o, cap, r->port) < 0)
      return -1;
    if (put_name(out, o, cap, r->rname) < 0)
      return -1;
    break;
  case MDNS_TXT:
    if (r->txtlen == 0) { /* min TXT = one empty str */
      if (*o + 1 > cap)
        return -1;
      out[(*o)++] = 0;
    } else {
      if (*o + r->txtlen > cap)
        return -1;
      for (int i = 0; i < r->txtlen; i++)
        out[(*o)++] = r->txt[i];
    }
    break;
  default:
    return -1;
  }
  unsigned rdlen = (unsigned)(*o - rd);
  out[rdlen_at] = (unsigned char)(rdlen >> 8);
  out[rdlen_at + 1] = (unsigned char)rdlen;
  return 0;
}

static bool idx_in(const int *arr, int n, int v)
{
  for (int i = 0; i < n; i++)
    if (arr[i] == v)
      return true;
  return false;
}

/* Build a response to the query datagram `q`/`qlen` against `recs`, using `myip` for A records */
static int mdns_build_response(const unsigned char *q, int qlen, const struct mdns_record *recs, int nrec,
                               const unsigned char myip[4], unsigned char *out, int outcap, int *want_unicast)
{
  *want_unicast = 0;
  if (qlen < 12)
    return 0;
  if (q[2] & 0x80)
    return 0; /* QR=1: a response, not a query */
  int qd = (int)be16(q + 4);

  int ans[MDNS_MAXREC * 2], nans = 0; /* answer-section record indices */
  int add[MDNS_MAXREC * 2], nadd = 0; /* additional-section indices */

  int pos = 12;
  for (int qi = 0; qi < qd && pos < qlen; qi++) {
    char qname[MDNS_NAME];
    int l = name_ilen(q, pos, qlen);
    if (!l || pos + l + 4 > qlen)
      break;
    name_decode(q, qlen, pos, qname, sizeof qname);
    int qtype = (int)be16(q + pos + l);
    int qclass = (int)be16(q + pos + l + 2);
    pos += l + 4;
    if (qclass & 0x8000)
      *want_unicast = 1; /* QU bit */

    for (int i = 0; i < nrec; i++) {
      if (!name_eq(recs[i].name, qname))
        continue;
      if (qtype != MDNS_ANY && qtype != recs[i].type)
        continue;
      if (recs[i].type == MDNS_A && ip_zero(myip))
        continue; /* no addr yet */
      if (!idx_in(ans, nans, i) && nans < (int)(sizeof ans / sizeof *ans))
        ans[nans++] = i;
    }
  }
  if (nans == 0)
    return 0;

  /* Knowledge amplification (RFC 6763 §12): a PTR answer drags in the target's SRV/TXT */
  int base = nans;
  for (int k = 0; k < base; k++) {
    if (recs[ans[k]].type != MDNS_PTR)
      continue;
    for (int i = 0; i < nrec; i++)
      if ((recs[i].type == MDNS_SRV || recs[i].type == MDNS_TXT) && name_eq(recs[i].name, recs[ans[k]].rname) &&
          !idx_in(ans, nans, i) && !idx_in(add, nadd, i) && nadd < (int)(sizeof add / sizeof *add))
        add[nadd++] = i;
  }
  int nadd_srv = nadd;
  for (int pass = 0; pass < 2; pass++) { /* SRVs in answers, then in additionals */
    const int *lst = pass ? add : ans;
    int n = pass ? nadd_srv : nans;
    for (int k = 0; k < n; k++) {
      if (recs[lst[k]].type != MDNS_SRV)
        continue;
      for (int i = 0; i < nrec; i++)
        if (recs[i].type == MDNS_A && !ip_zero(myip) && name_eq(recs[i].name, recs[lst[k]].rname) &&
            !idx_in(ans, nans, i) && !idx_in(add, nadd, i) && nadd < (int)(sizeof add / sizeof *add))
          add[nadd++] = i;
    }
  }

  int o = 0;
  if (outcap < 12)
    return 0;
  out[o++] = 0;
  out[o++] = 0; /* id 0 (mDNS convention) */
  out[o++] = 0x84;
  out[o++] = 0x00; /* flags: QR=1, AA=1 */
  out[o++] = 0;
  out[o++] = 0; /* QDCOUNT 0 (responses omit) */
  int an_at = o;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0; /* NSCOUNT 0 */
  int ar_at = o;
  out[o++] = 0;
  out[o++] = 0;

  int emitted = 0;
  for (int k = 0; k < nans; k++)
    if (put_record(out, &o, outcap, &recs[ans[k]], myip, -1) == 0)
      emitted++;
  int adddone = 0;
  for (int k = 0; k < nadd; k++)
    if (put_record(out, &o, outcap, &recs[add[k]], myip, -1) == 0)
      adddone++;

  if (emitted == 0)
    return 0;
  out[an_at] = (unsigned char)(emitted >> 8);
  out[an_at + 1] = (unsigned char)emitted;
  out[ar_at] = (unsigned char)(adddone >> 8);
  out[ar_at + 1] = (unsigned char)adddone;
  return o;
}

/* record table */

static struct mdns_record g_rec[MDNS_MAXREC];
static int g_nrec;
static char g_host[MDNS_NAME]; /* "<hostname>.local" */

static int rec_add(const struct mdns_record *r)
{
  /* Dedupe: identical type+name+target already published */
  for (int i = 0; i < g_nrec; i++)
    if (g_rec[i].type == r->type && name_eq(g_rec[i].name, r->name) && name_eq(g_rec[i].rname, r->rname))
      return 0;
  if (g_nrec >= MDNS_MAXREC)
    return -1;
  g_rec[g_nrec++] = *r;
  return 0;
}

/* dst = "<a>.<b>" into out (bounded) */
static void name_join(char *out, int cap, const char *a, const char *b)
{
  int o = 0;
  for (; a[o] && o < cap - 2; o++)
    out[o] = a[o];
  if (o < cap - 1)
    out[o++] = '.';
  int i = 0;
  while (b[i] && o < cap - 1)
    out[o++] = b[i++];
  out[o] = '\0';
}

static void str_copy(char *dst, int cap, const char *src)
{
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

/* Advertise a DNS-SD service (its PTR/SRV/TXT + the "_services._dns-sd._udp" enumeration entry) */
static int advertise(const char *instance, const char *type, uint16_t port, const char *const *txt, int ntxt)
{
  char fqdn[MDNS_NAME];
  name_join(fqdn, sizeof fqdn, instance, type);

  struct mdns_record ptr = {.type = MDNS_PTR};
  str_copy(ptr.name, sizeof ptr.name, type);
  str_copy(ptr.rname, sizeof ptr.rname, fqdn);

  struct mdns_record srv = {.type = MDNS_SRV, .port = port};
  str_copy(srv.name, sizeof srv.name, fqdn);
  str_copy(srv.rname, sizeof srv.rname, g_host);

  struct mdns_record rtxt = {.type = MDNS_TXT};
  str_copy(rtxt.name, sizeof rtxt.name, fqdn);
  for (int i = 0; i < ntxt && txt && txt[i]; i++) { /* pack length-prefixed */
    int l = strlen(txt[i]);
    if (l > 255 || rtxt.txtlen + 1 + l > MDNS_TXTMAX)
      break;
    rtxt.txt[rtxt.txtlen++] = (uint8_t)l;
    for (int j = 0; j < l; j++)
      rtxt.txt[rtxt.txtlen++] = (uint8_t)txt[i][j];
  }

  /* Service-type enumeration, so `dns-sd -B` / browsers find the type itself */
  struct mdns_record enu = {.type = MDNS_PTR};
  str_copy(enu.name, sizeof enu.name, "_services._dns-sd._udp.local");
  str_copy(enu.rname, sizeof enu.rname, type);

  if (rec_add(&ptr) < 0 || rec_add(&srv) < 0 || rec_add(&rtxt) < 0 || rec_add(&enu) < 0)
    return -1;
  return 0;
}

/* config parsing */

/* Parse /etc/mdns.conf: one "service_type port [txt...]" per line, '#' comment */
static void load_conf(const char *hostlabel)
{
  static char buf[2048];
  int n = readfile("/etc/mdns.conf", buf, (int)sizeof buf - 1);
  if (n <= 0)
    return;
  buf[n] = '\0';

  int i = 0;
  while (i < n) {
    /* one line -> tokens */
    char line[256];
    int ll = 0;
    while (i < n && buf[i] != '\n' && ll < (int)sizeof line - 1)
      line[ll++] = buf[i++];
    if (i < n && buf[i] == '\n')
      i++;
    line[ll] = '\0';

    /* split on whitespace; up to 6 fields (type, port, 4 txt) */
    char *tok[6];
    int nt = 0;
    int j = 0;
    while (line[j] && nt < 6) {
      while (line[j] == ' ' || line[j] == '\t' || line[j] == '\r')
        line[j++] = '\0';
      if (!line[j])
        break;
      tok[nt++] = &line[j];
      while (line[j] && line[j] != ' ' && line[j] != '\t' && line[j] != '\r')
        j++;
    }
    if (nt < 2 || tok[0][0] == '#')
      continue;

    int port = 0;
    for (const char *p = tok[1]; *p >= '0' && *p <= '9'; p++)
      port = port * 10 + (*p - '0');
    if (port <= 0 || port > 65535)
      continue;

    /* the conf carries the transport-qualified type without ".local" */
    char type[MDNS_NAME];
    name_join(type, sizeof type, tok[0], "local");

    const char *txt[4];
    int ntxt = 0;
    for (int k = 2; k < nt && ntxt < 4; k++)
      txt[ntxt++] = tok[k];
    advertise(hostlabel, type, (uint16_t)port, txt, ntxt);
  }
}

/* IP + send glue */

/* Read our current IPv4 from sysfs (reflects the dhcpcd lease) */
static void read_ip(unsigned char ip[4])
{
  char b[32];
  ip[0] = ip[1] = ip[2] = ip[3] = 0;
  int n = readfile("/sys/class/net/eth0/ipaddr", b, (int)sizeof b - 1);
  if (n <= 0)
    return;
  b[n] = '\0';
  int v = 0, part = 0;
  for (int i = 0; i <= n; i++) {
    char c = b[i];
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
    } else if (c == '.' || c == '\0' || c == '\n') {
      if (part < 4)
        ip[part++] = (unsigned char)v;
      v = 0;
      if (c != '.')
        break;
    } else
      break;
  }
}

/* Send `recs`[list] as an unsolicited response to the mDNS group */
static void announce(int fd, const unsigned char myip[4], const int *list, int n, long ttl_override)
{
  unsigned char out[1400];
  int o = 0;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0x84;
  out[o++] = 0x00; /* QR=1, AA=1 */
  out[o++] = 0;
  out[o++] = 0;
  int an_at = o;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  int emitted = 0;
  for (int k = 0; k < n; k++)
    if (put_record(out, &o, (int)sizeof out, &g_rec[list[k]], myip, ttl_override) == 0)
      emitted++;
  if (!emitted)
    return;
  out[an_at] = (unsigned char)(emitted >> 8);
  out[an_at + 1] = (unsigned char)emitted;
  net_sendto(fd, mdns_group, MDNS_PORT, out, o);
}

static void announce_all(int fd, const unsigned char myip[4], long ttl_override)
{
  int list[MDNS_MAXREC];
  int n = 0;
  for (int i = 0; i < g_nrec; i++)
    list[n++] = i;
  announce(fd, myip, list, n, ttl_override);
}

/* main loop */

int main(void)
{
  /* Publish our "<hostname>.local" A record up front */
  char host[64];
  int hn = readfile("/etc/hostname", host, (int)sizeof host - 1);
  if (hn <= 0) {
    host[0] = 'o';
    host[1] = 'w';
    host[2] = 'o';
    host[3] = 's';
    hn = 4;
  }
  while (hn > 0 && (host[hn - 1] == '\n' || host[hn - 1] == '\r' || host[hn - 1] == ' '))
    hn--;
  host[hn] = '\0';
  name_join(g_host, sizeof g_host, host, "local");

  struct mdns_record a = {.type = MDNS_A};
  str_copy(a.name, sizeof a.name, g_host);
  g_nrec = 0;
  rec_add(&a);
  load_conf(host);

  int fd = net_bind(MDNS_PORT);
  if (fd < 0) {
    puts("mdnsd: cannot bind port 5353\n");
    return 1;
  }
  net_set_ttl(fd, 255); /* RFC 6762: mDNS uses IP TTL 255 */

  unsigned char myip[4] = {0, 0, 0, 0};
  unsigned last_announce = 0;
  bool announced_once = false;

  for (;;) {
    unsigned char src[4];
    int sport = 0;
    unsigned char q[1500];
    int n = net_recvfrom(fd, q, (int)sizeof q, src, &sport);
    if (n == NET_EINTR) {
      if (!ip_zero(myip))
        announce_all(fd, myip, 0); /* goodbye on exit */
      net_close(fd);
      return EXIT_SIGINT;
    }

    read_ip(myip); /* track the dhcpcd lease */

    /* Announce once shortly after an address is held */
    if (!ip_zero(myip)) {
      unsigned now = msec();
      if (!announced_once || now - last_announce >= 60000) {
        announce_all(fd, myip, -1);
        last_announce = now;
        announced_once = true;
      }
    }

    if (n <= 0)
      continue; /* timeout / no datagram */

    unsigned char out[1400];
    int qu = 0;
    int rn = mdns_build_response(q, n, g_rec, g_nrec, myip, out, (int)sizeof out, &qu);
    if (rn <= 0)
      continue;

    if (sport != MDNS_PORT)
      net_sendto(fd, src, sport, out, rn);
    else if (qu)
      net_sendto(fd, src, MDNS_PORT, out, rn);
    else
      net_sendto(fd, mdns_group, MDNS_PORT, out, rn);
  }
}
