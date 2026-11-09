/* hid_pad.c: see hid_pad.h. */

#include "hid_pad.h"
#include "crc32.h"
#include "hid_desc.h"

#if __STDC_HOSTED__
#include <string.h>
#else
#include "kstring.h"
#endif

#define VID_SONY 0x054C
#define VID_NINTENDO 0x057E
#define VID_MICROSOFT 0x045E
#define DS4_REPORT_COMMON 9 /* x y rx ry buttons[3] l2 r2 */

enum pad_kind pad_kind_usb(uint16_t vid, uint16_t pid)
{
  if (vid == VID_SONY && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0))
    return PAD_DS4;
  if (vid == VID_SONY && (pid == 0x0CE6 || pid == 0x0DF2))
    return PAD_DS5;
  if (vid == VID_MICROSOFT && (pid == 0x02D1 || pid == 0x02DD || pid == 0x02E3 || pid == 0x02EA || pid == 0x02FD ||
                               pid == 0x0B00 || pid == 0x0B12))
    return PAD_XBOX;
  if (vid == VID_NINTENDO && pid == 0x2009)
    return PAD_SWITCH_PRO;
  return PAD_GENERIC;
}

enum pad_kind pad_kind_name(const char *name)
{
  if (!strcmp(name, "Wireless Controller"))
    return PAD_DS4;
  if (!strcmp(name, "DualSense Wireless Controller") || !strcmp(name, "DualSense Edge Wireless Controller"))
    return PAD_DS5;
  if (!strcmp(name, "Pro Controller"))
    return PAD_SWITCH_PRO;
  return PAD_GENERIC;
}

static const struct {
  const char *token, *label;
} kind_names[] = {
    [PAD_GENERIC] = {"generic", "Gamepad"},
    [PAD_DS4] = {"ds4", "Wireless Controller"},
    [PAD_DS5] = {"ds5", "DualSense Wireless Controller"},
    [PAD_SWITCH_PRO] = {"switchpro", "Pro Controller"},
    [PAD_XBOX] = {"xbox", "Xbox Wireless Controller"},
};

const char *pad_kind_token(enum pad_kind k) { return kind_names[k].token; }
const char *pad_kind_label(enum pad_kind k) { return kind_names[k].label; }

static void add_bt_crc(uint8_t *buf, int len);

int ds4_pack_rumble(bool bt, uint16_t strong, uint16_t weak, uint8_t *buf)
{
  int len = bt ? DS4_BT_OUT_LEN : DS4_USB_OUT_LEN, motors = bt ? 6 : 4;
  memset(buf, 0, (size_t)len);
  if (bt) {
    buf[0] = DS4_BT_OUT_ID;
    buf[1] = 0xC4; /* HID + CRC, 4 ms poll interval */
    buf[3] = 0x01; /* update the rumble motors only */
  } else {
    buf[0] = DS4_USB_OUT_ID;
    buf[1] = 0x01;
  }
  buf[motors] = (uint8_t)(weak >> 8);
  buf[motors + 1] = (uint8_t)(strong >> 8);
  if (bt)
    add_bt_crc(buf, len);
  return len;
}

bool ds4_decode_bt(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6])
{
  if (len < 3 + DS4_REPORT_COMMON || data[0] != DS4_BT_IN_ID)
    return false;
  const uint8_t *c = data + 3;
  uint8_t dpad = c[4] & 0x0F;
  *hat = dpad < 8 ? (uint8_t)(dpad + 1) : 0;
  *buttons = (uint32_t)(c[4] >> 4) | (uint32_t)c[5] << 4 | (uint32_t)(c[6] & 0x03) << 12;
  axes[0] = hid_scale_i16(c[0], 0, 255);
  axes[1] = hid_scale_i16(c[1], 0, 255);
  axes[2] = hid_scale_i16(c[2], 0, 255);
  axes[5] = hid_scale_i16(c[3], 0, 255);
  axes[3] = hid_scale_i16(c[7], 0, 255);
  axes[4] = hid_scale_i16(c[8], 0, 255);
  return true;
}

static void add_bt_crc(uint8_t *buf, int len)
{
  static const uint8_t hdr = 0xA2;
  uint32_t crc = crc32(crc32(0, &hdr, 1), buf, (uint32_t)len - 4);
  for (int i = 0; i < 4; i++)
    buf[len - 4 + i] = (uint8_t)(crc >> (8 * i));
}

