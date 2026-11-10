/* uvc_stream_test.c: the UVC camera driver (drivers/usb/uvc.c) against a fake xHCI host: stream negotiation, endpoint
 * choice, frame mailbox and latching, idle shut-off, unplug. */
#define OWOS_SUITE_TEST
#include "../drivers/usb/usbhost.h"
#include "../drivers/usb/uvc.h"
#include "../drivers/usb/uvc_desc.h"
#include "test.h"
#include "uvc_cfg.h"
#include "uvc_host/timer.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- kernel services uvc.c expects --- */
static volatile uint32_t fake_ticks;
static long live_allocs;

uint32_t timer_ticks(void) { return fake_ticks; }
int uvc_host_yield(void)
{
  usleep(200);
  return 0;
}
void *uvc_host_alloc(size_t n)
{
  void *p = malloc(n);
  if (p)
    __sync_fetch_and_add(&live_allocs, 1);
  return p;
}
void uvc_host_free(void *p)
{
  if (p) {
    __sync_fetch_and_sub(&live_allocs, 1);
    free(p);
  }
}

/* --- fake host controller --- */
struct ctl {
  uint8_t type, req;
  uint16_t value, index;
  uint8_t data[64];
  int len;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct ctl clog[64];
static int nclog;
static uint8_t last_probe[48];
static int probe_len, fail_opens, bad_frame_reply;
static uint32_t reply_payload;

static int isoc_opens, bulk_opens, closes;
static uint8_t isoc_ep, isoc_burst, isoc_mult, isoc_interval, bulk_ep_open;
static uint16_t isoc_mps, bulk_mps_open;
static usbh_isoc_cb iso_cb;
static usbh_pipe_cb blk_cb;
static int pipe_token;

static void log_ctl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *d, int len)
{
  pthread_mutex_lock(&mu);
  if (nclog < 64) {
    struct ctl *c = &clog[nclog++];
    c->type = type;
    c->req = req;
    c->value = value;
    c->index = index;
    c->len = len;
    if (d)
      memcpy(c->data, d, (size_t)(len < 64 ? len : 64));
  }
  pthread_mutex_unlock(&mu);
}

int usbh_control_in(int slot, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len)
{
  (void)slot;
  log_ctl(type, req, value, index, NULL, len);
  memset(out, 0, (size_t)len);
  if (type == 0x80 && req == 6 && value == 0x0100) {
    out[15] = 2;
  } else if (type == 0x80 && req == 6 && value == 0x0302) {
    static const char name[] = "Test Cam";
    out[0] = 2 + 2 * 8;
    out[1] = 3;
    for (int i = 0; i < 8; i++)
      out[2 + 2 * i] = (uint8_t)name[i];
  } else if (type == 0xA1 && req == 0x81 && value == 0x0100) {
    memcpy(out, last_probe, (size_t)probe_len);
    out[22] = (uint8_t)reply_payload;
    out[23] = (uint8_t)(reply_payload >> 8);
    if (bad_frame_reply)
      out[3] ^= 0x7F;
  }
  return len;
}

int usbh_control_out(int slot, uint8_t type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *d, int len)
{
  (void)slot;
  log_ctl(type, req, value, index, d, len);
  if (type == 0x21 && req == 0x01 && value == 0x0100) {
    memcpy(last_probe, d, (size_t)len);
    probe_len = len;
  }
  return len;
}

int usbh_control_nodata(int slot, uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
  (void)slot;
  log_ctl(type, req, value, index, NULL, 0);
  return 0;
}

struct usbh_pipe *usbh_open_isoc_in(int slot, uint8_t ep, uint16_t mps, uint8_t burst, uint8_t mult, uint8_t binterval,
                                    usbh_isoc_cb cb, void *ctx)
{
  (void)slot;
  (void)ctx;
  pthread_mutex_lock(&mu);
  isoc_opens++;
  isoc_ep = ep;
  isoc_mps = mps;
  isoc_burst = burst;
  isoc_mult = mult;
  isoc_interval = binterval;
  iso_cb = cb;
  int fail = fail_opens > 0 && fail_opens--;
  pthread_mutex_unlock(&mu);
  return fail ? NULL : (struct usbh_pipe *)&pipe_token;
}

