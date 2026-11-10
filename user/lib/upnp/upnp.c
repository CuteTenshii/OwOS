/* upnp.c: UPnP IGD message codec: SSDP request, device description and SOAP envelopes. Pure computation, no
 * I/O; see upnp.h. */

#include "upnp.h"
#include "ascii.h"
#include "headers.h"
#include "owos.h"
#include "url.h"
#include "xml.h"

int upnp_msearch(char *buf, int cap, const char *st, int mx)
{
  int n = snprintf(
      buf, (unsigned)cap,
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: %d\r\nST: %s\r\n\r\n", mx,
      st);
  return n < cap ? n : 0;
}

static int is_wan_service(const char *type)
{
  static const char *const pre[] = {"urn:schemas-upnp-org:service:WANIPConnection:",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:"};
  for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; i++)
    if (starts_ci(type, pre[i]))
      return 1;
  return 0;
}

int upnp_parse_desc(const char *xml, const char *loc, struct upnp_igd *g)
{
  struct xml_span doc = xml_all(xml), svc;
  char base[UPNP_URL_MAX], type[UPNP_SVC_MAX], ref[UPNP_URL_MAX], ctl[UPNP_URL_MAX];
  struct purl lu, cu;

  strlcpy(base, loc, sizeof base);
  if (xml_text(doc, "URLBase", ref, sizeof ref) > 0)
    strlcpy(base, ref, sizeof base);
  if (!parse_url(loc, &lu))
    return 0;

  while (xml_next(&doc, "service", &svc)) {
    if (xml_text(svc, "serviceType", type, sizeof type) < 0 || !is_wan_service(type))
      continue;
    if (xml_text(svc, "controlURL", ref, sizeof ref) <= 0)
      continue;
    resolve_url(base, ref, ctl, sizeof ctl);
    if (!parse_url(ctl, &cu) || cu.tls || strcmp(cu.host, lu.host) != 0)
      continue;
    strlcpy(g->ctl, ctl, sizeof g->ctl);
    strlcpy(g->svc, type, sizeof g->svc);
    return 1;
  }
  return 0;
}

int upnp_soap_body(char *buf, int cap, const struct upnp_igd *g, const char *action, const char *args)
{
  int n = snprintf(buf, (unsigned)cap,
                   "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                   "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                   "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
                   action, g->svc, args, action);
  return n < cap ? n : 0;
}

int upnp_soap_headers(char *buf, int cap, const struct upnp_igd *g, const char *action, int len)
{
  int n = snprintf(buf, (unsigned)cap,
                   "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\n",
                   g->svc, action, len);
  return n < cap ? n : 0;
}

int upnp_fault_code(const char *body)
{
  char v[16];
  if (xml_text(xml_all(body), "errorCode", v, sizeof v) <= 0)
    return 0;
  return (int)http_parse_uint(v, 0);
}
