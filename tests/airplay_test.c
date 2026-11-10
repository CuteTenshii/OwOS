/* Host test for the AirPlay sender (user/lib/airplay/airplay.c) against a scripted receiver: the netconn,
 * mDNS and thread calls are replaced by the fakes below, and airplay.c is included so its poller can be driven
 * by hand. */
#define OWOS_SUITE_TEST
#define rng_bytes airplay_test_rng_bytes
#define now_ms airplay_test_now_ms
#define msleep airplay_test_msleep
#define thread_stack_alloc airplay_test_stack_alloc
#define thread_stack_free airplay_test_stack_free
#define thread_create airplay_test_thread_create
#define thread_create_arg airplay_test_thread_create_arg
#define thread_join airplay_test_thread_join
#define nc_open airplay_test_nc_open
#define nc_close airplay_test_nc_close
#define nc_read airplay_test_nc_read
#define nc_write airplay_test_nc_write
#define mdns_browse airplay_test_mdns_browse
#include <stddef.h>
void msleep(unsigned ms);
void *thread_stack_alloc(int bytes);
void thread_stack_free(void *p);
int thread_create(void (*fn)(void), void *top);
int thread_create_arg(void (*fn)(void *), void *arg, void *top);
int thread_join(int tid);
void rng_bytes(void *buf, int len);
#include "../user/lib/uuid/uuid.c"
#include "../user/lib/airplay/airplay.c"
#include "test.h"
#include <stdio.h>

static char g_req[4096]; /* every request written, in order */
static int g_reqlen;
static char g_resp[4096];
static int g_resp_len, g_resp_pos;
static int g_server_status = 200;
static const char *g_info = "<plist><dict/></plist>";
static struct mdns_service g_svc[4];
static int g_nsvc;
static int g_fake;

void msleep(unsigned ms) { (void)ms; }
void *thread_stack_alloc(int bytes) { return malloc((size_t)bytes); }
void thread_stack_free(void *p) { free(p); }
int thread_create_arg(void (*fn)(void *), void *arg, void *top)
{
  (void)fn, (void)arg, (void)top;
  return 1;
}
int thread_create(void (*fn)(void), void *top)
{
  (void)fn, (void)top;
  return 1;
}
int thread_join(int tid)
{
  (void)tid;
  return 0;
}
void rng_bytes(void *buf, int len) { memset(buf, 0x5a, (size_t)len); }
long long now_ms(void) { return 0; }

int mdns_browse(const char *service, struct mdns_service *out, int max, int timeout_ms)
{
  (void)service, (void)timeout_ms;
  memcpy(out, g_svc, sizeof g_svc[0] * (size_t)(g_nsvc < max ? g_nsvc : max));
  return g_nsvc;
}

netconn *nc_open(const char *host, int port, int tls, int offer_h2)
{
  (void)host, (void)port, (void)tls, (void)offer_h2;
  return (netconn *)&g_fake;
}
void nc_close(netconn *c) { (void)c; }

static void respond(int status, const char *body)
{
  g_resp_len = snprintf(g_resp, sizeof g_resp, "HTTP/1.1 %d X\r\nContent-Length: %d\r\n\r\n%s", status, (int)strlen(body), body);
  g_resp_pos = 0;
}

int nc_write(netconn *c, const void *buf, int len)
{
  const char *b = buf;
  (void)c;
  memcpy(g_req + g_reqlen, b, (size_t)len);
  g_reqlen += len;
  g_req[g_reqlen] = '\0';
  if (!strncmp(b, "GET /server-info", 16))
    respond(g_server_status, g_info);
  else if (!strncmp(b, "GET /playback-info", 18))
    respond(200, g_info);
  else if (!strncmp(b, "POST ", 5))
    respond(200, "");
  return len;
}

int nc_read(netconn *c, void *buf, int max)
{
  int n = g_resp_len - g_resp_pos;
  (void)c;
  if (n <= 0)
    return 0;
  if (n > 7)
    n = 7; /* dribble, to exercise partial reads */
  if (n > max)
    n = max;
  memcpy(buf, g_resp + g_resp_pos, (size_t)n);
  g_resp_pos += n;
  return n;
}

static void reset(void)
{
  g_reqlen = 0;
  g_req[0] = '\0';
  g_server_status = 200;
  g_info = "<plist><dict/></plist>";
  g_resp_len = g_resp_pos = 0;
}

static struct airplay_device dev(void)
{
  struct airplay_device d;
  memset(&d, 0, sizeof d);
  d.ip[0] = 10;
  d.ip[3] = 7;
  d.port = 7000;
  return d;
}

