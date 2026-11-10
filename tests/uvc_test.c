/* uvc_test.c: USB Video Class descriptor parsing, stream negotiation records and payload reassembly
 * (drivers/usb/uvc_desc.c). */
#define OWOS_SUITE_TEST
#include "../drivers/usb/uvc_desc.h"
#include "test.h"
#include "uvc_cfg.h"
#include <string.h>

static void parse_formats(void)
{
  struct blob o;
  struct uvc_cfg c;
  camera(&o);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.vs_iface, 1);
  expect_eq(c.bcd, 0x0110);
  expect_eq(c.nformats, 2);
  expect_eq(c.formats[0].fourcc, UVC_MJPG);
  expect_eq(c.formats[0].index, 1);
  expect_eq(c.formats[0].nframes, 2);
  expect_eq(c.formats[0].frames[0].w, 640);
  expect_eq(c.formats[0].frames[0].h, 480);
  expect_eq(c.formats[0].frames[0].nival, 2);
  expect_eq(c.formats[0].frames[0].ival[1], 666666);
  expect_eq(c.formats[0].frames[1].nival, 0);
  expect_eq(c.formats[0].frames[1].ival_max, 2000000);
  expect_eq(c.formats[1].fourcc, UVC_YUY2);
  expect_eq(c.formats[1].frames[0].max_bytes, 640 * 480 * 2);
  expect_eq(c.bulk_ep, 0);
}

static void parse_alts(void)
{
  struct blob o;
  struct uvc_cfg c;
  camera(&o);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.nalts, 3);
  expect_eq(c.alts[0].alt, 1);
  expect_eq(c.alts[0].ep, 0x81);
  expect_eq(c.alts[0].mps, 128);
  expect_eq(c.alts[0].payload, 128);
  expect_eq(c.alts[1].mps, 512);
  expect_eq(c.alts[1].burst, 0);
  expect_eq(c.alts[1].payload, 512);
  expect_eq(c.alts[2].payload, 1023);
}

static void high_bandwidth_endpoint(void)
{
  struct blob o;
  struct uvc_cfg c;
  camera(&o);
  o.b[o.n - 3] = 0x00;
  o.b[o.n - 2] = 0x00;
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.nalts, 2);
  camera(&o);
  o.b[o.n - 3] = 0x00;
  o.b[o.n - 2] = 0x0C;
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.alts[2].mps, 1024);
  expect_eq(c.alts[2].burst, 1);
  expect_eq(c.alts[2].payload, 2048);
}

static void super_speed_companion(void)
{
  struct blob o;
  struct uvc_cfg c;
  memset(&o, 0, sizeof o);
  uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&o, cfg, 9);
  iface(&o, 3, 0, 0, 2);
  format_mjpeg(&o, 1, 1);
  frame(&o, 7, 1, 1920, 1080, fps30, 1, 0, 0, 4000000);
  iface(&o, 3, 1, 1, 2);
  iso_ep(&o, 0x82, 1024, 1);
  ss_companion(&o, 2, 1, 6144);
  iface(&o, 3, 2, 1, 2);
  iso_ep(&o, 0x82, 1024, 1);
  ss_companion(&o, 2, 0, 3072);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.vs_iface, 3);
  expect_eq(c.nalts, 1);
  expect_eq(c.alts[0].alt, 2);
  expect_eq(c.alts[0].burst, 2);
  expect_eq(c.alts[0].mult, 0);
  expect_eq(c.alts[0].payload, 3072);
  expect_eq(c.formats[0].frames[0].max_bytes, UVC_MAX_FRAME_BYTES);
}

static void bulk_streaming(void)
{
  struct blob o;
  struct uvc_cfg c;
  memset(&o, 0, sizeof o);
  uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&o, cfg, 9);
  iface(&o, 1, 0, 1, 2);
  format_mjpeg(&o, 1, 1);
  frame(&o, 7, 1, 320, 240, fps30, 1, 0, 0, 0);
  bulk_ep(&o, 0x83, 512);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.nalts, 0);
  expect_eq(c.bulk_ep, 0x83);
  expect_eq(c.bulk_mps, 512);
  expect_eq(c.bcd, 0x0100);
  expect_eq(c.formats[0].frames[0].max_bytes, UVC_MAX_FRAME_BYTES);
}

