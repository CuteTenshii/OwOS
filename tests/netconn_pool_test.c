/* Host test for the netconn idle pool (user/lib/netconn/netconn.c) over fake sockets and a controllable clock;
 * netconn.c is built with its system calls renamed to the fakes below. */
#define OWOS_SUITE_TEST
#include "netconn.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

static unsigned g_now = 1000;
static int g_next_fd, g_dials;
static int g_closed[64], g_nclosed;
static int g_dead_fd = -1; /* the peer of this fd has hung up */

unsigned ncp_msec(void) { return g_now; }

int ncp_dial(const char *host, int port, int proto)
{
  (void)host, (void)port, (void)proto;
  g_dials++;
  return ++g_next_fd;
}

int ncp_close(int fd)
{
  g_closed[g_nclosed++] = fd;
  return 0;
}

int ncp_recv_nb(int fd, void *buf, int max)
{
  (void)buf, (void)max;
  return fd == g_dead_fd ? -1 : 0; /* 0: would block, the connection is quiet */
}

int ncp_recv(int fd, void *buf, int max)
{
  (void)fd, (void)buf, (void)max;
  return 0;
}

int ncp_send(int fd, const void *buf, int len)
{
  (void)fd, (void)buf;
  return len;
}

typedef struct tls_conn tls_conn;
static int g_tls_obj, g_tls_closed, g_tls_allow_data, g_tls_closes;

tls_conn *ncp_tls_client(const char *host, int port, int offer_h2)
{
  (void)host, (void)port, (void)offer_h2;
  g_dials++;
  return (tls_conn *)&g_tls_obj;
}
int ncp_tls_peer_closed(tls_conn *c, int allow_data)
{
  (void)c;
  g_tls_allow_data = allow_data;
  return g_tls_closed;
}
int ncp_tls_last_error(void) { return 0; }
int ncp_tls_read(tls_conn *c, void *buf, int max) { return (void)c, (void)buf, (void)max, 0; }
int ncp_tls_write(tls_conn *c, const void *buf, int len) { return (void)c, (void)buf, len; }
int ncp_tls_alpn(tls_conn *c) { return (void)c, 0; }
const char *ncp_tls_verify_message(tls_conn *c) { return (void)c, (const char *)0; }
void ncp_tls_close(tls_conn *c)
{
  (void)c;
  g_tls_closes++;
}

static void reset(void)
{
  g_now += 100000; /* expires anything a previous test left parked */
  nc_idle_take("never", 1, 0, NC_IDLE_H1);
  g_dials = g_nclosed = g_tls_closes = 0;
  g_dead_fd = -1;
  g_tls_closed = 0;
}

static int was_closed(int fd)
{
  for (int i = 0; i < g_nclosed; i++)
    if (g_closed[i] == fd)
      return 1;
  return 0;
}

static void preconnect_serves_one_open(void)
{
  reset();
  nc_preconnect("example.com", 80, 0, 1);
  expect_eq(g_dials, 1);
  nc_preconnect("example.com", 80, 0, 1); /* already parked */
  expect_eq(g_dials, 1);

  netconn *c = nc_open("example.com", 80, 0, 1);
  expect(c != 0);
  expect(nc_pooled(c));
  expect_eq(g_dials, 1);
  netconn *d = nc_open("example.com", 80, 0, 1);
  expect(!nc_pooled(d));
  expect_eq(g_dials, 2);
  nc_close(c);
  nc_close(d);
}

static void open_only_takes_a_matching_preconnect(void)
{
  reset();
  nc_preconnect("example.com", 80, 0, 1);
  netconn *c = nc_open("example.com", 80, 0, 0); /* offer_h2 differs */
  expect(!nc_pooled(c));
  netconn *d = nc_open("other.example", 80, 0, 1);
  expect(!nc_pooled(d));
  netconn *e = nc_open("example.com", 8080, 0, 1);
  expect(!nc_pooled(e));
  netconn *f = nc_open("example.com", 80, 0, 1);
  expect(nc_pooled(f));
  nc_close(c), nc_close(d), nc_close(e), nc_close(f);
}

static void released_connection_goes_to_acquire_only(void)
{
  reset();
  netconn *c = nc_open("example.com", 80, 0, 1);
  nc_release(c, "example.com", 80, 0);
  netconn *o = nc_open("example.com", 80, 0, 1); /* nc_open never hands out a used connection */
  expect(!nc_pooled(o));
  netconn *a = nc_acquire("example.com", 80, 0);
  expect(a == c);
  expect(nc_pooled(a));
  nc_close(o), nc_close(a);
}

static void idle_connections_expire(void)
{
  reset();
  netconn *c = nc_open("example.com", 80, 0, 1);
  nc_release(c, "example.com", 80, 0);
  g_now += 6000; /* past the keep-alive TTL */
  netconn *a = nc_acquire("example.com", 80, 0);
  expect(!nc_pooled(a));
  expect(was_closed(g_next_fd - 1)); /* the parked one was dropped */
  nc_close(a);
}