struct usbh_pipe *usbh_open_bulk_in(int slot, uint8_t ep, uint16_t mps, usbh_pipe_cb cb, void *ctx)
{
  (void)slot;
  (void)ctx;
  pthread_mutex_lock(&mu);
  bulk_opens++;
  bulk_ep_open = ep;
  bulk_mps_open = mps;
  blk_cb = cb;
  pthread_mutex_unlock(&mu);
  return (struct usbh_pipe *)&pipe_token;
}

void usbh_close_pipe(struct usbh_pipe *p)
{
  (void)p;
  pthread_mutex_lock(&mu);
  closes++;
  pthread_mutex_unlock(&mu);
}

/* --- harness --- */
static pthread_t poller;
static volatile int poll_stop;
static struct blob cfg;
static struct usbdev dev;

static void *poll_main(void *arg)
{
  (void)arg;
  while (!poll_stop) {
    uvc_tick();
    usleep(300);
  }
  return NULL;
}

static void reset_stubs(void)
{
  pthread_mutex_lock(&mu);
  nclog = isoc_opens = bulk_opens = closes = fail_opens = bad_frame_reply = probe_len = 0;
  reply_payload = 600;
  fake_ticks = 1000;
  pthread_mutex_unlock(&mu);
}

static void attach(void)
{
  reset_stubs();
  memset(&dev, 0, sizeof dev);
  dev.slot = 5;
  dev.vid = 0x046d;
  dev.pid = 0x0825;
  dev.cfg = cfg.b;
  dev.cfg_len = cfg.n;
  uvc_bind(&dev);
  poll_stop = 0;
  pthread_create(&poller, NULL, poll_main, NULL);
}

static void detach(void)
{
  poll_stop = 1;
  pthread_join(poller, NULL);
  uvc_unbind(5);
  expect_eq(live_allocs, 0);
}

static int count_ctl(uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
  int n = 0;
  pthread_mutex_lock(&mu);
  for (int i = 0; i < nclog; i++)
    n += clog[i].type == type && clog[i].req == req && clog[i].value == value && clog[i].index == index;
  pthread_mutex_unlock(&mu);
  return n;
}

static const struct ctl *nth_ctl(uint8_t type, uint8_t req, uint16_t value, int n)
{
  const struct ctl *hit = NULL;
  pthread_mutex_lock(&mu);
  for (int i = 0; i < nclog && !hit; i++)
    if (clog[i].type == type && clog[i].req == req && clog[i].value == value && n-- == 0)
      hit = &clog[i];
  pthread_mutex_unlock(&mu);
  return hit;
}

static int wait_for(volatile int *v, int want)
{
  for (int i = 0; i < 4000 && *v != want; i++)
    usleep(500);
  return *v == want;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void feed_jpeg(const uint8_t *body, int n, int fid)
{
  uint8_t pl[300];
  pl[0] = 2;
  pl[1] = (uint8_t)(0x80 | 0x02 | fid);
  memcpy(pl + 2, body, (size_t)n);
  iso_cb(NULL, pl, n + 2, 0);
}

/* --- tests --- */
static void bind_reports_the_camera(void)
{
  camera(&cfg);
  attach();
  expect(uvc_present());
  char t[512];
  int n = uvc_info_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "name Test Cam\nid 046d:0825\n"
                   "format MJPG 640x480 30 15\n"
                   "format MJPG 1280x720 30 25 24 20 15 10 5\n"
                   "format YUY2 640x480 30\n");
  n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "idle\n");
  expect_eq(dev.bound & USBH_BOUND_UVC, USBH_BOUND_UVC);
  detach();
  expect(!uvc_present());
}

static void other_devices_are_not_cameras(void)
{
  uint8_t hid[18] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 1, 1, 0};
  struct usbdev d = {.slot = 3, .cfg = hid, .cfg_len = sizeof hid};
  uvc_bind(&d);
  expect(!uvc_present());
  expect_eq(d.bound, 0);
  expect(uvc_command("start MJPG 640 480 30") != 0);
}