static void unusable_cameras(void)
{
  struct blob o;
  struct uvc_cfg c;
  memset(&o, 0, sizeof o);
  uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&o, cfg, 9);
  iface(&o, 1, 0, 0, 2);
  format_uncompressed(&o, 1, 1, "H264");
  frame(&o, 5, 1, 640, 480, fps30, 1, 0, 0, 0);
  iface(&o, 1, 1, 1, 2);
  iso_ep(&o, 0x81, 512, 1);
  expect(uvc_parse_config(o.b, o.n, &c) != 0);

  camera(&o);
  o.n = 9 + 9 + 13;
  expect(uvc_parse_config(o.b, o.n, &c) != 0);

  uint8_t hid[18] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 1, 1, 0};
  expect(uvc_parse_config(hid, sizeof hid, &c) != 0);
  expect(uvc_parse_config(o.b, 5, &c) != 0);
}

static void oversized_uncompressed_dropped(void)
{
  struct blob o;
  struct uvc_cfg c;
  memset(&o, 0, sizeof o);
  uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&o, cfg, 9);
  iface(&o, 1, 0, 0, 2);
  format_uncompressed(&o, 1, 2, "YUY2");
  frame(&o, 5, 1, 1920, 1080, fps30, 1, 0, 0, 0);
  frame(&o, 5, 2, 320, 240, fps30, 1, 0, 0, 0);
  iface(&o, 1, 1, 1, 2);
  iso_ep(&o, 0x81, 512, 1);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.formats[0].nframes, 1);
  expect_eq(c.formats[0].frames[0].w, 320);
}

static void nv12_format(void)
{
  struct blob o;
  struct uvc_cfg c;
  memset(&o, 0, sizeof o);
  uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
  put(&o, cfg, 9);
  iface(&o, 1, 0, 0, 2);
  format_uncompressed(&o, 4, 1, "NV12");
  frame(&o, 5, 1, 640, 480, fps30, 1, 0, 0, 0);
  iface(&o, 1, 1, 1, 2);
  iso_ep(&o, 0x81, 512, 1);
  expect_eq(uvc_parse_config(o.b, o.n, &c), 0);
  expect_eq(c.formats[0].fourcc, UVC_NV12);
  expect_eq(c.formats[0].frames[0].max_bytes, 640 * 480 * 3 / 2);
}

static void fps_selection(void)
{
  struct blob o;
  struct uvc_cfg c;
  const struct uvc_frame *fr = NULL;
  int fps[16];
  camera(&o);
  uvc_parse_config(o.b, o.n, &c);

  expect(uvc_find_format(&c, UVC_MJPG, 640, 480, &fr) == &c.formats[0]);
  expect_eq(uvc_fps_list(fr, fps, 16), 2);
  expect_eq(fps[0], 30);
  expect_eq(fps[1], 15);
  expect_eq(uvc_interval_for_fps(fr, 30), 333333);
  expect_eq(uvc_interval_for_fps(fr, 15), 666666);
  expect_eq(uvc_interval_for_fps(fr, 24), 0);
  expect_eq(uvc_interval_for_fps(fr, 0), 0);

  expect(uvc_find_format(&c, UVC_MJPG, 1280, 720, &fr) == &c.formats[0]);
  int n = uvc_fps_list(fr, fps, 16);
  expect_eq(n, 7);
  expect_eq(fps[0], 30);
  expect_eq(fps[n - 1], 5);
  expect_eq(uvc_interval_for_fps(fr, 25), 400000);
  expect_eq(uvc_interval_for_fps(fr, 60), 0);

  expect(!uvc_find_format(&c, UVC_YUY2, 1280, 720, &fr));
  expect(!uvc_find_format(&c, UVC_NV12, 640, 480, &fr));
  expect(uvc_find_format(&c, UVC_YUY2, 640, 480, &fr) == &c.formats[1]);
}

static void alt_selection(void)
{
  struct blob o;
  struct uvc_cfg c;
  camera(&o);
  uvc_parse_config(o.b, o.n, &c);
  expect_eq(uvc_pick_alt(&c, 100)->alt, 1);
  expect_eq(uvc_pick_alt(&c, 129)->alt, 2);
  expect_eq(uvc_pick_alt(&c, 600)->alt, 3);
  expect_eq(uvc_pick_alt(&c, 5000)->alt, 3);
  c.nalts = 0;
  expect(!uvc_pick_alt(&c, 100));
}