static void hung_up_connection_is_discarded(void)
{
  reset();
  netconn *c = nc_open("example.com", 80, 0, 1);
  nc_release(c, "example.com", 80, 0);
  g_dead_fd = g_next_fd;
  netconn *a = nc_acquire("example.com", 80, 0);
  expect(!nc_pooled(a));
  expect(was_closed(g_dead_fd));
  nc_close(a);
}

static void tls_connection_closed_by_the_peer_is_discarded(void)
{
  reset();
  nc_preconnect("secure.example", 443, 1, 1);
  netconn *a = nc_open("secure.example", 443, 1, 1);
  expect(nc_pooled(a));
  nc_release(a, "secure.example", 443, 1);

  g_tls_closed = 1; /* close_notify arrived while it sat idle */
  netconn *b = nc_acquire("secure.example", 443, 1);
  expect(!nc_pooled(b));
  expect_eq(g_tls_closes, 1);
  expect_eq(g_tls_allow_data, 0); /* an HTTP/1.1 peer must stay silent between requests */

  g_tls_closed = 0;
  nc_release(b, "secure.example", 443, 1);
  netconn *c = nc_acquire("secure.example", 443, 1);
  expect(nc_pooled(c));
  nc_close(c);
}

static int g_destroyed, g_obj_closed;
static void destroy_obj(void *o)
{
  (void)o;
  g_destroyed++;
}
static int obj_closed(void *o)
{
  (void)o;
  return g_obj_closed;
}

static void generic_entries_are_taken_by_kind_and_destroyed_on_expiry(void)
{
  reset();
  g_destroyed = 0;
  int obj;
  nc_idle_put("example.com", 443, 1, NC_IDLE_H2, &obj, destroy_obj, 0);
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H1) == 0);
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H2) == &obj);
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H2) == 0);
  expect_eq(g_destroyed, 0);

  nc_idle_put("example.com", 443, 1, NC_IDLE_H2, &obj, destroy_obj, 0);
  g_now += 31000;
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H2) == 0);
  expect_eq(g_destroyed, 1);
}

static void h3_entries_expire_after_15_seconds(void)
{
  reset();
  g_destroyed = 0;
  int obj;
  nc_idle_put("example.com", 443, 1, NC_IDLE_H3, &obj, destroy_obj, 0);
  g_now += 14000;
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H3) == &obj);
  nc_idle_put("example.com", 443, 1, NC_IDLE_H3, &obj, destroy_obj, 0);
  g_now += 16000;
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H3) == 0);
  expect_eq(g_destroyed, 1);
}

static void idle_entry_with_a_hung_up_peer_is_destroyed(void)
{
  reset();
  g_destroyed = g_obj_closed = 0;
  int obj;
  nc_idle_put("example.com", 443, 1, NC_IDLE_H2, &obj, destroy_obj, obj_closed);
  g_obj_closed = 1;
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H2) == 0);
  expect_eq(g_destroyed, 1);
  g_obj_closed = 0;
  nc_idle_put("example.com", 443, 1, NC_IDLE_H2, &obj, destroy_obj, obj_closed);
  expect(nc_idle_take("example.com", 443, 1, NC_IDLE_H2) == &obj);
  expect_eq(g_destroyed, 1);
}

static void a_full_pool_drops_the_oldest(void)
{
  reset();
  netconn *first = nc_open("h0", 80, 0, 1);
  int first_fd = g_next_fd;
  nc_release(first, "h0", 80, 0);
  for (int i = 1; i <= 16; i++) {
    g_now += 10;
    char host[8];
    snprintf(host, sizeof host, "h%d", i);
    nc_release(nc_open(host, 80, 0, 1), host, 80, 0);
  }
  expect(was_closed(first_fd));
  netconn *gone = nc_acquire("h0", 80, 0);
  expect(!nc_pooled(gone));
  netconn *kept = nc_acquire("h16", 80, 0);
  expect(nc_pooled(kept));
  nc_close(gone), nc_close(kept);
}

int main(void)
{
  test("preconnect_serves_one_open", preconnect_serves_one_open);
  test("open_only_takes_a_matching_preconnect", open_only_takes_a_matching_preconnect);
  test("released_connection_goes_to_acquire_only", released_connection_goes_to_acquire_only);
  test("idle_connections_expire", idle_connections_expire);
  test("hung_up_connection_is_discarded", hung_up_connection_is_discarded);
  test("generic_entries_are_taken_by_kind_and_destroyed_on_expiry",
       generic_entries_are_taken_by_kind_and_destroyed_on_expiry);
  test("tls_connection_closed_by_the_peer_is_discarded", tls_connection_closed_by_the_peer_is_discarded);
  test("h3_entries_expire_after_15_seconds", h3_entries_expire_after_15_seconds);
  test("idle_entry_with_a_hung_up_peer_is_destroyed", idle_entry_with_a_hung_up_peer_is_destroyed);
  test("a_full_pool_drops_the_oldest", a_full_pool_drops_the_oldest);
  return 0;
}
