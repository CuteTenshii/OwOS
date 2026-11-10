/* igd.c: UPnP IGD I/O: SSDP discovery over UDP and SOAP calls over HTTP; see upnp.h. */

#include "headers.h"
#include "http.h"
#include "owos.h"
#include "upnp.h"
#include "url.h"
#include "xml.h"

#define SSDP_PORT 1900
#define REPLY_MAX 4096
#define DESC_MAX 16384

static int last_fault;

struct sink {
  char *p;
  int len, cap;
};

static int sink_fn(void *ctx, const unsigned char *buf, int len)
{
  struct sink *s = ctx;
  for (int i = 0; i < len && s->len + 1 < s->cap; i++)
    s->p[s->len++] = (char)buf[i];
  s->p[s->len] = 0;
  return 0;
}

int upnp_last_fault(void) { return last_fault; }

int upnp_local_ip(char out[16])
{
  char b[32];
  int n = readfile("/sys/class/net/eth0/ipaddr", b, (int)sizeof b - 1);
  if (n <= 0)
    return 0;
  b[n] = 0;
  while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r'))
    b[--n] = 0;
  if (n == 0 || n >= 16 || streq(b, "0.0.0.0"))
    return 0;
  strcpy(out, b);
  return 1;
}

/* Fetch the description a search reply points at and pick the WAN service out of it. */
static int probe(const char *reply, const unsigned char src[16], struct upnp_igd *g)
{
  static char desc[DESC_MAX];
  struct http_headers h;
  struct purl u;
  char from[16];
  int found = 0;

  http_headers_parse(&h, reply);
  const char *loc = http_headers_get(&h, "location");
  snprintf(from, sizeof from, "%u.%u.%u.%u", src[12], src[13], src[14], src[15]);
  if (h.status == 200 && loc && parse_url(loc, &u) && !u.tls && strcmp(u.host, from) == 0) {
    struct sink s = {desc, 0, sizeof desc};
    desc[0] = 0;
    if (http_get(loc, sink_fn, &s) == 200)
      found = upnp_parse_desc(desc, loc, g);
  }
  http_headers_free(&h);
  return found;
}

int upnp_discover(struct upnp_igd *g)
{
  static const char *const targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                        "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
  unsigned char group[16] = {0};
  group[10] = group[11] = 0xff;
  group[12] = 239, group[13] = 255, group[14] = 255, group[15] = 250;

  int fd = -1;
  for (int i = 0; fd < 0 && i < 8; i++)
    fd = net_bind(49152 + (int)((msec() + (unsigned)i * 7) % 16000));
  if (fd < 0)
    return 0;
  net_set_ttl(fd, 2);

  int rc = 0;
  for (int round = 0; round < 3 && rc == 0; round++) {
    for (unsigned t = 0; t < sizeof targets / sizeof targets[0]; t++) {
      char q[256];
      int n = upnp_msearch(q, (int)sizeof q, targets[t], 2);
      if (n)
        net_sendto(fd, group, SSDP_PORT, q, n);
    }
    for (;;) {
      char reply[REPLY_MAX];
      unsigned char src[16];
      int port;
      int n = net_recvfrom(fd, reply, (int)sizeof reply - 1, src, &port);
      if (n == NET_EINTR) {
        rc = UPNP_EINTR;
        break;
      }
      if (n <= 0)
        break;
      reply[n] = 0;
      if (probe(reply, src, g)) {
        rc = 1;
        break;
      }
    }
  }
  net_close(fd);
  return rc;
}

/* Run a SOAP action; the reply body lands in resp. */
static int soap(const struct upnp_igd *g, const char *action, const char *args, char *resp, int cap)
{
  char body[1536], hdrs[256];
  int bl = upnp_soap_body(body, (int)sizeof body, g, action, args);
  if (!bl || !upnp_soap_headers(hdrs, (int)sizeof hdrs, g, action, bl))
    return UPNP_EFAIL;

  struct sink s = {resp, 0, cap};
  resp[0] = 0;
  int status = http_post(g->ctl, hdrs, body, bl, 0, 0, 0, sink_fn, &s);
  if (status == HTTP_EINTR)
    return UPNP_EINTR;
  if (status == 200)
    return 0;
  if (status == 500) {
    last_fault = upnp_fault_code(resp);
    return UPNP_EFAULT;
  }
  return UPNP_EFAIL;
}

int upnp_external_ip(const struct upnp_igd *g, char *out, int cap)
{
  char resp[2048];
  int rc = soap(g, "GetExternalIPAddress", "", resp, (int)sizeof resp);
  if (rc)
    return rc;
  return xml_text(xml_all(resp), "NewExternalIPAddress", out, cap) > 0 ? 0 : UPNP_EFAIL;
}

int upnp_add_mapping(const struct upnp_igd *g, const char *proto, int ext_port, int int_port, const char *int_ip,
                     const char *desc, unsigned lease)
{
  if (!streq(proto, "TCP") && !streq(proto, "UDP"))
    return UPNP_EFAIL;
  char args[1024], ip[64], d[UPNP_STR_MAX * 6];
  if (xml_escape(ip, 0, sizeof ip, int_ip) < 0 || xml_escape(d, 0, sizeof d, desc) < 0)
    return UPNP_EFAIL;
  int n = snprintf(args, sizeof args,
                   "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>%s</NewProtocol>"
                   "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
                   "<NewEnabled>1</NewEnabled><NewPortMappingDescription>%s</NewPortMappingDescription>"
                   "<NewLeaseDuration>%u</NewLeaseDuration>",
                   ext_port, proto, int_port, ip, d, lease);
  if (n >= (int)sizeof args)
    return UPNP_EFAIL;
  char resp[2048];
  return soap(g, "AddPortMapping", args, resp, (int)sizeof resp);
}

int upnp_del_mapping(const struct upnp_igd *g, const char *proto, int ext_port)
{
  if (!streq(proto, "TCP") && !streq(proto, "UDP"))
    return UPNP_EFAIL;
  char args[256], resp[2048];
  snprintf(args, sizeof args,
           "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>%s</NewProtocol>",
           ext_port, proto);
  return soap(g, "DeletePortMapping", args, resp, (int)sizeof resp);
}

int upnp_get_mapping(const struct upnp_igd *g, int index, struct upnp_map *m)
{
  char args[96], resp[4096], v[UPNP_STR_MAX];
  snprintf(args, sizeof args, "<NewPortMappingIndex>%d</NewPortMappingIndex>", index);
  int rc = soap(g, "GetGenericPortMappingEntry", args, resp, (int)sizeof resp);
  if (rc)
    return rc;

  struct xml_span doc = xml_all(resp);
  memset(m, 0, sizeof *m);
  if (xml_text(doc, "NewProtocol", m->proto, sizeof m->proto) < 0 || xml_text(doc, "NewExternalPort", v, sizeof v) < 0)
    return UPNP_EFAIL;
  m->ext_port = (int)http_parse_uint(v, 0);
  if (xml_text(doc, "NewInternalPort", v, sizeof v) >= 0)
    m->int_port = (int)http_parse_uint(v, 0);
  xml_text(doc, "NewInternalClient", m->int_ip, sizeof m->int_ip);
  if (xml_text(doc, "NewEnabled", v, sizeof v) >= 0)
    m->enabled = http_parse_uint(v, 0) != 0;
  xml_text(doc, "NewPortMappingDescription", m->desc, sizeof m->desc);
  if (xml_text(doc, "NewLeaseDuration", v, sizeof v) >= 0)
    m->lease = http_parse_uint(v, 0);
  return 0;
}