static void probe_layout(void)
{
  struct uvc_probe p = {1, 2, 3, 333333, 614400, 1024}, q;
  uint8_t b[48];
  memset(b, 0xEE, sizeof b);
  uvc_probe_pack(&p, b, 34);
  expect_eq(b[0], 1);
  expect_eq(b[1], 0);
  expect_eq(b[2], 2);
  expect_eq(b[3], 3);
  expect_eq(b[4], 333333 & 0xFF);
  expect_eq(b[5], (333333 >> 8) & 0xFF);
  expect_eq(b[6], 333333 >> 16);
  expect_eq(b[18], 614400 & 0xFF);
  expect_eq(b[22], 0);
  expect_eq(b[23], 4);
  expect_eq(b[33], 0);
  expect_eq(b[34], 0xEE);
  memset(&q, 0, sizeof q);
  uvc_probe_unpack(b, 34, &q);
  expect(!memcmp(&p, &q, sizeof p));
  expect_eq(uvc_probe_size(0x0100), 26);
  expect_eq(uvc_probe_size(0x0110), 34);
  expect_eq(uvc_probe_size(0x0150), 48);
}

/* --- reassembly --- */

static uint8_t frames[4][70000];
static uint32_t frame_len[4];
static int nframes;
static uint8_t spare[70000];

static uint8_t *collect(void *ctx, uint8_t *frame, uint32_t len)
{
  (void)ctx;
  memcpy(frames[nframes], frame, len);
  frame_len[nframes++] = len;
  return frame;
}

static void asm_init(struct uvc_asm *a, uint32_t fourcc, int w, int h)
{
  memset(a, 0, sizeof *a);
  a->buf = spare;
  a->cap = sizeof spare;
  a->fourcc = fourcc;
  a->w = (uint32_t)w;
  a->h = (uint32_t)h;
  nframes = 0;
}

static int payload(uint8_t *out, int info, const uint8_t *data, int n, int hlen)
{
  memset(out, 0, (size_t)hlen);
  out[0] = (uint8_t)hlen;
  out[1] = (uint8_t)info;
  memcpy(out + hlen, data, (size_t)n);
  return hlen + n;
}

static void jpeg_frames_over_payloads(void)
{
  struct uvc_asm a;
  uint8_t pl[300], part1[] = {0xFF, 0xD8, 1, 2, 3}, part2[] = {4, 5, 6}, part3[] = {7, 0xFF, 0xD9};
  asm_init(&a, UVC_MJPG, 640, 480);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0, part1, 5, 12), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0, part2, 3, 12), true, true, collect, NULL);
  expect_eq(nframes, 0);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0x02, part3, 3, 12), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 11);
  expect_eq(frames[0][0], 0xFF);
  expect_eq(frames[0][5], 4);
  expect_eq(frames[0][10], 0xD9);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0x01 | 0x02, part1, 5, 2), true, true, collect, NULL);
  expect_eq(nframes, 2);
  expect_eq(frame_len[1], 5);
}

static void empty_payloads_are_ignored(void)
{
  struct uvc_asm a;
  uint8_t pl[16], part[] = {0xFF, 0xD8, 9, 9};
  asm_init(&a, UVC_MJPG, 640, 480);
  uvc_asm_payload(&a, pl, 0, true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80, part, 4, 2), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, 0, true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, part, 0, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 4);
  expect_eq(a.dropped, 0);
}

static void fid_toggle_completes_without_eof(void)
{
  struct uvc_asm a;
  uint8_t pl[32], f1[] = {0xFF, 0xD8, 1, 1}, f2[] = {0xFF, 0xD8, 2, 2, 2};
  asm_init(&a, UVC_MJPG, 640, 480);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0, f1, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 0);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 1, f2, 5, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 4);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0, f1, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 2);
  expect_eq(frame_len[1], 5);
}

static void corrupt_frames_are_dropped(void)
{
  struct uvc_asm a;
  uint8_t pl[32], f[] = {0xFF, 0xD8, 1, 2};
  asm_init(&a, UVC_MJPG, 640, 480);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0x40, f, 4, 2), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0x02, f, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 0);
  expect_eq(a.dropped, 1);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 1 | 2, f, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);

  uint8_t junk[] = {1, 2, 3, 4};
  uvc_asm_payload(&a, pl, payload(pl, 0x80, junk, 4, 2), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, junk, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(a.dropped, 2);

  uvc_asm_payload(&a, pl, payload(pl, 0x80, f, 4, 2), true, true, collect, NULL);
  uint8_t tiny[] = {5};
  uvc_asm_payload(&a, tiny, 1, true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, f, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(a.dropped, 3);
}

static void still_image_payload_ignored(void)
{
  struct uvc_asm a;
  uint8_t pl[32], f[] = {0xFF, 0xD8, 1, 2};
  asm_init(&a, UVC_MJPG, 640, 480);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 0x20 | 0x02, f, 4, 2), true, true, collect, NULL);
  expect_eq(nframes, 0);
  expect_eq(a.dropped, 0);
}