static void test_connect_requires_server_info(void)
{
  struct airplay_device d = dev();
  airplay_session *s;
  reset();
  g_server_status = 403;
  expect(airplay_connect(&d, 1000) == 0);
  reset();
  s = airplay_connect(&d, 1000);
  expect(s != 0);
  expect(strstr(g_req, "GET /server-info HTTP/1.1") != 0);
  expect(strstr(g_req, "X-Apple-Session-ID: 5A5A5A5A-5A5A-4A5A-9A5A-5A5A5A5A5A5A\r\n") != 0);
  airplay_close(s);
}

static void test_load_posts_the_url(void)
{
  struct airplay_device d = dev();
  airplay_session *s;
  reset();
  s = airplay_connect(&d, 1000);
  g_reqlen = 0;
  g_req[0] = '\0';
  expect(airplay_load(s, "http://example.test/v.mp4", 0) == 0);
  expect(strstr(g_req, "POST /play HTTP/1.1") != 0);
  expect(strstr(g_req, "Content-Type: text/parameters") != 0);
  expect(strstr(g_req, "Content-Location: http://example.test/v.mp4\n") != 0);
  airplay_close(s);
}

static void test_transport_commands(void)
{
  struct airplay_device d = dev();
  airplay_session *s;
  reset();
  s = airplay_connect(&d, 1000);
  g_reqlen = 0;
  g_req[0] = '\0';
  expect(airplay_pause(s) == 0);
  expect(strstr(g_req, "POST /rate?value=0.000000 ") != 0);
  expect(airplay_play(s) == 0);
  expect(strstr(g_req, "POST /rate?value=1.000000 ") != 0);
  expect(airplay_seek(s, 30.5) == 0);
  expect(strstr(g_req, "POST /scrub?position=30.500 ") != 0);
  airplay_close(s);
}

static void test_status_follows_playback_info(void)
{
  struct airplay_device d = dev();
  struct airplay_status st;
  airplay_session *s;
  reset();
  s = airplay_connect(&d, 1000);
  s->loaded = 1;
  g_info = "<plist><dict><key>duration</key><real>60.25</real><key>position</key><real>12.5</real><key>rate</key><real>1.0</real></dict></plist>";
  poll_status(s);
  airplay_get_status(s, &st);
  expect_eq(st.state, AIRPLAY_PLAYING);
  expect_eq((int)(st.current_time * 1000), 12500);
  expect_eq((int)(st.duration * 1000), 60250);
  g_info = "<plist><dict><key>duration</key><real>60.25</real><key>position</key><real>13</real><key>rate</key><real>0.0</real></dict></plist>";
  poll_status(s);
  airplay_get_status(s, &st);
  expect_eq(st.state, AIRPLAY_PAUSED);
  g_info = "<plist><dict/></plist>"; /* the clip finished and the receiver forgot it */
  poll_status(s);
  airplay_get_status(s, &st);
  expect_eq(st.state, AIRPLAY_IDLE);
  airplay_close(s);
}

static void set_svc(int i, const char *inst, const char *txt, int txtlen)
{
  memset(&g_svc[i], 0, sizeof g_svc[i]);
  snprintf(g_svc[i].instance, sizeof g_svc[i].instance, "%s", inst);
  g_svc[i].ip[0] = 192;
  g_svc[i].ip[3] = (uint8_t)(10 + i);
  g_svc[i].port = 7000;
  memcpy(g_svc[i].txt, txt, (size_t)txtlen);
}

static void test_discovery_keeps_video_capable_receivers(void)
{
  struct airplay_device out[4];
  int n;
  memset(g_svc, 0, sizeof g_svc);
  set_svc(0, "Living Room", "deviceid=AA:BB\0features=0x5A7FFFF7,0x1E\0model=AppleTV3,1\0", sizeof("deviceid=AA:BB\0features=0x5A7FFFF7,0x1E\0model=AppleTV3,1\0") - 1);
  set_svc(1, "Speaker", "deviceid=CC:DD\0features=0x4A7FCFD6\0", sizeof("deviceid=CC:DD\0features=0x4A7FCFD6\0") - 1);
  g_nsvc = 2;
  n = airplay_discover(out, 4, 100);
  expect_eq(n, 1);
  expect_string(out[0].name, "Living Room");
  expect_string(out[0].id, "AA:BB");
  expect_string(out[0].model, "AppleTV3,1");
  expect_eq(out[0].ip[3], 10);
}

int main(void)
{
  test("connect needs a 200 from /server-info", test_connect_requires_server_info);
  test("load posts the media URL", test_load_posts_the_url);
  test("play, pause and seek", test_transport_commands);
  test("status follows /playback-info", test_status_follows_playback_info);
  test("discovery skips receivers without video", test_discovery_keeps_video_capable_receivers);
  return 0;
}
