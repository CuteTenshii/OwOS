/* hid_desc_test.c: USB HID descriptor parser, report decoding, key translation, typematic and interval logic
 * (drivers/usb/hid_desc.c, hid_kbd.h, usbhost.h, keymap.h). */
#define OWOS_SUITE_TEST
#include "../drivers/input/keymap.h"
#include "../drivers/usb/hid_desc.h"
#include "../drivers/usb/hid_kbd.h"
#include "../drivers/usb/usbhost.h"
#include "test.h"
#include <string.h>

/* Dumped from QEMU's usb-kbd, usb-mouse and usb-tablet through the guest's own HID bind log. */
static const uint8_t qemu_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};
static const uint8_t qemu_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
};
static const uint8_t qemu_tablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25,
    0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};

/* Second interface of a typical keyboard: consumer controls (report ID 2) and system controls (ID 3). */
static const uint8_t kbd_consumer[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x19, 0x00, 0x2a, 0x3c, 0x02, 0x15, 0x00, 0x26, 0x3c, 0x02,
    0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0, 0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x03, 0x19, 0x81, 0x29,
    0x83, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0xc0,
};
/* Consumer keys declared as individual bits (ID 4): volume up, volume down, mute, play/pause, calculator. */
static const uint8_t consumer_bits[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x09, 0xe9,
    0x09, 0xea, 0x09, 0xe2, 0x09, 0xcd, 0x0a, 0x92, 0x01, 0x81, 0x02, 0x75, 0x03, 0x95, 0x01, 0x81, 0x01, 0xc0,
};
/* N-key rollover keyboard: modifiers plus a bitmap of keyboard usages 4..0x67, LED output with a report ID. */
static const uint8_t nkro_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x19, 0x04, 0x29, 0x67, 0x95, 0x64, 0x81, 0x02, 0x95, 0x04, 0x81, 0x01,
    0x85, 0x02, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x95, 0x03, 0x91, 0x02, 0x95, 0x05, 0x91, 0x01, 0xc0,
};
/* Gamepad: four 8-bit axes (X Y Z Rz), a 4-bit hat with null state, 12 buttons crossing a byte boundary. */
static const uint8_t gamepad[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26,
    0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81,
    0x42, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02, 0xc0, 0xc0,
};
/* Xbox-style pad: 16 buttons in input report 1, haptics output report 3 (enable, four magnitudes, duration). */
static const uint8_t rumble_pad[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x0f, 0x09, 0x21, 0x85, 0x03, 0xa1, 0x02, 0x09, 0x97, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x04, 0x95, 0x01, 0x91, 0x02, 0x75, 0x04, 0x91, 0x03, 0x09, 0x70, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95,
    0x04, 0x91, 0x02, 0x09, 0x50, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0xc0, 0xc0,
};
/* Touchscreen with two finger collections: tip, contact ID, 12-bit-range X/Y in 16 bits, report ID 1. */
static const uint8_t touch2[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x81, 0x03, 0x09, 0x51, 0x25, 0x0f, 0x75, 0x08,
    0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
    0x05, 0x0d, 0xc0, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x81, 0x02, 0x75, 0x07, 0x81, 0x03, 0x09, 0x51, 0x25, 0x0f, 0x75, 0x08, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x30, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0x05, 0x0d, 0xc0, 0xc0,
};

/* Pen digitizer: tip, barrel, eraser, secondary barrel and In Range bits, then 16-bit X and Y. */
static const uint8_t pen[] = {
    0x05, 0x0d, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x20, 0xa1, 0x00, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x05, 0x09, 0x42, 0x09, 0x44, 0x09, 0x45, 0x09, 0x5a, 0x09, 0x32, 0x81, 0x02, 0x95, 0x03, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0, 0xc0,
};

/* Wheel mouse with a horizontal wheel as AC Pan (Consumer 0x238), as Microsoft and Logitech mice declare it. */
static const uint8_t pan_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};
/* System control as an array whose logical minimum is 1 (ID 5). */
static const uint8_t sysctl_array[] = {
    0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x05, 0x19, 0x81, 0x29, 0x83,
    0x15, 0x01, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};
/* Consumer array whose logical range (0..255) is wider than its usage range (0..5). */
static const uint8_t wide_array[] = {
    0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x05, 0x0c, 0x19, 0x00, 0x29, 0x05,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
};
/* Four-way hat switch. */
static const uint8_t hat4[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x03,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x81, 0x01, 0xc0,
};

struct events {
  uint16_t key[64];
  bool down[64];
  int n;
};

static void record(uint16_t key, bool down, void *ctx)
{
  struct events *e = ctx;
  e->key[e->n] = key;
  e->down[e->n] = down;
  e->n++;
}

