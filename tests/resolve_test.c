/* resolve_test.c: host test for user/lib/resolve/resolve.c (IPv6 literals, AAAA lookups, net_dial family order)
 * against a scripted DNS server. Run with `make test`. */
#define OWOS_SUITE_TEST
#include "owos.h"
#include <stdint.h>
#include <string.h>

#define net_dial rt_net_dial
#define net_dial_fam rt_net_dial_fam
#define resolve rt_resolve
#define resolve6 rt_resolve6
#define resolve_ech rt_resolve_ech
#define resolve_nameserver rt_resolve_nameserver
#define net_connect rt_net_connect
#define net_send rt_net_send
#define net_recv rt_net_recv
#define net_close rt_net_close
#define msec rt_msec
#define mdns_resolve rt_mdns_resolve
#define dns_name_len rt_dns_name_len
#define svcb_find_ech rt_svcb_find_ech
#define readfile(name, buf, max) rt_readfile(name, buf, max)

int rt_net_connect(const char *host, int port, int proto);
int rt_net_send(int h, const void *buf, int len);
int rt_net_recv(int h, void *buf, int max);
int rt_net_close(int h);
unsigned rt_msec(void);
int rt_readfile(const char *name, char *buf, int max);

#define be16 svcb_be16
#include "../user/lib/resolve/svcb.c"
#undef be16
#include "../user/lib/resolve/resolve.c"

#define H_DNS 7

static char g_a_ip[4]; /* A answer, or all-zero for none */
static unsigned char g_aaaa_ip[16];
static int g_has_aaaa;
static int g_has_a;
static const char *g_ip6addr = "\n"; /* contents of /sys/class/net/eth0/ip6addr */
static int g_v6_connect_fails;
static const char *g_resolv;               /* /etc/resolv.conf contents, NULL = absent */
static const char *g_dns6 = "\n";          /* /sys/class/net/eth0/dns6 */
static const char *g_silent;               /* DNS server that never answers */
static char g_tried[8][40], g_cur_dns[40]; /* servers queried, in order */
static int g_ntried;

static int g_nconnect, g_nquery_a, g_nquery_aaaa;
static char g_last_host[64];
static int g_last_port;
static unsigned char g_query[512];
static int g_query_len;

unsigned rt_msec(void) { return 0x1234; }
int rt_mdns_resolve(const char *host, uint8_t ip[4]) { return (void)host, (void)ip, 0; }
int rt_net_close(int h) { return (void)h, 0; }

int rt_readfile(const char *name, char *buf, int max)
{
  const char *src = !strcmp(name, "/sys/class/net/eth0/ip6addr") ? g_ip6addr
                    : !strcmp(name, "/sys/class/net/eth0/dns6")  ? g_dns6
                    : !strcmp(name, "/etc/resolv.conf")          ? g_resolv
                                                                 : NULL;
  if (!src)
    return -1;
  int n = (int)strlen(src);
  if (n > max)
    n = max;
  memcpy(buf, src, (size_t)n);
  return n;
}

int rt_net_connect(const char *host, int port, int proto)
{
  g_nconnect++;
  snprintf(g_last_host, sizeof g_last_host, "%s", host);
  g_last_port = port;
  if (proto == SK_UDP && port == 53) {
    snprintf(g_cur_dns, sizeof g_cur_dns, "%s", host);
    if (g_ntried < 8)
      snprintf(g_tried[g_ntried++], sizeof g_tried[0], "%s", host);
    return H_DNS;
  }
  if (strchr(host, ':') && g_v6_connect_fails)
    return -3;
  return 3;
}

int rt_net_send(int h, const void *buf, int len)
{
  if (h != H_DNS)
    return len;
  g_query_len = len;
  memcpy(g_query, buf, (size_t)len);
  int qtype = (g_query[len - 4] << 8) | g_query[len - 3];
  g_nquery_a += qtype == 1;
  g_nquery_aaaa += qtype == 28;
  return len;
}

int rt_net_recv(int h, void *buf, int max)
{
  (void)max;
  if (h != H_DNS || (g_silent && !strcmp(g_cur_dns, g_silent)))
    return 0;
  int qtype = (g_query[g_query_len - 4] << 8) | g_query[g_query_len - 3];
  unsigned char *r = buf;
  memcpy(r, g_query, (size_t)g_query_len);
  r[2] = 0x81;
  r[3] = 0x80;
  int have = qtype == 1 ? g_has_a : g_has_aaaa;
  int n = g_query_len;
  r[6] = 0;
  r[7] = (unsigned char)have;
  if (have) {
    int rdlen = qtype == 1 ? 4 : 16;
    unsigned char rr[10] = {0xC0, 0x0C, 0, (unsigned char)qtype, 0, 1, 0, 0, 0, 60};
    memcpy(r + n, rr, sizeof rr);
    n += (int)sizeof rr;
    r[n++] = 0;
    r[n++] = (unsigned char)rdlen;
    memcpy(r + n, qtype == 1 ? (unsigned char *)g_a_ip : g_aaaa_ip, (size_t)rdlen);
    n += rdlen;
  }
  return n;
}