static void start_negotiates_and_opens_isoc(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);

  const struct ctl *probe = nth_ctl(0x21, 0x01, 0x0100, 0);
  expect(probe != NULL);
  expect_eq(probe->index, 1);
  expect_eq(probe->len, 34);
  expect_eq(probe->data[0], 1);
  expect_eq(probe->data[2], 1);
  expect_eq(probe->data[3], 1);
  expect_eq(rd32(probe->data + 4), 333333);
  expect_eq(count_ctl(0xA1, 0x81, 0x0100, 1), 1);
  const struct ctl *commit = nth_ctl(0x21, 0x01, 0x0200, 0);
  expect(commit != NULL);
  expect_eq(commit->len, 34);
  expect_eq(rd32(commit->data + 22), 600);

  expect_eq(count_ctl(0x01, 0x0B, 3, 1), 1);
  expect_eq(isoc_opens, 1);
  expect_eq(isoc_ep, 0x81);
  expect_eq(isoc_mps, 1023);
  expect_eq(isoc_burst, 0);
  expect_eq(isoc_mult, 0);
  expect_eq(isoc_interval, 1);

  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "streaming MJPG 640 480 30\n");
  detach();
  expect_eq(closes, 0);
}

static void smaller_payload_picks_smaller_alt(void)
{
  camera(&cfg);
  attach();
  reply_payload = 100;
  expect_eq(uvc_command("start YUY2 640 480 30"), 0);
  expect_eq(count_ctl(0x01, 0x0B, 1, 1), 1);
  expect_eq(isoc_mps, 128);
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "streaming YUY2 640 480 30\n");
  detach();
}

static void rejected_requests_change_nothing(void)
{
  camera(&cfg);
  attach();
  expect(uvc_command("start MJPG 641 480 30") != 0);
  expect(uvc_command("start MJPG 640 480 29") != 0);
  expect(uvc_command("start H264 640 480 30") != 0);
  expect(uvc_command("start MJPG 640 480") != 0);
  expect(uvc_command("start MJP 640 480 30") != 0);
  expect(uvc_command("start MJPG 64x 480 30") != 0);
  expect(uvc_command("pause") != 0);
  expect_eq(isoc_opens, 0);
  expect_eq(count_ctl(0x21, 0x01, 0x0100, 1), 0);

  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  expect(uvc_command("start MJPG 641 480 30") != 0);
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "streaming MJPG 640 480 30\n");
  expect_eq(closes, 0);
  detach();
}

static void negotiation_must_echo_the_request(void)
{
  camera(&cfg);
  attach();
  bad_frame_reply = 1;
  expect(uvc_command("start MJPG 640 480 30") != 0);
  expect_eq(isoc_opens, 0);
  expect_eq(count_ctl(0x21, 0x01, 0x0200, 1), 0);
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "idle\n");
  detach();
}

static void bandwidth_failure_tries_a_bigger_alt(void)
{
  camera(&cfg);
  attach();
  reply_payload = 400;
  fail_opens = 1;
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  expect_eq(isoc_opens, 2);
  expect_eq(isoc_mps, 1023);
  expect_eq(count_ctl(0x01, 0x0B, 2, 1), 1);
  expect_eq(count_ctl(0x01, 0x0B, 3, 1), 1);
  expect(count_ctl(0x01, 0x0B, 0, 1) >= 1);
  detach();
}

static void no_alt_fits_fails_cleanly(void)
{
  camera(&cfg);
  attach();
  fail_opens = 5;
  expect(uvc_command("start MJPG 640 480 30") != 0);
  expect_eq(isoc_opens, 1);
  expect(count_ctl(0x01, 0x0B, 0, 1) >= 1);
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "idle\n");
  detach();
}

