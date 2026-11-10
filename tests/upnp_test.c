/* Host test for the UPnP IGD message codec (user/lib/upnp/upnp.c). */
#define OWOS_SUITE_TEST
#include "test.h"
#include "upnp.h"
#include <stdio.h>
#include <string.h>

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != 0; }

static const char *DESC =
    "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
    "<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>"
    "<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
    "<controlURL>/ctl/L3F</controlURL></service></serviceList>"
    "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
    "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
    "<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL></service></serviceList>"
    "</device></deviceList></device></deviceList></device></root>";

static void test_msearch_names_the_target(void)
{
  char q[256];
  int n = upnp_msearch(q, sizeof q, "urn:x:1", 2);
  expect_eq(n, (int)strlen(q));
  expect(strncmp(q, "M-SEARCH * HTTP/1.1\r\n", 21) == 0);
  expect(has(q, "HOST: 239.255.255.250:1900\r\n") && has(q, "MAN: \"ssdp:discover\"\r\n"));
  expect(has(q, "ST: urn:x:1\r\n") && has(q, "MX: 2\r\n"));
  expect_eq(strlen(q) >= 4 && !strcmp(q + strlen(q) - 4, "\r\n\r\n"), 1);
  expect_eq(upnp_msearch(q, 20, "urn:x:1", 2), 0);
}

static void test_desc_picks_the_wan_service_and_resolves_its_url(void)
{
  struct upnp_igd g;
  expect(upnp_parse_desc(DESC, "http://192.168.1.1:5000/rootDesc.xml", &g));
  expect_string(g.ctl, "http://192.168.1.1:5000/ctl/IPConn");
  expect_string(g.svc, "urn:schemas-upnp-org:service:WANIPConnection:1");
}

static void test_desc_honours_urlbase_and_relative_control_url(void)
{
  struct upnp_igd g;
  const char *d = "<root><URLBase>http://10.0.0.1:49152/</URLBase><device><serviceList><service>"
                  "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
                  "<controlURL>upnp/control/ppp</controlURL></service></serviceList></device></root>";
  expect(upnp_parse_desc(d, "http://10.0.0.1:49152/gatedesc.xml", &g));
  expect_string(g.ctl, "http://10.0.0.1:49152/upnp/control/ppp");
}

static void test_desc_rejects_a_control_url_on_another_host(void)
{
  struct upnp_igd g;
  const char *d = "<root><device><serviceList><service>"
                  "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                  "<controlURL>http://192.168.1.99/ctl</controlURL></service></serviceList></device></root>";
  expect(!upnp_parse_desc(d, "http://192.168.1.1/d.xml", &g));
}

static void test_desc_without_a_wan_service_is_rejected(void)
{
  struct upnp_igd g;
  expect(!upnp_parse_desc("<root><device><serviceList><service><serviceType>urn:x</serviceType>"
                          "<controlURL>/c</controlURL></service></serviceList></device></root>",
                          "http://192.168.1.1/d.xml", &g));
}

static void test_soap_envelope_and_headers_agree(void)
{
  struct upnp_igd g = {"http://192.168.1.1:5000/ctl/IPConn", "urn:schemas-upnp-org:service:WANIPConnection:1"};
  char body[512], hdr[256];
  int bl = upnp_soap_body(body, sizeof body, &g, "AddPortMapping", "<NewExternalPort>80</NewExternalPort>");
  expect_eq(bl, (int)strlen(body));
  expect(has(body, "<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
                   "<NewExternalPort>80</NewExternalPort></u:AddPortMapping>"));
  int hl = upnp_soap_headers(hdr, sizeof hdr, &g, "AddPortMapping", bl);
  expect_eq(hl, (int)strlen(hdr));
  expect(has(hdr, "SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"\r\n"));
  char want[32];
  snprintf(want, sizeof want, "Content-Length: %d\r\n", bl);
  expect(has(hdr, want));
  expect_eq(upnp_soap_body(body, 40, &g, "AddPortMapping", ""), 0);
}

static void test_fault_code(void)
{
  const char *fault = "<s:Envelope><s:Body><s:Fault><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                      "<errorCode>718</errorCode><errorDescription>ConflictInMappingEntry</errorDescription>"
                      "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
  expect_eq(upnp_fault_code(fault), 718);
  expect_eq(upnp_fault_code("<u:Resp><NewExternalIPAddress>1.2.3.4</NewExternalIPAddress></u:Resp>"), 0);
}

int main(void)
{
  test("M-SEARCH names the target", test_msearch_names_the_target);
  test("description yields the WAN service and its absolute control URL",
       test_desc_picks_the_wan_service_and_resolves_its_url);
  test("URLBase and a relative controlURL resolve", test_desc_honours_urlbase_and_relative_control_url);
  test("a control URL on another host is rejected", test_desc_rejects_a_control_url_on_another_host);
  test("a description without a WAN service is rejected", test_desc_without_a_wan_service_is_rejected);
  test("SOAP envelope and headers agree", test_soap_envelope_and_headers_agree);
  test("fault code is read from a SOAP fault only", test_fault_code);
  return 0;
}