static void reset(void)
{
  g_nconnect = g_nquery_a = g_nquery_aaaa = 0;
  g_has_a = g_has_aaaa = g_v6_connect_fails = 0;
  g_ip6addr = "\n";
  g_resolv = "nameserver 10.0.2.3\n";
  g_dns6 = "\n";
  g_silent = NULL;
  g_ntried = 0;
  g_last_host[0] = '\0';
  g_last_port = 0;
  static const char a4[4] = {93, 184, 216, 34};
  memcpy(g_a_ip, a4, 4);
  static const unsigned char a6[16] = {0x26, 0x06, 0x28, 0x00, 0x02, 0x20, 0x00, 0x01,
                                       0x02, 0x48, 0x18, 0x93, 0x25, 0xc8, 0x19, 0x46};
  memcpy(g_aaaa_ip, a6, 16);
}

static void test_literals(void)
{
  unsigned char ip[16];
  static const unsigned char loop[16] = {[15] = 1};
  expect(resolve6("::1", ip) == 1);
  expect(memcmp(ip, loop, 16) == 0);

  expect(resolve6("2001:db8::ff00:42:8329", ip) == 1);
  static const unsigned char want[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0x00, 0x00, 0x42, 0x83, 0x29};
  expect(memcmp(ip, want, 16) == 0);

  expect(resolve6("1:2:3:4:5:6:7:8", ip) == 1);
  expect(ip[0] == 0 && ip[1] == 1 && ip[14] == 0 && ip[15] == 8);

  expect(resolve6("fe80::1%eth0", ip) == 1);
  expect(ip[0] == 0xfe && ip[1] == 0x80 && ip[15] == 1);

  expect(resolve6("::", ip) == 1);
  expect(memcmp(ip, (unsigned char[16]){0}, 16) == 0);

  reset();
  const char *bad[] = {":::", "1::2::3", "12345::1", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", "::1::", "1:", ":1", "g::1"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    g_nquery_aaaa = 0;
    resolve6(bad[i], ip); /* falls through to DNS, which has no answer */
    expect(g_nquery_aaaa == 1);
  }
  expect(g_nconnect == (int)(sizeof bad / sizeof *bad)); /* only the DNS socket was opened */
}

static void test_no_dns_for_v4_literal_or_local(void)
{
  unsigned char ip[16];
  reset();
  expect(resolve6("10.1.2.3", ip) == 0);
  expect(resolve6("printer.local", ip) == 0);
  expect(g_nconnect == 0);
}

static void test_aaaa_lookup(void)
{
  unsigned char ip[16];
  reset();
  g_has_aaaa = 1;
  expect(resolve6("example.com", ip) == 1);
  expect(memcmp(ip, g_aaaa_ip, 16) == 0);
  expect(g_nquery_aaaa == 1 && g_nquery_a == 0);

  reset();
  expect(resolve6("example.com", ip) == 0);
}

static const char *GLOBAL = "fec0::5054:ff:fe12:3456\n";

static void test_dial_v6_literal(void)
{
  int fam = 0;
  reset();
  expect(net_dial_fam("2001:db8::1", 443, SK_TCP, &fam) == 3);
  expect(fam == 6);
  expect_string(g_last_host, "2001:db8:0:0:0:0:0:1");
  expect(g_last_port == 443);
  expect(g_nquery_a + g_nquery_aaaa == 0);
}

static void test_dial_v4_when_no_global_v6(void)
{
  int fam = 0;
  reset();
  g_has_a = g_has_aaaa = 1;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == 3);
  expect(fam == 4);
  expect_string(g_last_host, "93.184.216.34");
  expect(g_nquery_aaaa == 0);
}

static void test_dial_tcp_prefers_v6_with_global_address(void)
{
  int fam = 0;
  reset();
  g_ip6addr = GLOBAL;
  g_has_a = g_has_aaaa = 1;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == 3);
  expect(fam == 6);
  expect_string(g_last_host, "2606:2800:220:1:248:1893:25c8:1946");
  expect(g_nquery_a == 0);
}

static void test_dial_tcp_falls_back_to_v4(void)
{
  int fam = 0;
  reset();
  g_ip6addr = GLOBAL;
  g_has_a = g_has_aaaa = 1;
  g_v6_connect_fails = 1;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == 3);
  expect(fam == 4);
  expect_string(g_last_host, "93.184.216.34");
}