static void frames_reach_the_reader_and_latch(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);

  uint8_t hdr[24], buf[64];
  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr), 0);
  expect_eq(rd32(hdr + 16), 0);
  expect_eq(uvc_frame_size(), 24);

  const uint8_t a[] = {0xFF, 0xD8, 'a', 'a', 'a', 0xFF, 0xD9};
  const uint8_t b[] = {0xFF, 0xD8, 'b', 'b', 0xFF, 0xD9};
  feed_jpeg(a, sizeof a, 0);
  expect_eq(uvc_frame_size(), 24 + sizeof a);
  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr), 1);
  expect_eq(rd32(hdr + 4), UVC_MJPG);
  expect_eq(rd32(hdr + 8), 640);
  expect_eq(rd32(hdr + 12), 480);
  expect_eq(rd32(hdr + 16), sizeof a);
  expect_eq(rd32(hdr + 20), 0);

  feed_jpeg(b, sizeof b, 1);
  expect_eq(uvc_frame_read(24, buf, sizeof buf), sizeof a);
  expect(!memcmp(buf, a, sizeof a));

  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr), 2);
  expect_eq(rd32(hdr + 16), sizeof b);
  expect_eq(uvc_frame_read(24, buf, sizeof buf), sizeof b);
  expect(!memcmp(buf, b, sizeof b));

  /* no new frame: the same one is served again */
  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr), 2);

  /* one read spanning header and payload, and a read starting inside the payload */
  uint8_t all[64];
  expect_eq(uvc_frame_read(0, all, sizeof all), 24 + sizeof b);
  expect(!memcmp(all + 24, b, sizeof b));
  expect_eq(uvc_frame_read(26, buf, 2), 2);
  expect_eq(buf[0], 'b');
  expect_eq(uvc_frame_read(24 + sizeof b, buf, 8), 0);
  detach();
}

static void large_frames_read_in_pieces(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  uint8_t pl[1002], big[5000];
  big[0] = 0xFF;
  big[1] = 0xD8;
  for (int i = 2; i < 5000; i++)
    big[i] = (uint8_t)(i * 7);
  for (int off = 0; off < 5000; off += 1000) {
    pl[0] = 2;
    pl[1] = (uint8_t)(0x80 | (off == 4000 ? 0x02 : 0));
    memcpy(pl + 2, big + off, 1000);
    iso_cb(NULL, pl, 1002, 0);
  }
  static uint8_t out[6000];
  expect_eq(uvc_frame_size(), 24 + 5000);
  expect_eq(uvc_frame_read(0, out, sizeof out), 24 + 5000);
  expect_eq(rd32(out + 16), 5000);
  expect(!memcmp(out + 24, big, 5000));
  detach();
}

static void missed_intervals_corrupt_the_frame(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  uint8_t pl[16] = {2, 0x80, 0xFF, 0xD8, 1, 2};
  iso_cb(NULL, pl, 6, 0);
  iso_cb(NULL, NULL, 0, 23);
  pl[1] = 0x82;
  iso_cb(NULL, pl, 6, 0);
  expect_eq(uvc_frame_size(), 24);
  pl[1] = 0x83;
  iso_cb(NULL, pl, 6, 0);
  expect_eq(uvc_frame_size(), 24 + 4);
  detach();
}

static void restart_and_stop(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  expect_eq(isoc_opens, 1);
  expect_eq(closes, 0);

  expect_eq(uvc_command("start YUY2 640 480 30"), 0);
  expect_eq(isoc_opens, 2);
  expect_eq(closes, 1);
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "streaming YUY2 640 480 30\n");

  expect_eq(uvc_command("stop"), 0);
  expect_eq(closes, 2);
  expect(count_ctl(0x01, 0x0B, 0, 1) >= 2);
  n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "idle\n");
  uint8_t hdr[24];
  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr), 0);
  expect_eq(rd32(hdr + 4), 0);
  expect_eq(rd32(hdr + 16), 0);
  expect_eq(uvc_command("stop"), 0);
  expect_eq(closes, 2);
  detach();
}