static void overflow_drops_frame(void)
{
  struct uvc_asm a;
  uint8_t pl[300], f[] = {0xFF, 0xD8, 1, 2, 3, 4, 5, 6};
  asm_init(&a, UVC_MJPG, 640, 480);
  a.cap = 10;
  uvc_asm_payload(&a, pl, payload(pl, 0x80, f, 8, 2), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, f, 8, 2), true, true, collect, NULL);
  expect_eq(nframes, 0);
  expect_eq(a.dropped, 1);
}

static void raw_frames_need_exact_size(void)
{
  struct uvc_asm a;
  static uint8_t px[4 * 2 * 2], pl[200];
  asm_init(&a, UVC_YUY2, 4, 2);
  memset(px, 0x55, sizeof px);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, px, 8, 12), true, true, collect, NULL);
  expect_eq(nframes, 0);
  expect_eq(a.dropped, 1);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 1, px, 8, 12), true, true, collect, NULL);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 1 | 2, px + 8, 8, 12), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 16);

  asm_init(&a, UVC_NV12, 4, 2);
  uvc_asm_payload(&a, pl, payload(pl, 0x80 | 2, px, 12, 2), true, true, collect, NULL);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 12);
}

static void bulk_payload_boundaries(void)
{
  struct uvc_asm a;
  uint8_t jp[] = {0xFF, 0xD8, 0xAA, 0xBB}, tail[] = {1, 2, 3, 4, 5}, chunk[32];
  asm_init(&a, UVC_MJPG, 640, 480);

  /* a 16 byte packet (mps 16) does not end the payload, the short one after it does */
  expect_eq(payload(chunk, 0x80 | 2, jp, 4, 12), 16);
  uvc_asm_bulk(&a, chunk, 16, 16, 1000, collect, NULL);
  expect_eq(a.pl_bytes, 16);
  expect_eq(nframes, 0);
  uvc_asm_bulk(&a, tail, 5, 16, 1000, collect, NULL);
  expect_eq(a.pl_bytes, 0);
  expect_eq(nframes, 1);
  expect_eq(frame_len[0], 9);

  /* an exact multiple of mps is closed by a zero-length packet */
  expect_eq(payload(chunk, 0x80 | 1 | 2, jp, 4, 12), 16);
  uvc_asm_bulk(&a, chunk, 16, 16, 1000, collect, NULL);
  expect_eq(nframes, 1);
  uvc_asm_bulk(&a, chunk, 0, 16, 1000, collect, NULL);
  expect_eq(nframes, 2);
  expect_eq(frame_len[1], 4);
  expect_eq(a.pl_bytes, 0);

  /* reaching dwMaxPayloadTransferSize ends the payload without a short packet */
  asm_init(&a, UVC_MJPG, 640, 480);
  expect_eq(payload(chunk, 0x80 | 2, jp, 4, 12), 16);
  uvc_asm_bulk(&a, chunk, 16, 16, 16, collect, NULL);
  expect_eq(a.pl_bytes, 0);
  expect_eq(nframes, 1);

  /* a stray zero-length packet between payloads does nothing */
  uvc_asm_bulk(&a, chunk, 0, 16, 16, collect, NULL);
  expect_eq(nframes, 1);
}

int main(void)
{
  test("config: formats, frames and intervals", parse_formats);
  test("config: isochronous alternate settings", parse_alts);
  test("config: high-bandwidth endpoints", high_bandwidth_endpoint);
  test("config: SuperSpeed companion overrides the payload", super_speed_companion);
  test("config: bulk streaming camera", bulk_streaming);
  test("config: unusable devices are refused", unusable_cameras);
  test("config: frames too large for the heap are not offered", oversized_uncompressed_dropped);
  test("config: NV12 format", nv12_format);
  test("frame rate list and interval lookup", fps_selection);
  test("alternate setting is the smallest that fits", alt_selection);
  test("probe/commit block layout", probe_layout);
  test("JPEG frame spans several payloads", jpeg_frames_over_payloads);
  test("empty and header-only payloads", empty_payloads_are_ignored);
  test("frame toggle completes a frame without EOF", fid_toggle_completes_without_eof);
  test("error, non-JPEG and runt payloads drop the frame", corrupt_frames_are_dropped);
  test("still-image payloads are ignored", still_image_payload_ignored);
  test("frame larger than the buffer is dropped", overflow_drops_frame);
  test("uncompressed frames need the exact size", raw_frames_need_exact_size);
  test("bulk payload boundaries", bulk_payload_boundaries);
  return 0;
}