static void test_dial_udp_uses_v4_unless_name_is_v6_only(void)
{
  int fam = 0;
  reset();
  g_ip6addr = GLOBAL;
  g_has_a = g_has_aaaa = 1;
  expect(net_dial_fam("example.com", 123, SK_UDP, &fam) == 3);
  expect(fam == 4);
  expect(g_nquery_aaaa == 0);

  reset();
  g_ip6addr = GLOBAL;
  g_has_aaaa = 1;
  fam = 0;
  expect(net_dial_fam("v6only.example", 123, SK_UDP, &fam) == 3);
  expect(fam == 6);
  expect_string(g_last_host, "2606:2800:220:1:248:1893:25c8:1946");
}

static void test_dial_forced_family(void)
{
  int fam = 6;
  reset();
  g_has_a = g_has_aaaa = 1; /* no global address: -6 still looks up AAAA */
  expect(net_dial_fam("example.com", 80, SK_ICMP, &fam) == 3);
  expect(fam == 6);
  expect_string(g_last_host, "2606:2800:220:1:248:1893:25c8:1946");

  reset();
  g_ip6addr = GLOBAL;
  g_has_a = g_has_aaaa = 1;
  fam = 4;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == 3);
  expect(fam == 4);
  expect(g_nquery_aaaa == 0);

  reset();
  g_has_a = 1; /* only an A record */
  fam = 6;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == -2);
  fam = 6;
  expect(net_dial_fam("10.1.2.3", 80, SK_TCP, &fam) == -2);
  fam = 4;
  expect(net_dial_fam("::1", 80, SK_TCP, &fam) == -2);
}

static void test_dial_unresolvable(void)
{
  reset();
  g_ip6addr = GLOBAL;
  expect(net_dial("nothing.example", 80, SK_TCP) == -2);
  reset();
  expect(net_dial("2001:db8::bad", 80, SK_TCP) == 3); /* literal: no lookup, caller sees connect result */
}

static void test_nameserver_list(void)
{
  unsigned char ip[16];
  reset();
  g_resolv = "nameserver 192.0.2.53\nnameserver 2001:db8::53\nsearch lan\n";
  g_has_aaaa = 1;
  expect(resolve6("example.com", ip) == 1);
  expect(g_ntried == 1 && !strcmp(g_tried[0], "192.0.2.53")); /* IPv6 server skipped: no global address */

  reset();
  g_ip6addr = GLOBAL;
  g_resolv = "nameserver 192.0.2.53\nnameserver 2001:db8::53\n";
  g_dns6 = "fec0::3\n";
  g_silent = "192.0.2.53";
  g_has_aaaa = 1;
  expect(resolve6("example.com", ip) == 1);
  expect(g_ntried == 2 && !strcmp(g_tried[0], "192.0.2.53") && !strcmp(g_tried[1], "2001:db8::53"));

  reset();
  g_ip6addr = GLOBAL;
  g_dns6 = "fec0::3\n"; /* router-advertised server is the last resort */
  g_silent = "10.0.2.3";
  g_has_aaaa = 1;
  expect(resolve6("example.com", ip) == 1);
  expect(g_ntried >= 2 && !strcmp(g_tried[g_ntried - 1], "fec0::3"));
}

static void test_no_nameserver_no_lookup(void)
{
  unsigned char ip[4];
  reset();
  g_resolv = NULL;
  g_has_a = 1;
  expect(resolve("example.com", ip) == 0);
  expect_eq(g_nconnect, 0); /* nothing is sent anywhere */
  int fam = 0;
  expect(net_dial_fam("example.com", 80, SK_TCP, &fam) == -2);
  expect_eq(g_nconnect, 0);
}

static void test_first_configured_nameserver(void)
{
  char ns[40];
  reset();
  g_resolv = NULL;
  expect(!resolve_nameserver(ns, sizeof ns)); /* none configured */

  g_resolv = "# comment\nnameserver 192.0.2.53\nnameserver 192.0.2.54\n";
  expect(resolve_nameserver(ns, sizeof ns));
  expect_string(ns, "192.0.2.53");
  expect(!resolve_nameserver(ns, 5));
}

int main(void)
{
  test("IPv6 literal parsing", test_literals);
  test("no AAAA query for IPv4 literals or .local", test_no_dns_for_v4_literal_or_local);
  test("AAAA lookup", test_aaaa_lookup);
  test("dial an IPv6 literal", test_dial_v6_literal);
  test("dial stays IPv4 without a global IPv6 address", test_dial_v4_when_no_global_v6);
  test("TCP dial prefers IPv6 with a global address", test_dial_tcp_prefers_v6_with_global_address);
  test("TCP dial falls back to IPv4 when IPv6 connect fails", test_dial_tcp_falls_back_to_v4);
  test("UDP dial uses IPv4 unless the name is IPv6-only", test_dial_udp_uses_v4_unless_name_is_v6_only);
  test("forced address family", test_dial_forced_family);
  test("dial of an unresolvable name", test_dial_unresolvable);
  test("nameserver list: resolv.conf, IPv6, router-advertised", test_nameserver_list);
  test("no nameserver, no lookup", test_no_nameserver_no_lookup);
  test("first configured nameserver", test_first_configured_nameserver);
  return 0;
}