int ds5_pack_rumble(bool bt, uint8_t seq, uint16_t strong, uint16_t weak, uint8_t *buf)
{
  int len = bt ? DS5_BT_OUT_LEN : DS5_USB_OUT_LEN, common = bt ? 3 : 1;
  memset(buf, 0, (size_t)len);
  if (bt) {
    buf[0] = DS5_BT_OUT_ID;
    buf[1] = (uint8_t)(seq << 4);
    buf[2] = 0x10;
  } else {
    buf[0] = DS5_USB_OUT_ID;
  }
  buf[common] = 0x03; /* compatible vibration, haptics select */
  buf[common + 2] = (uint8_t)(weak >> 8);
  buf[common + 3] = (uint8_t)(strong >> 8);
  if (bt)
    add_bt_crc(buf, len);
  return len;
}

bool ds5_decode_bt(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6])
{
  if (len < 2 + 10 || data[0] != DS5_BT_IN_ID)
    return false;
  const uint8_t *c = data + 2; /* x y rx ry l2 r2 seq buttons[4] */
  uint8_t dpad = c[7] & 0x0F;
  *hat = dpad < 8 ? (uint8_t)(dpad + 1) : 0;
  *buttons = (uint32_t)(c[7] >> 4) | (uint32_t)c[8] << 4 | (uint32_t)(c[9] & 0x07) << 12;
  axes[0] = hid_scale_i16(c[0], 0, 255);
  axes[1] = hid_scale_i16(c[1], 0, 255);
  axes[2] = hid_scale_i16(c[2], 0, 255);
  axes[5] = hid_scale_i16(c[3], 0, 255);
  axes[3] = hid_scale_i16(c[4], 0, 255);
  axes[4] = hid_scale_i16(c[5], 0, 255);
  return true;
}

int xbox_pack_init(uint8_t seq, uint8_t *buf)
{
  static const uint8_t power_on[XBOX_INIT_LEN] = {0x05, 0x20, 0x00, 0x01, 0x00};
  memcpy(buf, power_on, sizeof power_on);
  buf[2] = seq;
  return XBOX_INIT_LEN;
}

int xbox_pack_rumble(uint8_t seq, uint16_t strong, uint16_t weak, uint8_t *buf)
{
  static const uint8_t base[XBOX_RUMBLE_LEN] = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0, 0, 0, 0, 0xFF, 0x00, 0xEB};
  memcpy(buf, base, sizeof base);
  buf[2] = seq;
  buf[8] = (uint8_t)(strong >> 9);
  buf[9] = (uint8_t)(weak >> 9);
  return XBOX_RUMBLE_LEN;
}

static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

bool xbox_decode(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6])
{
  static const uint8_t hats[16] = {0, 1, 5, 0, 7, 8, 6, 7, 3, 2, 4, 3, 0, 1, 5, 0};
  if (len < 18 || data[0] != 0x20)
    return false;
  uint8_t main = data[4], extra = data[5];
  *hat = hats[extra & 0x0F];
  *buttons = (uint32_t)(main >> 4 & 0x0F) | (uint32_t)(extra >> 4 & 0x03) << 4 | (uint32_t)(main >> 3 & 1) << 6 |
             (uint32_t)(main >> 2 & 1) << 7 | (uint32_t)(extra >> 6 & 0x03) << 8;
  axes[3] = hid_scale_i16(data[6] | (data[7] & 0x03) << 8, 0, 1023);
  axes[4] = hid_scale_i16(data[8] | (data[9] & 0x03) << 8, 0, 1023);
  axes[0] = le16s(data + 10);
  axes[1] = ~le16s(data + 12);
  axes[2] = le16s(data + 14);
  axes[5] = ~le16s(data + 16);
  return true;
}

bool xbox_decode_guide(const uint8_t *data, int len, bool *down)
{
  if (len < 5 || data[0] != 0x07)
    return false;
  *down = data[4] & 1;
  return true;
}

int switch_usb_step(int step, uint8_t *buf)
{
  static const uint8_t cmds[SWITCH_USB_STEPS] = {0x02, 0x03, 0x02, 0x04}; /* handshake, 3 Mbaud, handshake, HID only */
  if (step < 0 || step >= SWITCH_USB_STEPS)
    return 0;
  buf[0] = 0x80;
  buf[1] = cmds[step];
  return 2;
}
