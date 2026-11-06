/* Host unit test for HTTP Client Hints (user/apps/browser/features/clienthints/clienthints.c).
 *
 *   cc -std=gnu11 -Iscripts -Iuser/lib -Iuser/lib/curl -Iuser/apps/browser \
 *      tests/clienthints_test.c user/lib/curl/url.c user/lib/mutex.c -lpthread \
 *      -o /tmp/clienthints_test && /tmp/clienthints_test
 */
#define OWOS_SUITE_TEST
#include "test.h"
#include <string.h>

static int g_vw = 1024, g_vh = 600;
void browser_viewport_px(int *w, int *h)
{
  *w = g_vw;
  *h = g_vh;
}
#include "../user/apps/browser/features/clienthints/clienthints.c"

#define PAGE "https://page.example"

static char hdrs[2048];

static bool has(const char *line)
{
  for (const char *p = hdrs; *p; p = strstr(p, "\r\n") + 2) {
    size_t n = strlen(line);
    if (!strncmp(p, line, n) && p[n] == '\r')
      return true;
  }
  return false;
}

static bool has_name(const char *name)
{
  size_t n = strlen(name);
  for (const char *p = hdrs; *p; p = strstr(p, "\r\n") + 2)
    if (!strncasecmp(p, name, n) && p[n] == ':')
      return true;
  return false;
}

static void hints_for(const char *page, const char *policy, const char *url)
{
  ch_request_headers_for(page, policy, url, hdrs, sizeof hdrs);
}

