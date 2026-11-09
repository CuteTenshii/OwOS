/* hid_pad_test.c: controller identification, DualShock 4 rumble and Bluetooth reports, Switch Pro USB start-up
 * (drivers/usb/hid_pad.c). */
#define OWOS_SUITE_TEST
#include "../drivers/usb/hid_pad.h"
#include "test.h"
#include "zlib/crc32.h"
#include <string.h>

static void identification(void)
{
  expect_eq(pad_kind_usb(0x054C, 0x05C4), PAD_DS4);
  expect_eq(pad_kind_usb(0x054C, 0x09CC), PAD_DS4);
  expect_eq(pad_kind_usb(0x057E, 0x2009), PAD_SWITCH_PRO);
  expect_eq(pad_kind_usb(0x057E, 0x2006), PAD_GENERIC);
  expect_eq(pad_kind_usb(0x045E, 0x02FF), PAD_GENERIC);
  expect_eq(pad_kind_usb(0x054C, 0x0CE6), PAD_DS5);
  expect_eq(pad_kind_usb(0x054C, 0x0DF2), PAD_DS5);
  expect_eq(pad_kind_usb(0x045E, 0x0B12), PAD_XBOX);
  expect_eq(pad_kind_usb(0x045E, 0x02EA), PAD_XBOX);
  expect_eq(pad_kind_name("DualSense Wireless Controller"), PAD_DS5);
  expect_eq(pad_kind_name("DualSense Edge Wireless Controller"), PAD_DS5);
  expect_eq(pad_kind_name("Wireless Controller"), PAD_DS4);
  expect_eq(pad_kind_name("Pro Controller"), PAD_SWITCH_PRO);
  expect_eq(pad_kind_name("Joy-Con (L)"), PAD_GENERIC);
  expect_eq(pad_kind_name(""), PAD_GENERIC);
  expect_string(pad_kind_token(PAD_DS4), "ds4");
  expect_string(pad_kind_token(PAD_SWITCH_PRO), "switchpro");
}

static void ds4_rumble_usb(void)
{
  uint8_t b[DS4_BT_OUT_LEN];
  expect_eq(ds4_pack_rumble(false, 0xFFFF, 0x8000, b), DS4_USB_OUT_LEN);
  expect_eq(b[0], 0x05);
  expect_eq(b[1], 0x01);
  expect_eq(b[4], 0x80);
  expect_eq(b[5], 0xFF);
  for (int i = 6; i < DS4_USB_OUT_LEN; i++)
    expect_eq(b[i], 0);
}

static void ds4_rumble_bluetooth(void)
{
  uint8_t b[DS4_BT_OUT_LEN];
  expect_eq(ds4_pack_rumble(true, 0x4000, 0xFF00, b), DS4_BT_OUT_LEN);
  expect_eq(b[0], 0x11);
  expect_eq(b[6], 0xFF);
  expect_eq(b[7], 0x40);
  uint8_t seed = 0xA2;
  uint32_t crc = crc32(crc32(0, &seed, 1), b, DS4_BT_OUT_LEN - 4);
  expect_eq(b[74], (uint8_t)crc);
  expect_eq(b[75], (uint8_t)(crc >> 8));
  expect_eq(b[76], (uint8_t)(crc >> 16));
  expect_eq(b[77], (uint8_t)(crc >> 24));
  ds4_pack_rumble(true, 0, 0, b);
  expect_eq(b[6], 0);
  expect_eq(b[7], 0);
}

static void ds4_bluetooth_input(void)
{
  uint8_t r[78] = {0x11, 0xC0, 0x00};
  uint32_t buttons;
  uint8_t hat;
  int32_t axes[6];
  /* sticks, then: hat 2 (east) with Cross, L1 and R3, PS, L2 and R2 analog */
  const uint8_t common[] = {0, 255, 128, 64, 0x22 | 0x20, 0x01 | 0x80, 0x01, 255, 0};
  memcpy(r + 3, common, sizeof common);
  expect(ds4_decode_bt(r, sizeof r, &buttons, &hat, axes));
  expect_eq(hat, 3);
  expect_eq(buttons, (1u << 1) | (1u << 4) | (1u << 11) | (1u << 12));
  expect_eq(axes[0], -32768);
  expect_eq(axes[1], 32767);
  expect(axes[2] > -300 && axes[2] < 300);
  expect(axes[5] > -17000 && axes[5] < -16000);
  expect_eq(axes[3], 32767);
  expect_eq(axes[4], -32768);
  r[7] = 8;
  expect(ds4_decode_bt(r, sizeof r, &buttons, &hat, axes));
  expect_eq(hat, 0);
  expect(!ds4_decode_bt(r, 8, &buttons, &hat, axes));
  r[0] = 0x01;
  expect(!ds4_decode_bt(r, sizeof r, &buttons, &hat, axes));
}

static void ds5_rumble(void)
{
  uint8_t b[DS5_BT_OUT_LEN];
  expect_eq(ds5_pack_rumble(false, 0, 0xFFFF, 0x8000, b), DS5_USB_OUT_LEN);
  expect_eq(b[0], 0x02);
  expect_eq(b[1], 0x03);
  expect_eq(b[3], 0x80);
  expect_eq(b[4], 0xFF);
  expect_eq(b[5], 0);

  expect_eq(ds5_pack_rumble(true, 5, 0x4000, 0xFF00, b), DS5_BT_OUT_LEN);
  expect_eq(b[0], 0x31);
  expect_eq(b[1], 0x50);
  expect_eq(b[2], 0x10);
  expect_eq(b[3], 0x03);
  expect_eq(b[5], 0xFF);
  expect_eq(b[6], 0x40);
  uint8_t seed = 0xA2;
  uint32_t crc = crc32(crc32(0, &seed, 1), b, DS5_BT_OUT_LEN - 4);
  expect_eq(b[74], (uint8_t)crc);
  expect_eq(b[77], (uint8_t)(crc >> 24));
}