static const struct hid_field *find(const struct hid_desc *d, uint32_t usage, uint8_t kind)
{
  for (size_t i = 0; i < d->nfields; i++)
    if (d->fields[i].usage == usage && d->fields[i].kind == kind)
      return &d->fields[i];
  return NULL;
}

static struct hid_desc d;
static struct hid_decoded dec;
static const struct hid_field *f;
static uint8_t led[4];
static uint8_t id;

static void boot_kbd_desc(void)
{
  expect(hid_parse(hid_boot_kbd_desc, sizeof hid_boot_kbd_desc, &d) == 0);
  expect(d.nfields == 8 + 5 + 1);
  expect(!d.has_ids);
  expect(hid_report_bytes(&d, HID_INPUT, 0) == 8);
  expect(hid_report_bytes(&d, HID_OUTPUT, 0) == 1);
  f = find(&d, HID_USAGE(7, 0xE1), HID_INPUT);
  expect(f && f->bit_off == 1 && f->size == 1 && f->count == 1 && f->lmax == 1);
  f = find(&d, HID_USAGE(7, 0), HID_INPUT);
  expect(f && f->bit_off == 16 && f->size == 8 && f->count == 6 && f->usage_max == HID_USAGE(7, 0x65));
  expect(f && !(f->flags & HID_F_VAR) && f->lmax == 0x65);
  f = find(&d, HID_USAGE(8, 2), HID_OUTPUT);
  expect(f && f->bit_off == 1 && f->size == 1);
  expect(hid_roles(&d) == HID_ROLE_KEYBOARD);
  hid_desc_free(&d);
}

static void qemu_kbd_desc(void)
{
  expect(hid_parse(qemu_kbd, sizeof qemu_kbd, &d) == 0);
  f = find(&d, HID_USAGE(7, 0), HID_INPUT);
  expect(f && f->lmax == 255 && f->usage_max == HID_USAGE(7, 0xFF));
  expect(hid_roles(&d) == HID_ROLE_KEYBOARD);
  hid_desc_free(&d);
}

