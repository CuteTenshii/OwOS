/* netconn.c — TCP-or-TLS transport wrapper. See netconn.h. */
#include "netconn.h"
#include "owos.h"
#include "resolve.h"
#include "tls.h"

struct netconn {
  int fd;        /* SK_TCP socket, or -1 when this is a TLS conn */
  tls_conn *tls; /* user-space TLS conn, or NULL when plain TCP  */
};

static int g_nc_error;

int nc_last_error(void) { return g_nc_error; }

netconn *nc_open(const char *host, int port, int tls, int offer_h2)
{
  g_nc_error = NC_OPEN_CONNECT;
  netconn *c = (netconn *)malloc(sizeof *c);
  if (!c)
    return 0;
  c->fd = -1;
  c->tls = 0;
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

netconn *nc_from_fd(int fd)
{
  netconn *c = (netconn *)malloc(sizeof *c);
  if (!c)
    return 0;
  c->fd = fd;
  c->tls = 0;
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
