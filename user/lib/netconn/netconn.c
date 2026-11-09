/* netconn.c — TCP-or-TLS transport wrapper. See netconn.h. */
#include "netconn.h"
#include "mutex.h"
#include "owos.h"
#include "resolve.h"
#include "tls.h"

struct netconn {
  int fd;        /* SK_TCP socket, or -1 when this is a TLS conn */
  tls_conn *tls; /* user-space TLS conn, or NULL when plain TCP  */
  int pooled;    /* came out of the idle pool                    */
};

static int g_nc_error;

int nc_last_error(void) { return g_nc_error; }

static netconn *nc_dial(const char *host, int port, int tls, int offer_h2)
{
  g_nc_error = NC_OPEN_CONNECT;
  netconn *c = (netconn *)malloc(sizeof *c);
  if (!c)
    return 0;
  c->fd = -1;
  c->tls = 0;
  c->pooled = 0;
  if (tls) {
    c->tls = tls_client(host, port, offer_h2); /* self-resolves + SNI=host */
    if (!c->tls) {
      g_nc_error = tls_last_error() == TLS_OPEN_RESOLVE   ? NC_OPEN_RESOLVE
                   : tls_last_error() == TLS_OPEN_CONNECT ? NC_OPEN_CONNECT
                                                          : NC_OPEN_TLS;
      free(c);
      return 0;
    }
  } else {
    c->fd = net_dial(host, port, SK_TCP); /* resolve, then TCP-connect */
    if (c->fd < 0) {
      g_nc_error = c->fd == -2 ? NC_OPEN_RESOLVE : NC_OPEN_CONNECT;
      free(c);
      return 0;
    }
  }
  return c;
}

/* Idle pool: preconnected sockets, kept-alive HTTP/1.1 connections and persistent h2 sessions, each handed out once. */
#define POOL_MAX 16
#define POOL_HOST_MAX 128

struct idle {
  void *obj;
  void (*destroy)(void *);
  int (*closed)(void *);
  char host[POOL_HOST_MAX];
  int port, tls, kind, h2;
  unsigned t;
};
static struct idle g_idle[POOL_MAX];
static mutex_t g_idle_lock = MUTEX_INIT;

/* Servers drop idle HTTP/1.1 connections within seconds; h2 peers keep them far longer, QUIC ones less so. */
static unsigned idle_ttl(int kind)
{
  return kind == NC_IDLE_H2 ? 30000 : kind == NC_IDLE_H3 ? 15000 : kind == NC_IDLE_H1 ? 5000 : 10000;
}

static void nc_destroy(void *c) { nc_close(c); }
static int nc_conn_closed(void *c) { return nc_peer_closed(c, 0); }

static int idle_match(const struct idle *p, const char *host, int port, int tls, int kind, int h2)
{
  return p->obj && p->port == port && p->tls == tls && p->kind == kind && p->h2 == h2 && streq(p->host, host);
}

/* Move expired entries into dead[]; the caller destroys them outside the lock. */
static int idle_sweep(struct idle *dead, unsigned now)
{
  int nd = 0;
  for (int i = 0; i < POOL_MAX; i++)
    if (g_idle[i].obj && now - g_idle[i].t > idle_ttl(g_idle[i].kind)) {
      dead[nd++] = g_idle[i];
      g_idle[i].obj = 0;
    }
  return nd;
}

static void idle_put(const char *host, int port, int tls, int kind, int h2, void *obj, void (*destroy)(void *),
                     int (*closed)(void *))
{
  if (strlen(host) >= POOL_HOST_MAX) {
    destroy(obj);
    return;
  }
  struct idle dead[POOL_MAX + 1];
  unsigned now = msec();
  mutex_lock(&g_idle_lock);
  int nd = idle_sweep(dead, now);
  struct idle *slot = 0;
  for (int i = 0; i < POOL_MAX; i++)
    if (!g_idle[i].obj) {
      slot = &g_idle[i];
      break;
    }
  if (!slot) { /* full: replace the oldest */
    slot = &g_idle[0];
    for (int i = 1; i < POOL_MAX; i++)
      if (g_idle[i].t < slot->t)
        slot = &g_idle[i];
    dead[nd++] = *slot;
  }
  slot->obj = obj;
  slot->destroy = destroy;
  slot->closed = closed;
  strcpy(slot->host, host);
  slot->port = port;
  slot->tls = tls;
  slot->kind = kind;
  slot->h2 = h2;
  slot->t = now;
  mutex_unlock(&g_idle_lock);
  for (int i = 0; i < nd; i++)
    dead[i].destroy(dead[i].obj);
}

