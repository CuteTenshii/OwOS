/* usb3_test.c: SuperSpeed/SuperSpeedPlus link-speed parsing (drivers/usb/usb3.c): xHCI Supported Protocol
 * capabilities, BOS SuperSpeedPlus capabilities, hub extended port status and rate formatting. */
#define OWOS_SUITE_TEST
#include "../drivers/usb/usb3.h"
#include "test.h"
#include <string.h>

#define NAME_USB 0x20425355u

static uint32_t proto_hdr(int major, int minor) { return 2u | 0x0400u | (uint32_t)minor << 16 | (uint32_t)major << 24; }
static uint32_t ports(int first, int count, int psic)
{
  return (uint32_t)first | (uint32_t)count << 8 | (uint32_t)psic << 28;
}

/* A PSI dword: speed id, exponent (0 b/s .. 3 Gb/s), link type, SuperSpeedPlus protocol, mantissa. */
static uint32_t psi(int psiv, int psie, int plt, int ssp, int mantissa)
{
  return (uint32_t)psiv | (uint32_t)psie << 4 | (uint32_t)plt << 6 | 1u << 8 | (uint32_t)ssp << 14 |
         (uint32_t)mantissa << 16;
}

static void usb30_defaults(void)
{
  uint32_t dw[4] = {proto_hdr(3, 0x00), NAME_USB, ports(5, 4, 0), 0};
  struct usb3_proto p;
  uint32_t kbps = 0;
  expect_eq(usb3_proto_parse(dw, 4, &p), 0);
  expect_eq(p.major, 3);
  expect_eq(p.minor, 0);
  expect_eq(p.port_first, 5);
  expect_eq(p.port_count, 4);
  expect_eq(p.npsi, 0);
  expect_eq(usb3_port_speed(&p, 4, &kbps), USB_SPEED_SUPER);
  expect_eq(kbps, 5000000);
  /* a USB 3.0 port has no Gen 2 or dual-lane ids */
  expect_eq(usb3_port_speed(&p, 5, &kbps), 0);
  expect_eq(usb3_port_speed(&p, 7, &kbps), 0);
  expect_eq(usb3_port_speed(&p, 3, &kbps), 0);
  expect_eq(usb3_port_speed(&p, 8, &kbps), 0);
}