int main(void)
{
  test("low-entropy hints without opt-in");
  hints_for(0, 0, "https://fresh1.example/");
  expect(has("Sec-CH-UA: \"OwOS-Browser\";v=\"1\", \"Not.A/Brand\";v=\"99\""));
  expect(has("Sec-CH-UA-Mobile: ?0"));
  expect(has("Sec-CH-UA-Platform: \"OwOS\""));
  expect(!has_name("Sec-CH-UA-Arch"));
  expect(!has_name("Sec-CH-Viewport-Width"));
  expect(!has_name("DPR"));

  test("secure contexts only");
  hints_for(0, 0, "http://fresh1.example/");
  expect(hdrs[0] == '\0');
  hints_for(0, 0, "http://localhost:8080/");
  expect(has_name("Sec-CH-UA"));
  hints_for(0, 0, "http://127.0.0.1/");
  expect(has_name("Sec-CH-UA-Mobile"));
  expect(ch_secure_url("https://a.example/") && ch_secure_url("http://foo.localhost/"));
  expect(!ch_secure_url("http://a.example/") && !ch_secure_url(0));

  test("high-entropy hints after Accept-CH");
  ch_accept("opt.example", "Sec-CH-UA-Arch, sec-ch-ua-bitness;x=1, SEC-CH-UA-Model, bogus-token");
  hints_for(0, 0, "https://opt.example/x");
  expect(has("Sec-CH-UA-Arch: \"x86\""));
  expect(has("Sec-CH-UA-Bitness: \"" CH_BITNESS "\""));
  expect(has("Sec-CH-UA-Model: \"\""));
  expect(!has_name("Sec-CH-UA-Platform-Version"));
  expect(!has_name("bogus-token"));
  hints_for(0, 0, "https://other.example/x");
  expect(!has_name("Sec-CH-UA-Arch"));
  ch_accept("", "sec-ch-ua-arch");
  ch_accept(0, "sec-ch-ua-arch");
  ch_accept("novalue.example", 0);
  ch_accept("allbogus.example", "nope, nada");
  hints_for(0, 0, "https://allbogus.example/");
  expect(!has_name("Sec-CH-UA-Arch"));

  test("full hint set and value formats");
  ch_accept("full.example",
            "Sec-CH-UA-Platform-Version, Sec-CH-UA-Full-Version-List, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors, "
            "Sec-CH-Prefers-Color-Scheme, Sec-CH-Prefers-Reduced-Motion, Sec-CH-Prefers-Reduced-Transparency, "
            "Sec-CH-Viewport-Width, Sec-CH-Viewport-Height, Sec-CH-DPR, Sec-CH-Width, Sec-CH-Device-Memory, "
            "DPR, Viewport-Width, Device-Memory, Save-Data, ECT, RTT, Downlink");
  hints_for(0, 0, "https://full.example/");
  expect(has("Sec-CH-UA-Platform-Version: \"1.0.0\""));
  expect(has("Sec-CH-UA-Full-Version-List: \"OwOS-Browser\";v=\"1.0.0.0\", \"Not.A/Brand\";v=\"99.0.0.0\""));
  expect(has("Sec-CH-UA-WoW64: ?0"));
  expect(has("Sec-CH-UA-Form-Factors: \"Desktop\""));
  expect(has("Sec-CH-Prefers-Color-Scheme: \"light\""));
  expect(has("Sec-CH-Prefers-Reduced-Motion: \"no-preference\""));
  expect(has("Sec-CH-Prefers-Reduced-Transparency: \"no-preference\""));
  expect(has("Sec-CH-Viewport-Width: 1024"));
  expect(has("Sec-CH-Viewport-Height: 600"));
  expect(has("Viewport-Width: 1024"));
  expect(has("Sec-CH-DPR: 1") && has("DPR: 1"));
  expect(has("Sec-CH-Device-Memory: 2") && has("Device-Memory: 2"));
  expect(has("ECT: 4g") && has("RTT: 50") && has("Downlink: 10"));
  expect(!has_name("Sec-CH-Width"));
  expect(!has_name("Save-Data"));
  g_vw = 800;
  g_vh = 480;
  hints_for(0, 0, "https://full.example/");
  expect(has("Sec-CH-Viewport-Width: 800") && has("Sec-CH-Viewport-Height: 480"));

  test("legacy ch_request_headers keeps host keying");
  ch_request_headers("full.example", hdrs, sizeof hdrs);
  expect(has("Sec-CH-Viewport-Width: 800") && has_name("Sec-CH-UA"));
  ch_request_headers("never.example", hdrs, sizeof hdrs);
  expect(has_name("Sec-CH-UA-Mobile") && !has_name("Sec-CH-DPR"));
  expect(ch_request_headers("full.example", hdrs, 3) == 0);

  test("truncation never splits a line");
  int n = ch_request_headers("full.example", hdrs, 80);
  expect(n > 0 && n < 80 && !strcmp(hdrs + n - 2, "\r\n") && (int)strlen(hdrs) == n);

  test("Permissions-Policy delegation");
  ch_accept("page.example", "Sec-CH-UA-Arch, Sec-CH-DPR, Sec-CH-Device-Memory");
  hints_for(PAGE, 0, "https://page.example/img.png");
  expect(has_name("Sec-CH-UA-Arch") && has_name("Sec-CH-DPR"));
  hints_for(PAGE, 0, "https://cdn.example/img.png");
  expect(has_name("Sec-CH-UA") && has_name("Sec-CH-UA-Platform"));
  expect(!has_name("Sec-CH-UA-Arch") && !has_name("Sec-CH-DPR"));
  hints_for(PAGE, "ch-dpr=(self \"https://cdn.example\")", "https://cdn.example/img.png");
  expect(has_name("Sec-CH-DPR") && !has_name("Sec-CH-Device-Memory") && !has_name("DPR"));
  hints_for(PAGE, "ch-dpr=(self \"https://cdn.example\")", "https://evil.example/img.png");
  expect(!has_name("Sec-CH-DPR"));
  hints_for(PAGE, "ch-ua-arch=*, ch-device-memory=(\"https://cdn.example\")", "https://evil.example/");
  expect(has_name("Sec-CH-UA-Arch") && !has_name("Sec-CH-Device-Memory"));
  hints_for(PAGE, "ch-device-memory=(\"https://cdn.example\")", "https://cdn.example/");
  expect(has("Sec-CH-Device-Memory: 2") && !has_name("Device-Memory"));
  hints_for(PAGE, "ch-ua=()", "https://cdn.example/");
  expect(!has_name("Sec-CH-UA") && has_name("Sec-CH-UA-Mobile"));
  hints_for(PAGE, "ch-ua-arch=(\"https://cdn.example:8443\")", "https://cdn.example/");
  expect(!has_name("Sec-CH-UA-Arch"));
  hints_for(PAGE, "geolocation=(), CH-DPR=*", "https://cdn.example/");
  expect(has_name("Sec-CH-DPR"));
  hints_for(PAGE, "ch-dpr=(), ch-dpr=*", "https://cdn.example/");
  expect(has_name("Sec-CH-DPR"));
  hints_for(PAGE, "ch-dpr=*", "http://cdn.example/");
  expect(hdrs[0] == '\0');

  test("Accept-CH is keyed on the page origin, not the sub-resource host");
  hints_for("https://page.example", "ch-ua-arch=*", "https://nooptin.example/");
  expect(has_name("Sec-CH-UA-Arch"));
  hints_for("https://nooptin.example", "ch-ua-arch=*", "https://page.example/");
  expect(!has_name("Sec-CH-UA-Arch"));

  test("Critical-CH retry");
  char sent[1024], now[1024];
  ch_request_headers_for(0, 0, "https://crit.example/", sent, sizeof sent);
  ch_accept("crit.example", "Sec-CH-UA-Arch, Sec-CH-Viewport-Width, Sec-CH-Width");
  ch_request_headers_for(0, 0, "https://crit.example/", now, sizeof now);
  expect(ch_critical_retry("Sec-CH-UA-Arch", sent, now));
  expect(ch_critical_retry("sec-ch-ua-arch", sent, now));
  expect(ch_critical_retry("Sec-CH-Viewport-Width, bogus", sent, now));
  expect(!ch_critical_retry("Sec-CH-UA", sent, now));
  expect(!ch_critical_retry("Sec-CH-UA-Model", sent, now));
  expect(!ch_critical_retry("Sec-CH-Width", sent, now));
  expect(!ch_critical_retry("Sec-CH-UA-Arch", now, now));
  expect(!ch_critical_retry(0, sent, now) && !ch_critical_retry("", sent, now));
  expect(ch_critical_retry("Sec-CH-UA-Arch", 0, now));

  test("headers and userAgentData share one source");
  ch_accept("src.example", "Sec-CH-UA-Platform-Version, Sec-CH-UA-Model");
  hints_for(0, 0, "https://src.example/");
  char want[256];
  snprintf(want, sizeof want, "Sec-CH-UA-Platform: \"%s\"", ch_ua.platform);
  expect(has(want));
  snprintf(want, sizeof want, "Sec-CH-UA-Platform-Version: \"%s\"", ch_ua.platform_version);
  expect(has(want));
  snprintf(want, sizeof want, "Sec-CH-UA: \"%s\";v=\"%s\", \"%s\";v=\"%s\"", ch_ua.brands[0].brand,
           ch_ua.brands[0].version, ch_ua.brands[1].brand, ch_ua.brands[1].version);
  expect(has(want));
  return 0;
}