/* Remove and return a matching entry, or NULL. An entry whose peer has hung up is dropped. */
static void *idle_take(const char *host, int port, int tls, int kind, int h2)
{
  struct idle dead[POOL_MAX];
  unsigned now = msec();
  mutex_lock(&g_idle_lock);
  int nd = idle_sweep(dead, now);
  struct idle got = {0};
  for (int i = 0; i < POOL_MAX; i++)
    if (idle_match(&g_idle[i], host, port, tls, kind, h2)) {
      got = g_idle[i];
      g_idle[i].obj = 0;
      break;
    }
  mutex_unlock(&g_idle_lock);
  for (int i = 0; i < nd; i++)
    dead[i].destroy(dead[i].obj);
  if (got.obj && got.closed && got.closed(got.obj)) {
    got.destroy(got.obj);
    return 0;
  }
  return got.obj;
}

void nc_idle_put(const char *host, int port, int tls, int kind, void *obj, void (*destroy)(void *),
                 int (*closed)(void *))
{
  idle_put(host, port, tls, kind, 0, obj, destroy, closed);
}

void *nc_idle_take(const char *host, int port, int tls, int kind) { return idle_take(host, port, tls, kind, 0); }

netconn *nc_open_fresh(const char *host, int port, int tls, int offer_h2) { return nc_dial(host, port, tls, offer_h2); }

netconn *nc_open(const char *host, int port, int tls, int offer_h2)
{
  netconn *c = idle_take(host, port, tls, NC_IDLE_FRESH, offer_h2);
  if (c) {
    c->pooled = 1;
    return c;
  }
  return nc_dial(host, port, tls, offer_h2);
}

netconn *nc_acquire(const char *host, int port, int tls)
{
  netconn *c = idle_take(host, port, tls, NC_IDLE_H1, 0);
  if (c) {
    c->pooled = 1;
    return c;
  }
  return nc_open(host, port, tls, 1);
}

void nc_release(netconn *c, const char *host, int port, int tls)
{
  idle_put(host, port, tls, NC_IDLE_H1, 0, c, nc_destroy, nc_conn_closed);
}

int nc_pooled(netconn *c) { return c && c->pooled; }

void nc_preconnect(const char *host, int port, int tls, int offer_h2)
{
  mutex_lock(&g_idle_lock);
  int have = 0;
  for (int i = 0; i < POOL_MAX; i++)
    have |= idle_match(&g_idle[i], host, port, tls, NC_IDLE_FRESH, offer_h2);
  mutex_unlock(&g_idle_lock);
  if (have)
    return;
  netconn *c = nc_dial(host, port, tls, offer_h2);
  if (c)
    idle_put(host, port, tls, NC_IDLE_FRESH, offer_h2, c, nc_destroy, nc_conn_closed);
}

netconn *nc_from_fd(int fd)
{
  netconn *c = (netconn *)malloc(sizeof *c);
  if (!c)
    return 0;
  c->fd = fd;
  c->tls = 0;
  c->pooled = 0;
  return c;
}

int nc_read(netconn *c, void *buf, int max)
{
  if (!c)
    return -1;
  return c->tls ? tls_read(c->tls, buf, max) : net_recv(c->fd, buf, max);
}

int nc_read_nb(netconn *c, void *buf, int max)
{
  if (!c)
    return -1;
  if (c->tls)
    return -1; /* no non-blocking TLS read (as SK_TLS never had) */
  return net_recv_nb(c->fd, buf, max);
}

int nc_peer_closed(netconn *c, int allow_data)
{
  if (c->tls)
    return tls_peer_closed(c->tls, allow_data);
  char b;
  int n = net_recv_nb(c->fd, &b, 1); /* data on an idle connection means it can't be reused either */
  return n > 0 || (n < 0 && n != NET_EINTR);
}

int nc_write(netconn *c, const void *buf, int len)
{
  if (!c)
    return -1;
  return c->tls ? tls_write(c->tls, buf, len) : net_send(c->fd, buf, len);
}

int nc_alpn(netconn *c)
{
  if (!c)
    return ALPN_NONE;
  return c->tls ? tls_alpn(c->tls) : ALPN_NONE; /* a plain TCP conn has no ALPN */
}

const char *nc_verify_message(netconn *c) { return c && c->tls ? tls_verify_message(c->tls) : NULL; }

void nc_close(netconn *c)
{
  if (!c)
    return;
  if (c->tls)
    tls_close(c->tls);
  else if (c->fd >= 0)
    net_close(c->fd);
  free(c);
}