static void usb31_and_32_defaults(void)
{
  uint32_t dw[4] = {proto_hdr(3, 0x10), NAME_USB, ports(1, 2, 0), 0};
  struct usb3_proto p;
  uint32_t kbps = 0;
  expect_eq(usb3_proto_parse(dw, 4, &p), 0);
  expect_eq(usb3_port_speed(&p, 5, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 10000000);
  expect_eq(usb3_port_speed(&p, 6, &kbps), 0);
  expect_eq(usb3_port_speed(&p, 7, &kbps), 0);
  dw[0] = proto_hdr(3, 0x20);
  expect_eq(usb3_proto_parse(dw, 4, &p), 0);
  expect_eq(usb3_port_speed(&p, 6, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 10000000);
  expect_eq(usb3_port_speed(&p, 7, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 20000000);
}

static void usb31_psi_table(void)
{
  uint32_t dw[8] = {proto_hdr(3, 0x10),  NAME_USB,           ports(1, 2, 4), 0,
                    psi(4, 3, 0, 0, 5),  psi(6, 3, 3, 1, 5), /* asymmetric transmit half listed first */
                    psi(6, 3, 2, 1, 10), psi(9, 3, 0, 1, 10)};
  struct usb3_proto p;
  uint32_t kbps = 0;
  expect_eq(usb3_proto_parse(dw, 8, &p), 0);
  expect_eq(p.minor, 0x10);
  expect_eq(p.npsi, 4);
  expect_eq(usb3_port_speed(&p, 4, &kbps), USB_SPEED_SUPER);
  expect_eq(kbps, 5000000);
  expect_eq(usb3_port_speed(&p, 6, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 10000000);
  /* a custom id for Gen 2: the table decides, not the default numbering */
  expect_eq(usb3_port_speed(&p, 9, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 10000000);
  expect_eq(usb3_port_speed(&p, 5, &kbps), 0);
}

static void usb2_psi_table(void)
{
  uint32_t dw[7] = {proto_hdr(2, 0x00),  NAME_USB, ports(1, 4, 3), 0, psi(1, 2, 0, 0, 12), psi(2, 1, 0, 0, 1500),
                    psi(3, 2, 0, 0, 480)};
  struct usb3_proto p;
  uint32_t kbps = 0;
  expect_eq(usb3_proto_parse(dw, 7, &p), 0);
  expect_eq(usb3_port_speed(&p, 1, &kbps), USB_SPEED_FULL);
  expect_eq(kbps, 12000);
  expect_eq(usb3_port_speed(&p, 2, &kbps), USB_SPEED_LOW);
  expect_eq(kbps, 1500);
  expect_eq(usb3_port_speed(&p, 3, &kbps), USB_SPEED_HIGH);
  expect_eq(kbps, 480000);
}

static void malformed_protocols(void)
{
  struct usb3_proto p;
  uint32_t truncated[5] = {proto_hdr(3, 0x10), NAME_USB, ports(1, 2, 3), 0, psi(4, 3, 0, 0, 5)};
  expect_eq(usb3_proto_parse(truncated, 5, &p), -1);
  uint32_t legacy[4] = {1u, NAME_USB, ports(1, 2, 0), 0};
  expect_eq(usb3_proto_parse(legacy, 4, &p), -1);
  uint32_t other[4] = {proto_hdr(3, 0), 0x12345678u, ports(1, 2, 0), 0};
  expect_eq(usb3_proto_parse(other, 4, &p), -1);
  uint32_t kbps = 7;
  expect_eq(usb3_port_speed(NULL, 3, &kbps), USB_SPEED_HIGH);
  expect_eq(kbps, 480000);
  expect_eq(usb3_port_speed(NULL, 0, &kbps), 0);
}

static void put32(uint8_t *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t sublink(int ssid, int lse, int asym, int tx, int ssp, int mantissa)
{
  return (uint32_t)ssid | (uint32_t)lse << 4 | (uint32_t)asym << 6 | (uint32_t)tx << 7 | (uint32_t)ssp << 14 |
         (uint32_t)mantissa << 16;
}

/* BOS of a Gen 2 hub or device: USB 2.0 extension, SuperSpeed and SuperSpeedPlus capabilities. Returns its length. */
static int gen2_bos(uint8_t *b)
{
  static const uint8_t usb2_ext[7] = {7, 0x10, 0x02, 0x02, 0, 0, 0};
  static const uint8_t ss[10] = {10, 0x10, 0x03, 0, 0x0E, 0, 1, 0x0A, 0xFF, 0x07};
  int n = 5;
  memcpy(b + n, usb2_ext, 7);
  n += 7;
  memcpy(b + n, ss, 10);
  n += 10;
  uint8_t *c = b + n;
  memset(c, 0, 28);
  c[0] = 28;
  c[1] = 0x10;
  c[2] = 0x0A;
  put32(c + 4, 3u | 1u << 5);
  c[8] = 0x00;
  c[9] = 0x11; /* one receive and one transmit lane at least */
  put32(c + 12, sublink(0, 3, 0, 0, 0, 5));
  put32(c + 16, sublink(1, 3, 0, 0, 1, 10));
  put32(c + 20, sublink(1, 3, 1, 1, 1, 20));
  put32(c + 24, sublink(2, 2, 0, 0, 1, 10000));
  n += 28;
  b[0] = 5;
  b[1] = 0x0F;
  b[2] = (uint8_t)n;
  b[3] = 0;
  b[4] = 3;
  return n;
}

static void bos_superspeed_plus(void)
{
  uint8_t b[64];
  int n = gen2_bos(b);
  struct usb3_bos bos;
  expect_eq(usb3_bos_parse(b, n, &bos), 0);
  expect(bos.ss);
  expect(bos.ssp);
  expect_eq(bos.min_rx_lanes, 1);
  expect_eq(bos.min_tx_lanes, 1);
  expect_eq(bos.nsublinks, 4);
  expect_eq(bos.sublinks[1].ssid, 1);
  expect(bos.sublinks[1].ssp);
  expect_eq(bos.sublinks[1].kbps, 10000000);
  expect(bos.sublinks[2].tx);
  expect_eq(bos.sublinks[3].kbps, 10000000);
  /* the 20 Gbps entry is a transmit half and does not count */
  expect_eq(usb3_bos_max_kbps(&bos), 10000000);
}

static void bos_without_superspeed_plus(void)
{
  uint8_t b[64];
  int n = gen2_bos(b) - 28;
  b[2] = (uint8_t)n;
  b[4] = 2;
  struct usb3_bos bos;
  expect_eq(usb3_bos_parse(b, n, &bos), 0);
  expect(bos.ss);
  expect(!bos.ssp);
  expect_eq(usb3_bos_max_kbps(&bos), 0);
}

static void bos_malformed(void)
{
  uint8_t b[64];
  struct usb3_bos bos;
  int n = gen2_bos(b);
  /* wTotalLength stops short of the SuperSpeedPlus capability, which must then be ignored */
  b[2] = (uint8_t)(n - 4);
  expect_eq(usb3_bos_parse(b, n, &bos), 0);
  expect(bos.ss);
  expect(!bos.ssp);
  /* a zero-length capability ends the walk instead of looping */
  gen2_bos(b);
  b[5] = 0;
  expect_eq(usb3_bos_parse(b, n, &bos), 0);
  expect(!bos.ss);
  b[1] = 0x02;
  expect_eq(usb3_bos_parse(b, n, &bos), -1);
  expect_eq(usb3_bos_parse(b, 4, &bos), -1);
}

static void hub_extended_port_status(void)
{
  uint8_t b[64];
  struct usb3_bos hub;
  usb3_bos_parse(b, gen2_bos(b), &hub);
  uint32_t kbps = 0;
  expect_eq(usb3_ext_port_speed(&hub, 0x00000011u, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 10000000);
  /* Gen 2x2: two receive lanes */
  expect_eq(usb3_ext_port_speed(&hub, 0x00001111u, &kbps), USB_SPEED_SUPER_PLUS);
  expect_eq(kbps, 20000000);
  expect_eq(usb3_ext_port_speed(&hub, 0x00000000u, &kbps), USB_SPEED_SUPER);
  expect_eq(kbps, 5000000);
  expect_eq(usb3_ext_port_speed(&hub, 0x00000009u, &kbps), USB_SPEED_SUPER);
  expect_eq(kbps, 5000000);
  struct usb3_bos plain;
  memset(&plain, 0, sizeof plain);
  expect_eq(usb3_ext_port_speed(&plain, 0x00000011u, &kbps), USB_SPEED_SUPER);
}

/* A configuration with a bulk pair (SuperSpeed companions), an interrupt endpoint without one, and an isochronous
 * endpoint with a SuperSpeedPlus companion. */
static int companion_cfg(uint8_t *c)
{
  static const uint8_t d[] = {
      9, 2,    0,    0,    2,    1,    0,    0x80, 50, /* configuration */
      9, 4,    0,    0,    3,    8,    6,    0x50, 0,  /* interface     */
      7, 5,    0x81, 2,    0x00, 4,    0,              /* bulk IN       */
      6, 0x30, 15,   0,    0,    0,                    /* burst 16      */
      7, 5,    0x02, 2,    0x00, 4,    0,              /* bulk OUT      */
      6, 0x30, 3,    0,    0,    0,                    /* burst 4       */
      7, 5,    0x83, 3,    0x08, 0,    4,              /* interrupt IN  */
      9, 4,    1,    1,    1,    14,   2,    0,    0,  /* interface     */
      7, 5,    0x84, 1,    0x00, 4,    1,              /* isochronous   */
      6, 0x30, 15,   0x80, 0,    0,                    /* SSP follows   */
      8, 0x31, 0,    0,    0x00, 0x80, 0x01, 0,        /* 96 KiB        */
  };
  memcpy(c, d, sizeof d);
  return (int)sizeof d;
}

static void endpoint_companions(void)
{
  uint8_t c[128];
  int n = companion_cfg(c);
  struct usb3_ep_comp e;
  expect_eq(usb3_ep_companion(c, n, 0x81, &e), 0);
  expect_eq(e.max_burst, 15);
  expect_eq(usb3_ep_companion(c, n, 0x02, &e), 0);
  expect_eq(e.max_burst, 3);
  expect_eq(usb3_ep_companion(c, n, 0x84, &e), 0);
  expect_eq(e.max_burst, 15);
  expect_eq(e.attributes, 0x80);
  expect_eq(e.bytes_per_interval, 0x18000);
  /* no companion (a USB 2 endpoint) and no such endpoint both leave out zeroed */
  e.max_burst = 9;
  expect_eq(usb3_ep_companion(c, n, 0x83, &e), -1);
  expect_eq(e.max_burst, 0);
  expect_eq(usb3_ep_companion(c, n, 0x85, &e), -1);
}

static void endpoint_companion_malformed(void)
{
  uint8_t c[128];
  int n = companion_cfg(c);
  struct usb3_ep_comp e;
  /* an out-of-range bMaxBurst is clamped to the endpoint context's 4-bit field */
  c[18 + 7 + 2] = 40;
  expect_eq(usb3_ep_companion(c, n, 0x81, &e), 0);
  expect_eq(e.max_burst, 15);
  /* the companion cut off by the end of the buffer is not read */
  expect_eq(usb3_ep_companion(c, 18 + 7 + 3, 0x81, &e), -1);
  /* an SSP companion cut off falls back to wBytesPerInterval */
  expect_eq(usb3_ep_companion(c, n - 2, 0x84, &e), 0);
  expect_eq(e.bytes_per_interval, 0);
  /* a zero-length descriptor stops the walk */
  c[9] = 0;
  expect_eq(usb3_ep_companion(c, n, 0x81, &e), -1);
}

static void default_rates(void)
{
  expect_eq(usb3_default_kbps(USB_SPEED_LOW), 1500);
  expect_eq(usb3_default_kbps(USB_SPEED_FULL), 12000);
  expect_eq(usb3_default_kbps(USB_SPEED_HIGH), 480000);
  expect_eq(usb3_default_kbps(USB_SPEED_SUPER), 5000000);
  expect_eq(usb3_default_kbps(USB_SPEED_SUPER_PLUS), 10000000);
  expect_eq(usb3_default_kbps(0), 0);
}

static void rate_strings(void)
{
  char s[16];
  expect_string(usb3_rate_str(1500, s, sizeof s), "1.5 Mbps");
  expect_string(usb3_rate_str(12000, s, sizeof s), "12 Mbps");
  expect_string(usb3_rate_str(480000, s, sizeof s), "480 Mbps");
  expect_string(usb3_rate_str(5000000, s, sizeof s), "5 Gbps");
  expect_string(usb3_rate_str(10000000, s, sizeof s), "10 Gbps");
  expect_string(usb3_rate_str(20000000, s, sizeof s), "20 Gbps");
  expect_string(usb3_rate_str(500, s, sizeof s), "500 kbps");
  expect_string(usb3_rate_str(0, s, sizeof s), "0 kbps");
  expect_string(usb3_rate_str(480000, s, 4), "480");
}

int main(void)
{
  test("protocol: USB 3.0 default speed ids", usb30_defaults);
  test("protocol: USB 3.1 and 3.2 default speed ids", usb31_and_32_defaults);
  test("protocol: USB 3.1 speed id table", usb31_psi_table);
  test("protocol: USB 2 speed id table", usb2_psi_table);
  test("protocol: malformed capabilities", malformed_protocols);
  test("bos: SuperSpeedPlus sublinks", bos_superspeed_plus);
  test("bos: SuperSpeed only", bos_without_superspeed_plus);
  test("bos: malformed descriptors", bos_malformed);
  test("hub: extended port status", hub_extended_port_status);
  test("endpoint: SuperSpeed companions", endpoint_companions);
  test("endpoint: malformed companions", endpoint_companion_malformed);
  test("default rates by speed class", default_rates);
  test("rate formatting", rate_strings);
  return 0;
}
