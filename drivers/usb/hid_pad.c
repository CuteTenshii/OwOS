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
#define DS4_REPORT_COMMON 9 /* x y rx ry buttons[3] l2 r2 */

enum pad_kind pad_kind_usb(uint16_t vid, uint16_t pid)
{
  if (vid == VID_SONY && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0))
    return PAD_DS4;
  if (vid == VID_NINTENDO && pid == 0x2009)
    return PAD_SWITCH_PRO;
  return PAD_GENERIC;
}

enum pad_kind pad_kind_name(const char *name)
{
  if (!strcmp(name, "Wireless Controller"))
    return PAD_DS4;
  if (!strcmp(name, "Pro Controller"))
    return PAD_SWITCH_PRO;
  return PAD_GENERIC;
}

const char *pad_kind_token(enum pad_kind k)
{
  return k == PAD_DS4 ? "ds4" : k == PAD_SWITCH_PRO ? "switchpro" : "generic";
}

const char *pad_kind_label(enum pad_kind k)
{
  return k == PAD_DS4 ? "Wireless Controller" : k == PAD_SWITCH_PRO ? "Pro Controller" : "Gamepad";
}

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
  if (bt) {
    static const uint8_t hdr = 0xA2;
    uint32_t crc = crc32(crc32(0, &hdr, 1), buf, DS4_BT_OUT_LEN - 4);
    for (int i = 0; i < 4; i++)
      buf[DS4_BT_OUT_LEN - 4 + i] = (uint8_t)(crc >> (8 * i));
  }
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

int switch_usb_step(int step, uint8_t *buf)
{
  static const uint8_t cmds[SWITCH_USB_STEPS] = {0x02, 0x03, 0x02, 0x04}; /* handshake, 3 Mbaud, handshake, HID only */
  if (step < 0 || step >= SWITCH_USB_STEPS)
    return 0;
  buf[0] = 0x80;
  buf[1] = cmds[step];
  return 2;
}