static void idle_stream_is_shut_off(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  uint8_t hdr[24];
  fake_ticks += 2 * TIMER_HZ;
  uvc_frame_read(0, hdr, 24);
  fake_ticks += 2 * TIMER_HZ;
  usleep(20000);
  expect_eq(closes, 0);
  fake_ticks += 2 * TIMER_HZ;
  expect(wait_for(&closes, 1));
  char t[64];
  int n = uvc_control_text(t, sizeof t);
  t[n] = 0;
  expect_string(t, "idle\n");
  detach();
}

static void unplug_while_streaming(void)
{
  camera(&cfg);
  attach();
  expect_eq(uvc_command("start MJPG 640 480 30"), 0);
  const uint8_t a[] = {0xFF, 0xD8, 'a', 'a', 0xFF, 0xD9};
  feed_jpeg(a, sizeof a, 0);
  poll_stop = 1;
  pthread_join(poller, NULL);
  uvc_unbind(5);
  expect(!uvc_present());
  expect_eq(live_allocs, 0);
  expect_eq(closes, 0);
  uint8_t hdr[24];
  expect_eq(uvc_frame_read(0, hdr, 24), 24);
  expect_eq(rd32(hdr + 16), 0);
  expect(uvc_command("start MJPG 640 480 30") != 0);
  expect_eq(uvc_frame_size(), 24);
}

static void bulk_camera(void)
{
  memset(&cfg, 0, sizeof cfg);
  uint8_t head[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&cfg, head, 9);
  iface(&cfg, 1, 0, 1, 2);
  format_mjpeg(&cfg, 1, 1);
  frame(&cfg, 7, 1, 320, 240, fps30, 1, 0, 0, 0);
  bulk_ep(&cfg, 0x83, 64);
  attach();
  reply_payload = 100;
  expect_eq(uvc_command("start MJPG 320 240 30"), 0);
  expect_eq(isoc_opens, 0);
  expect_eq(bulk_opens, 1);
  expect_eq(bulk_ep_open, 0x83);
  expect_eq(bulk_mps_open, 64);
  expect_eq(count_ctl(0x01, 0x0B, 1, 1), 0);

  /* a 100 byte payload: 64 byte packet then a 36 byte one; the header says EOF */
  uint8_t pl[100] = {2, 0x82, 0xFF, 0xD8};
  for (int i = 4; i < 100; i++)
    pl[i] = (uint8_t)i;
  blk_cb(NULL, pl, 64);
  expect_eq(uvc_frame_size(), 24);
  blk_cb(NULL, pl + 64, 36);
  expect_eq(uvc_frame_size(), 24 + 98);
  uint8_t out[130];
  expect_eq(uvc_frame_read(0, out, sizeof out), 24 + 98);
  expect(!memcmp(out + 24, pl + 2, 98));

  expect_eq(uvc_command("stop"), 0);
  expect_eq(closes, 1);
  expect_eq(count_ctl(0x01, 0x0B, 0, 1), 0);
  detach();
}

int main(void)
{
  test("bind reports name, id and formats", bind_reports_the_camera);
  test("a non-camera device is not bound", other_devices_are_not_cameras);
  test("start negotiates probe/commit and opens the isochronous endpoint", start_negotiates_and_opens_isoc);
  test("the smallest alternate setting that fits is used", smaller_payload_picks_smaller_alt);
  test("rejected requests leave the stream alone", rejected_requests_change_nothing);
  test("a device that changes the format fails the start", negotiation_must_echo_the_request);
  test("bandwidth failure falls back to a bigger alternate setting", bandwidth_failure_tries_a_bigger_alt);
  test("no usable alternate setting fails cleanly", no_alt_fits_fails_cleanly);
  test("frames reach the reader and latch", frames_reach_the_reader_and_latch);
  test("large frames read in pieces", large_frames_read_in_pieces);
  test("a missed interval corrupts only its frame", missed_intervals_corrupt_the_frame);
  test("restart, stop and idle header", restart_and_stop);
  test("a stream nobody reads is shut off", idle_stream_is_shut_off);
  test("unplug while streaming", unplug_while_streaming);
  test("bulk camera", bulk_camera);
  return 0;
}