static void kbd_report_decoding(void)
{
  expect(hid_parse(hid_boot_kbd_desc, sizeof hid_boot_kbd_desc, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t rep[8] = {0x02, 0, 0x04, 0x05, 0, 0, 0, 0};
  hid_decode(&d, rep, sizeof rep, &dec);
  expect(dec.valid && dec.has_kbd && !dec.phantom);
  expect(dec.mods == 0x02 && dec.nkeys == 2 && dec.keys[0] == 4 && dec.keys[1] == 5);
  uint8_t rep2[8] = {0x81, 0, 0x1E, 0, 0, 0, 0, 0};
  hid_decode(&d, rep2, sizeof rep2, &dec);
  expect(dec.mods == 0x81 && dec.nkeys == 1 && dec.keys[0] == 0x1E);
}

static void rollover_phantom(void)
{
  uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
  hid_decode(&d, rollover, sizeof rollover, &dec);
  expect(dec.phantom && dec.nkeys == 0);
  uint8_t partial[8] = {0, 0, 1, 1, 0x04, 1, 1, 1};
  hid_decode(&d, partial, sizeof partial, &dec);
  expect(!dec.phantom && dec.nkeys == 1 && dec.keys[0] == 4);
  uint8_t shortrep[3] = {0x01, 0, 0x04};
  hid_decode(&d, shortrep, sizeof shortrep, &dec);
  expect(dec.mods == 1 && dec.nkeys == 1 && dec.keys[0] == 4);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void boot_and_wheel_mouse(void)
{
  expect(hid_parse(hid_boot_mouse_desc, sizeof hid_boot_mouse_desc, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t m3[3] = {0x05, 0xFE, 0x03};
  hid_decode(&d, m3, sizeof m3, &dec);
  expect(dec.has_rel && dec.has_buttons && dec.buttons == 5 && dec.dx == -2 && dec.dy == 3 && dec.wheel == 0);
  expect(hid_roles(&d) == HID_ROLE_MOUSE);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
  expect(hid_parse(qemu_mouse, sizeof qemu_mouse, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t m4[4] = {0x1F, 0x7F, 0x80, 0xFF};
  hid_decode(&d, m4, sizeof m4, &dec);
  expect(dec.buttons == 0x1F && dec.dx == 127 && dec.dy == -128 && dec.wheel == -1 && !dec.has_abs);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void tablet_abs_axes(void)
{
  expect(hid_parse(qemu_tablet, sizeof qemu_tablet, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t t6[6] = {0x01, 0x00, 0x40, 0x00, 0x20, 0x01};
  hid_decode(&d, t6, sizeof t6, &dec);
  expect(dec.has_abs && dec.ax == 0x4000 && dec.ay == 0x2000 && dec.ax_max == 0x7FFF && dec.ay_max == 0x7FFF);
  expect(dec.buttons == 1 && dec.wheel == 1 && dec.ax_min == 0);
  expect(hid_roles(&d) == HID_ROLE_TABLET);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void pen_range_and_buttons(void)
{
  expect(hid_parse(pen, sizeof pen, &d) == 0);
  expect(hid_roles(&d) == HID_ROLE_TABLET);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t hover[5] = {0x10, 0x00, 0x10, 0x00, 0x20};
  hid_decode(&d, hover, sizeof hover, &dec);
  expect(dec.has_range && dec.in_range && !dec.tip && dec.buttons == 0 && dec.ax == 0x1000 && dec.ay == 0x2000);
  uint8_t away[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  hid_decode(&d, away, sizeof away, &dec);
  expect(dec.has_range && !dec.in_range);
  uint8_t barrel[5] = {0x13, 0x00, 0x10, 0x00, 0x20};
  hid_decode(&d, barrel, sizeof barrel, &dec);
  expect(dec.tip && dec.buttons == 2);
  uint8_t eraser[5] = {0x14, 0x00, 0x10, 0x00, 0x20};
  hid_decode(&d, eraser, sizeof eraser, &dec);
  expect(dec.tip && dec.buttons == 0);
  uint8_t second[5] = {0x18, 0x00, 0x10, 0x00, 0x20};
  hid_decode(&d, second, sizeof second, &dec);
  expect(!dec.tip && dec.buttons == 4);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void abs_axis_scaling(void)
{
  expect(hid_scale_abs(0x4000, 0, 0x7FFF, 1023) == 511);
  expect(hid_scale_abs(0, 0, 0x7FFF, 1023) == 0);
  expect(hid_scale_abs(0x7FFF, 0, 0x7FFF, 1023) == 1023);
  expect(hid_scale_abs(-5, 0, 100, 800) == 0);
  expect(hid_scale_abs(500, 0, 100, 800) == 800);
  expect(hid_scale_abs(50, 0, 0, 800) == 0);
  expect(hid_scale_abs(150, 100, 200, 1000) == 500);
  expect(hid_scale_i16(0, 0, 255) == -32768);
  expect(hid_scale_i16(255, 0, 255) == 32767);
  expect(hid_scale_i16(-128, -128, 127) == -32768);
  expect(hid_scale_i16(0, -128, 127) == 128);
}

static void consumer_report_ids(void)
{
  expect(hid_parse(kbd_consumer, sizeof kbd_consumer, &d) == 0);
  expect(d.has_ids);
  expect(hid_report_bytes(&d, HID_INPUT, 2) == 2 && hid_report_bytes(&d, HID_INPUT, 3) == 1);
  expect(hid_roles(&d) == HID_ROLE_CONSUMER);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t vol[3] = {2, 0xE9, 0x00};
  hid_decode(&d, vol, sizeof vol, &dec);
  expect(dec.valid && dec.report_id == 2 && dec.has_cons && dec.ncons == 1 && dec.cons[0] == HID_USAGE(0x0C, 0xE9));
  uint8_t browser[3] = {2, 0x23, 0x02};
  hid_decode(&d, browser, sizeof browser, &dec);
  expect(dec.ncons == 1 && dec.cons[0] == HID_USAGE(0x0C, 0x223));
  uint8_t none[3] = {2, 0, 0};
  hid_decode(&d, none, sizeof none, &dec);
  expect(dec.valid && dec.ncons == 0);
  uint8_t sleep[2] = {3, 0x02};
  hid_decode(&d, sleep, sizeof sleep, &dec);
  expect(dec.valid && dec.report_id == 3 && dec.ncons == 1 && dec.cons[0] == HID_USAGE(1, 0x82));
  uint8_t unknown_id[3] = {9, 0xE9, 0};
  hid_decode(&d, unknown_id, sizeof unknown_id, &dec);
  expect(!dec.valid && dec.ncons == 0);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void consumer_bits_keys(void)
{
  expect(hid_parse(consumer_bits, sizeof consumer_bits, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t bits[2] = {4, 0x0A};
  hid_decode(&d, bits, sizeof bits, &dec);
  expect(dec.ncons == 2 && dec.cons[0] == HID_USAGE(0x0C, 0xEA) && dec.cons[1] == HID_USAGE(0x0C, 0xCD));
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void horizontal_wheel_pan(void)
{
  expect(hid_parse(pan_mouse, sizeof pan_mouse, &d) == 0);
  expect(hid_roles(&d) == HID_ROLE_MOUSE);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t pm[5] = {0x02, 0x00, 0x00, 0x02, 0xFF};
  hid_decode(&d, pm, sizeof pm, &dec);
  expect(dec.buttons == 2 && dec.wheel == 2 && dec.pan == -1 && dec.ncons == 0 && !dec.has_cons);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void array_logical_min(void)
{
  expect(hid_parse(sysctl_array, sizeof sysctl_array, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t sc[2] = {5, 2};
  hid_decode(&d, sc, sizeof sc, &dec);
  expect(dec.ncons == 1 && dec.cons[0] == HID_USAGE(1, 0x82));
  uint8_t sc0[2] = {5, 0};
  hid_decode(&d, sc0, sizeof sc0, &dec);
  expect(dec.valid && dec.ncons == 0);
  uint8_t sc9[2] = {5, 9};
  hid_decode(&d, sc9, sizeof sc9, &dec);
  expect(dec.ncons == 0);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void array_beyond_usage(void)
{
  expect(hid_parse(wide_array, sizeof wide_array, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t wa[1] = {3};
  hid_decode(&d, wa, sizeof wa, &dec);
  expect(dec.ncons == 1 && dec.cons[0] == HID_USAGE(0x0C, 3));
  wa[0] = 200;
  hid_decode(&d, wa, sizeof wa, &dec);
  expect(dec.valid && dec.ncons == 0);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void four_way_hat(void)
{
  expect(hid_parse(hat4, sizeof hat4, &d) == 0);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t h1[1] = {0x01};
  hid_decode(&d, h1, sizeof h1, &dec);
  expect(dec.hat == 3);
  h1[0] = 0x00;
  hid_decode(&d, h1, sizeof h1, &dec);
  expect(dec.hat == 1);
  h1[0] = 0x0F;
  hid_decode(&d, h1, sizeof h1, &dec);
  expect(dec.hat == 0);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void bitmap_kbd_led_id(void)
{
  expect(hid_parse(nkro_kbd, sizeof nkro_kbd, &d) == 0);
  expect(hid_report_bytes(&d, HID_INPUT, 1) == 14);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t nk[16] = {1, 0x01, 0x05, 0, 0x80};
  hid_decode(&d, nk, sizeof nk, &dec);
  /* payload byte 1 = 0x05 sets bitmap bits 0 and 2 (usages 4 and 6); byte 3 bit 7 is bitmap bit 23 (usage 27) */
  expect(dec.mods == 1 && dec.nkeys == 3);
  expect(dec.keys[0] == 4 && dec.keys[1] == 6 && dec.keys[2] == 4 + 23);
  expect(hid_pack_leds(&d, 0x03, led, hid_report_bytes(&d, HID_OUTPUT, 2), &id) == 2);
  expect(id == 2 && led[0] == 0x03);
  expect(hid_pack_leds(&d, 0x04, led, 1, &id) == 2 && led[0] == 0x04);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
  expect(hid_parse(qemu_mouse, sizeof qemu_mouse, &d) == 0);
  expect(hid_pack_leds(&d, 3, led, 1, &id) == -1);
  hid_desc_free(&d);
}

static void boot_kbd_led_packing(void)
{
  expect(hid_parse(hid_boot_kbd_desc, sizeof hid_boot_kbd_desc, &d) == 0);
  memset(led, 0xFF, sizeof led);
  expect(hid_pack_leds(&d, 0x02, led, 1, &id) == 0 && id == 0);
  expect(led[0] == 0x02);
  hid_desc_free(&d);
}

static void gamepad_axes(void)
{
  expect(hid_parse(gamepad, sizeof gamepad, &d) == 0);
  expect(hid_report_bytes(&d, HID_INPUT, 0) == 6);
  expect(hid_roles(&d) == HID_ROLE_GAMEPAD);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t pad[6] = {0x10, 0x80, 0xFF, 0x00, 0x02 | 0xA0, 0xFF};
  hid_decode(&d, pad, sizeof pad, &dec);
  expect(dec.has_pad && !dec.has_rel && !dec.has_buttons);
  expect(dec.axes[0] == 0x10 && dec.axes[1] == 0x80 && dec.axes[2] == 0xFF && dec.axes[5] == 0 &&
         dec.axes_max[0] == 255);
  expect(dec.hat == 3);
  /* buttons are bits 36..47: high nibble of byte 4 (0xA = buttons 2 and 4) and all of byte 5 */
  expect(dec.pad_buttons == (0xA | (0xFFu << 4)));
  uint8_t pad2[6] = {0, 0, 0, 0, 0x0F, 0};
  hid_decode(&d, pad2, sizeof pad2, &dec);
  expect(dec.hat == 0 && dec.pad_buttons == 0);
  uint8_t pad3[6] = {0, 0, 0, 0, 0x00, 0};
  hid_decode(&d, pad3, sizeof pad3, &dec);
  expect(dec.hat == 1);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void touchscreen_first_finger(void)
{
  expect(hid_parse(touch2, sizeof touch2, &d) == 0);
  expect(hid_roles(&d) == HID_ROLE_TABLET);
  expect(hid_decoded_init(&dec, &d) == 0);
  uint8_t tp[13] = {1, 0x01, 0x00, 0x34, 0x02, 0x78, 0x01, 0x01, 0x01, 0xFF, 0x0F, 0xFF, 0x0F};
  hid_decode(&d, tp, sizeof tp, &dec);
  expect(dec.has_touch && dec.tip && dec.has_abs && dec.ax == 0x234 && dec.ay == 0x178 && dec.ax_max == 0xFFF);
  uint8_t tp_up[13] = {1, 0x00, 0x00, 0x34, 0x02, 0x78, 0x01, 0x01, 0x01, 0xFF, 0x0F, 0xFF, 0x0F};
  hid_decode(&d, tp_up, sizeof tp_up, &dec);
  expect(!dec.tip && dec.ax == 0x234);
  hid_decoded_free(&dec);
  hid_desc_free(&d);
}

static void bit_extraction(void)
{
  uint8_t raw[4] = {0xF0, 0x0F, 0xAA, 0x55};
  expect(hid_get_bits(raw, 4, 4, 8, false) == 0xFF);
  expect(hid_get_bits(raw, 4, 4, 8, true) == -1);
  expect(hid_get_bits(raw, 4, 0, 4, false) == 0);
  expect(hid_get_bits(raw, 4, 12, 12, false) == 0xAA0);
  expect(hid_get_bits(raw, 4, 12, 12, true) == (int32_t)0xFFFFFAA0);
  expect(hid_get_bits(raw, 4, 0, 32, false) == (int32_t)0x55AA0FF0);
  expect(hid_get_bits(raw, 4, 24, 16, false) == 0x55);
  expect(hid_get_bits(raw, 4, 31, 1, false) == 0);
  expect(hid_get_bits(raw, 4, 3, 1, true) == 0);
  expect(hid_get_bits(raw, 4, 7, 1, true) == -1);
  uint8_t out[3] = {0};
  hid_set_bits(out, 3, 5, 10, 0x2B7);
  expect(hid_get_bits(out, 3, 5, 10, false) == 0x2B7);
  expect(out[0] == 0xE0 && out[1] == 0x56);
  hid_set_bits(out, 3, 5, 10, 0);
  expect(out[0] == 0 && out[1] == 0 && out[2] == 0);
  hid_set_bits(out, 3, 20, 8, 0xFF);
  expect(out[2] == 0xF0);
}

static void descriptor_edge_cases(void)
{
  expect(hid_parse((const uint8_t *)"", 0, &d) == -1);
  static const uint8_t trunc[] = {0x05, 0x01, 0x09, 0x06, 0xa1};
  expect(hid_parse(trunc, sizeof trunc, &d) == -1);
  static const uint8_t trunc_item[] = {0x05, 0x01, 0xa1, 0x01, 0x26, 0xff};
  expect(hid_parse(trunc_item, sizeof trunc_item, &d) == -1);
  static const uint8_t longitem[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xfe, 0x02, 0x10, 0xaa, 0xbb, 0x09,
                                     0x30, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0};
  expect(hid_parse(longitem, sizeof longitem, &d) == 0);
  f = find(&d, HID_USAGE(1, 0x30), HID_INPUT);
  expect(f && (f->flags & HID_F_REL) && f->lmin == -127 && f->lmax == 127);
  hid_desc_free(&d);
  static const uint8_t longtrunc[] = {0x05, 0x01, 0xfe, 0x09, 0x10, 0xaa};
  expect(hid_parse(longtrunc, sizeof longtrunc, &d) == -1);
  /* Push/Pop restores the global report size; delimiters keep only the first usage. */
  static const uint8_t pushpop[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x75, 0x08, 0xa4, 0x75, 0x01,
                                    0x95, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25,
                                    0x01, 0x81, 0x02, 0xb4, 0x95, 0x02, 0xa9, 0x01, 0x09, 0x30, 0x09,
                                    0x31, 0xa9, 0x00, 0x15, 0x81, 0x25, 0x7f, 0x81, 0x06, 0xc0};
  expect(hid_parse(pushpop, sizeof pushpop, &d) == 0);
  f = find(&d, HID_USAGE(9, 1), HID_INPUT);
  expect(f && f->size == 1 && f->bit_off == 0);
  f = find(&d, HID_USAGE(1, 0x30), HID_INPUT);
  expect(f && f->size == 8 && f->bit_off == 1);
  expect(!find(&d, HID_USAGE(1, 0x31), HID_INPUT));
  expect(d.nfields == 3 && d.fields[2].usage == HID_USAGE(1, 0x30) && d.fields[2].bit_off == 9);
  hid_desc_free(&d);
  static const uint8_t bigcount[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x75,
                                     0x01, 0x96, 0xff, 0xff, 0x81, 0x02, 0xc0};
  expect(hid_parse(bigcount, sizeof bigcount, &d) == -1);
  static const uint8_t bigarray[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x75,
                                     0x01, 0x96, 0x88, 0x13, 0x81, 0x00, 0xc0};
  expect(hid_parse(bigarray, sizeof bigarray, &d) == -1);
  static const uint8_t longcut[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x75, 0x01,
                                    0x95, 0x01, 0x81, 0x02, 0xfe, 0x09, 0x10, 0xaa};
  expect(hid_parse(longcut, sizeof longcut, &d) == -1);
  expect(d.nfields == 0 && d.fields == NULL);
}

static void report_id_bit_offsets(void)
{
  static const uint8_t two_ids[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x75, 0x08, 0x95, 0x01,
                                    0x15, 0x00, 0x25, 0x7f, 0x81, 0x02, 0x85, 0x02, 0x09, 0x31, 0x81, 0x02, 0xc0};
  expect(hid_parse(two_ids, sizeof two_ids, &d) == 0);
  f = find(&d, HID_USAGE(1, 0x31), HID_INPUT);
  expect(f && f->report_id == 2 && f->bit_off == 0);
  hid_desc_free(&d);
}

static void usage_to_set1(void)
{
  static const char rows[3][11] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const uint8_t row_base[3] = {0x10, 0x1E, 0x2C};
  for (int r = 0; r < 3; r++)
    for (int i = 0; rows[r][i]; i++)
      expect(hid_usage_set1((uint8_t)(4 + rows[r][i] - 'a')) == row_base[r] + i);
  for (int i = 0; i < 10; i++)
    expect(hid_usage_set1((uint8_t)(0x1E + i)) == 0x02 + i);
  expect(hid_usage_set1(0x28) == 0x1C && hid_usage_set1(0x29) == 0x01 && hid_usage_set1(0x2A) == 0x0E);
  expect(hid_usage_set1(0x2B) == 0x0F && hid_usage_set1(0x2C) == 0x39 && hid_usage_set1(0x39) == 0x3A);
  expect(hid_usage_set1(0x3A) == 0x3B && hid_usage_set1(0x43) == 0x44);
  expect(hid_usage_set1(0x44) == 0x57 && hid_usage_set1(0x45) == 0x58);
  expect(hid_usage_set1(0x52) == 0x148 && hid_usage_set1(0x51) == 0x150);
  expect(hid_usage_set1(0x50) == 0x14B && hid_usage_set1(0x4F) == 0x14D);
  expect(hid_usage_set1(0x4A) == 0x147 && hid_usage_set1(0x4D) == 0x14F && hid_usage_set1(0x4C) == 0x153);
  expect(hid_usage_set1(0x53) == 0x45 && hid_usage_set1(0x59) == 0x4F && hid_usage_set1(0x62) == 0x52);
  expect(hid_usage_set1(0x58) == 0x11C && hid_usage_set1(0x54) == 0x135 && hid_usage_set1(0x64) == 0x56);
  expect(hid_usage_set1(0xE0) == 0x1D && hid_usage_set1(0xE1) == 0x2A && hid_usage_set1(0xE2) == 0x38);
  expect(hid_usage_set1(0xE3) == 0x15B && hid_usage_set1(0xE4) == 0x11D && hid_usage_set1(0xE5) == 0x36);
  expect(hid_usage_set1(0xE6) == 0x138 && hid_usage_set1(0xE7) == 0x15C);
  expect(hid_usage_set1(0x46) == 0x137 && hid_usage_set1(0x47) == 0x46);
  expect(hid_usage_set1(0x00) == 0 && hid_usage_set1(0x01) == 0 && hid_usage_set1(0x70) == 0 &&
         hid_usage_set1(0xFF) == 0);
}

static void laptop_media_fkeys(void)
{
  expect(hid_usage_set1(0x3E) == 0 && hid_usage_set1(0x3F) == 0 && hid_usage_set1(0x41) == 0 &&
         hid_usage_set1(0x42) == 0);
  expect(hid_usage_set1(0x3D) == 0x3E && hid_usage_set1(0x40) == 0x41);
  expect(hid_fkey_key(0x3E) == KEY_F5 && hid_fkey_key(0x3F) == KEY_F6 && hid_fkey_key(0x41) == KEY_F8 &&
         hid_fkey_key(0x42) == KEY_F9 && hid_fkey_key(0x40) == 0);
}

static void special_key_mapping(void)
{
  expect(hid_special_key(HID_USAGE(0x0C, 0xE9)) == KEY_VOL_UP);
  expect(hid_special_key(HID_USAGE(0x0C, 0xCD)) == KEY_MEDIA_PLAY);
  expect(hid_special_key(HID_USAGE(0x01, 0x81)) == KEY_POWER);
  expect(hid_special_key(HID_USAGE(0x01, 0x82)) == KEY_SLEEP);
  expect(hid_special_key(HID_USAGE(0x01, 0x83)) == 0);
  expect(hid_special_key(HID_USAGE(0x0B, 0x2F)) == KEY_MIC_MUTE);
  expect(hid_special_key(HID_USAGE(0x0B, 0x2E)) == 0);
  expect(hid_special_key(HID_USAGE(0x07, 0xE9)) == 0);
  expect(hid_special_key(HID_USAGE(0x0C, 0x70)) == KEY_BRIGHT_DOWN);
}

static void typematic_eligibility(void)
{
  expect(hid_key_repeats(0x04) && hid_key_repeats(0x2C) && hid_key_repeats(0x4F) && hid_key_repeats(0x58));
  expect(!hid_key_repeats(0xE0) && !hid_key_repeats(0xE7));
  expect(!hid_key_repeats(0x39) && !hid_key_repeats(0x47) && !hid_key_repeats(0x53) && !hid_key_repeats(0x66));
  expect(hid_key_repeats(0x100 | KEY_VOL_UP) && hid_key_repeats(0x100 | KEY_VOL_DOWN));
  expect(hid_key_repeats(0x100 | KEY_BRIGHT_UP) && hid_key_repeats(0x100 | KEY_BRIGHT_DOWN));
  expect(!hid_key_repeats(0x100 | KEY_MUTE) && !hid_key_repeats(0x100 | KEY_POWER) &&
         !hid_key_repeats(0x100 | KEY_MEDIA_PLAY));
}

static void key_diff_ordering(void)
{
  struct events ev = {0};
  uint16_t none_keys[1] = {0};
  uint16_t cur[2] = {0x04, 0x05};
  hid_kbd_diff(0, 0x02, none_keys, 0, cur, 2, record, &ev);
  expect(ev.n == 3 && ev.key[0] == 0xE1 && ev.down[0] && ev.key[1] == 4 && ev.down[1] && ev.key[2] == 5);
  memset(&ev, 0, sizeof ev);
  uint16_t old[2] = {0x04, 0x05}, now[2] = {0x05, 0x06};
  hid_kbd_diff(0x02, 0x01, old, 2, now, 2, record, &ev);
  /* modifier press, key release, key press, modifier release */
  expect(ev.n == 4);
  expect(ev.key[0] == 0xE0 && ev.down[0]);
  expect(ev.key[1] == 4 && !ev.down[1]);
  expect(ev.key[2] == 6 && ev.down[2]);
  expect(ev.key[3] == 0xE1 && !ev.down[3]);
  memset(&ev, 0, sizeof ev);
  hid_kbd_diff(0x03, 0x03, now, 2, now, 2, record, &ev);
  expect(ev.n == 0);
}

static void typematic_state_machine(void)
{
  struct hid_typematic tm = {0};
  uint16_t key = 0;
  expect(!hid_tm_due(&tm, 1000, &key));
  hid_tm_press(&tm, 0x04, 1000);
  expect(!hid_tm_due(&tm, 1499, &key));
  expect(hid_tm_due(&tm, 1500, &key) && key == 4);
  expect(!hid_tm_due(&tm, 1500, &key));
  expect(!hid_tm_due(&tm, 1532, &key));
  expect(hid_tm_due(&tm, 1533, &key));
  expect(hid_tm_due(&tm, 1566, &key));
  hid_tm_press(&tm, 0x05, 1600);
  expect(!hid_tm_due(&tm, 2099, &key));
  expect(hid_tm_due(&tm, 2100, &key) && key == 5);
  hid_tm_release(&tm, 0x04);
  expect(hid_tm_due(&tm, 2133, &key) && key == 5);
  hid_tm_release(&tm, 0x05);
  expect(!hid_tm_due(&tm, 5000, &key));
  hid_tm_press(&tm, 0x06, 10000);
  expect(hid_tm_due(&tm, 12000, &key));
  expect(!hid_tm_due(&tm, 12000, &key));
  expect(!hid_tm_due(&tm, 12032, &key));
  expect(hid_tm_due(&tm, 12033, &key));
  hid_tm_press(&tm, 0x08, 0xFFFFFE00u);
  expect(hid_tm_due(&tm, 0xFFFFFFF8u, &key) && key == 8);
  expect(hid_tm_due(&tm, 0x00000015u, &key));
  hid_tm_release(&tm, 0x08);
  hid_tm_press(&tm, 0x07, 0xFFFFFF00u);
  expect(!hid_tm_due(&tm, 0xFFFFFFF0u, &key));
  expect(!hid_tm_due(&tm, 0x00000050u, &key));
  expect(hid_tm_due(&tm, 0x000000F4u, &key) && key == 7);
}

static void intr_interval_encoding(void)
{
  expect(usbh_intr_exponent(USB_SPEED_HIGH, 1) == 0);
  expect(usbh_intr_exponent(USB_SPEED_HIGH, 4) == 3);
  expect(usbh_intr_exponent(USB_SPEED_HIGH, 16) == 15);
  expect(usbh_intr_exponent(USB_SPEED_HIGH, 20) == 15);
  expect(usbh_intr_exponent(USB_SPEED_HIGH, 0) == 0);
  expect(usbh_intr_exponent(USB_SPEED_SUPER, 8) == 7);
  expect(usbh_intr_exponent(USB_SPEED_FULL, 1) == 3);
  expect(usbh_intr_exponent(USB_SPEED_FULL, 8) == 6);
  expect(usbh_intr_exponent(USB_SPEED_FULL, 10) == 6);
  expect(usbh_intr_exponent(USB_SPEED_LOW, 10) == 6);
  expect(usbh_intr_exponent(USB_SPEED_LOW, 4) == 5);
  expect(usbh_intr_exponent(USB_SPEED_FULL, 255) == 10);
  expect(usbh_intr_exponent(USB_SPEED_FULL, 0) == 3);
}

static void rumble_packing(void)
{
  struct hid_desc d;
  uint8_t id = 0, buf[16];
  expect_eq(hid_parse(rumble_pad, sizeof rumble_pad, &d), 0);
  expect_eq(hid_report_bytes(&d, HID_OUTPUT, 3), 6);
  expect_eq(hid_pack_rumble(&d, 65535, 32768, 250, buf, 6, &id), 3);
  expect_eq(id, 3);
  expect_eq(buf[0] & 0x0f, 0x0f);
  expect_eq(buf[1], 0);
  expect_eq(buf[2], 0);
  expect_eq(buf[3], 100);
  expect_eq(buf[4], 50);
  expect_eq(buf[5], 25);
  hid_desc_free(&d);
  expect_eq(hid_parse(qemu_kbd, sizeof qemu_kbd, &d), 0);
  expect_eq(hid_pack_rumble(&d, 1, 1, 1, buf, sizeof buf, &id), -1);
  hid_desc_free(&d);
}

int main(void)
{
  test("boot keyboard descriptor", boot_kbd_desc);
  test("QEMU usb-kbd descriptor", qemu_kbd_desc);
  test("keyboard report decoding", kbd_report_decoding);
  test("rollover error is phantom and carries no keys", rollover_phantom);
  test("boot mouse and wheel mouse", boot_and_wheel_mouse);
  test("tablet absolute axes", tablet_abs_axes);
  test("pen range and buttons", pen_range_and_buttons);
  test("absolute axis scaling", abs_axis_scaling);
  test("consumer and system controls with report IDs", consumer_report_ids);
  test("consumer keys as individual bits", consumer_bits_keys);
  test("horizontal wheel as AC Pan", horizontal_wheel_pan);
  test("array with a logical minimum above zero", array_logical_min);
  test("array values beyond the usage range are ignored", array_beyond_usage);
  test("four-way hat", four_way_hat);
  test("bitmap keyboard with LED report ID", bitmap_kbd_led_id);
  test("boot keyboard LED output packing", boot_kbd_led_packing);
  test("gamepad axes, hat and buttons across byte boundaries", gamepad_axes);
  test("dual-rumble output report packing", rumble_packing);
  test("touchscreen reports only its first finger", touchscreen_first_finger);
  test("bit extraction across byte boundaries", bit_extraction);
  test("descriptor edge cases", descriptor_edge_cases);
  test("report ID bit offsets are per report", report_id_bit_offsets);
  test("usage to PS/2 set-1 translation", usage_to_set1);
  test("laptop media F-keys are not synthesized from USB F5/F6/F8/F9", laptop_media_fkeys);
  test("special key mapping by page", special_key_mapping);
  test("typematic eligibility", typematic_eligibility);
  test("key diff ordering", key_diff_ordering);
  test("typematic state machine", typematic_state_machine);
  test("interrupt endpoint interval encoding", intr_interval_encoding);
  return 0;
}