static void ds5_bluetooth_input(void)
{
  uint8_t r[78] = {0x31, 0x01};
  uint32_t buttons;
  uint8_t hat;
  int32_t axes[6];
  /* x y rx ry l2 r2 seq, then hat 4 (south-east) with Triangle, R1 and Options, mute */
  const uint8_t common[] = {128, 255, 0, 128, 255, 0, 9, 0x83, 0x02 | 0x20, 0x04};
  memcpy(r + 2, common, sizeof common);
  expect(ds5_decode_bt(r, sizeof r, &buttons, &hat, axes));
  expect_eq(hat, 4);
  expect_eq(buttons, (1u << 3) | (1u << 5) | (1u << 9) | (1u << 14));
  expect(axes[0] > -300 && axes[0] < 300);
  expect_eq(axes[1], 32767);
  expect_eq(axes[2], -32768);
  expect_eq(axes[3], 32767);
  expect_eq(axes[4], -32768);
  expect(!ds5_decode_bt(r, 11, &buttons, &hat, axes));
  r[0] = 0x01;
  expect(!ds5_decode_bt(r, sizeof r, &buttons, &hat, axes));
}

static void xbox_input(void)
{
  uint8_t r[18] = {0x20, 0x00, 0x01, 0x0E};
  uint32_t buttons;
  uint8_t hat;
  int32_t axes[6];
  /* A and Menu, d-pad up+right, LB, RB and L3; LT full, RT 512; sticks */
  r[4] = 0x10 | 0x04;
  r[5] = 0x01 | 0x08 | 0x10 | 0x20 | 0x40;
  r[6] = 0xFF;
  r[7] = 0x03;
  r[8] = 0x00;
  r[9] = 0x02;
  r[10] = 0x00;
  r[11] = 0x80; /* LX -32768 */
  r[12] = 0xFF;
  r[13] = 0x7F; /* LY +32767: stick up */
  r[14] = 0x00;
  r[15] = 0x00; /* RX 0 */
  r[16] = 0x00;
  r[17] = 0x80; /* RY -32768: stick down */
  expect(xbox_decode(r, sizeof r, &buttons, &hat, axes));
  expect_eq(hat, 2);
  expect_eq(buttons, (1u << 0) | (1u << 7) | (1u << 4) | (1u << 5) | (1u << 8));
  expect_eq(axes[3], 32767);
  expect(axes[4] > -100 && axes[4] < 100);
  expect_eq(axes[0], -32768);
  expect_eq(axes[1], -32768);
  expect_eq(axes[2], 0);
  expect_eq(axes[5], 32767);
  r[4] = 0x20 | 0x08;
  r[5] = 0x02;
  expect(xbox_decode(r, sizeof r, &buttons, &hat, axes));
  expect_eq(hat, 5);
  expect_eq(buttons, (1u << 1) | (1u << 6));
  expect(!xbox_decode(r, 17, &buttons, &hat, axes));
  r[0] = 0x03;
  expect(!xbox_decode(r, sizeof r, &buttons, &hat, axes));

  bool down = false;
  const uint8_t guide[] = {0x07, 0x20, 0x00, 0x02, 0x01, 0x5B};
  expect(xbox_decode_guide(guide, sizeof guide, &down) && down);
  const uint8_t released[] = {0x07, 0x20, 0x00, 0x02, 0x00, 0x5B};
  expect(xbox_decode_guide(released, sizeof released, &down) && !down);
  expect(!xbox_decode_guide(r, sizeof r, &down));
}

static void xbox_output(void)
{
  uint8_t b[XBOX_RUMBLE_LEN];
  expect_eq(xbox_pack_init(7, b), XBOX_INIT_LEN);
  expect(!memcmp(b, (uint8_t[]){0x05, 0x20, 0x07, 0x01, 0x00}, 5));
  expect_eq(xbox_pack_rumble(3, 0xFFFF, 0x4000, b), XBOX_RUMBLE_LEN);
  expect(!memcmp(b, (uint8_t[]){0x09, 0x00, 0x03, 0x09, 0x00, 0x0F, 0, 0, 0x7F, 0x20, 0xFF, 0x00, 0xEB}, 13));
}

static void switch_usb_start(void)
{
  uint8_t b[2];
  static const uint8_t want[SWITCH_USB_STEPS] = {0x02, 0x03, 0x02, 0x04};
  for (int i = 0; i < SWITCH_USB_STEPS; i++) {
    expect_eq(switch_usb_step(i, b), 2);
    expect_eq(b[0], 0x80);
    expect_eq(b[1], want[i]);
  }
  expect_eq(switch_usb_step(SWITCH_USB_STEPS, b), 0);
}

int main(void)
{
  test("controller identification", identification);
  test("DualShock 4 USB rumble report", ds4_rumble_usb);
  test("DualShock 4 Bluetooth rumble report carries its CRC", ds4_rumble_bluetooth);
  test("DualShock 4 Bluetooth input report", ds4_bluetooth_input);
  test("DualSense rumble reports", ds5_rumble);
  test("DualSense Bluetooth input report", ds5_bluetooth_input);
  test("Xbox GIP input packets", xbox_input);
  test("Xbox GIP init and rumble packets", xbox_output);
  test("Switch Pro USB start-up sequence", switch_usb_start);
  return 0;
}
