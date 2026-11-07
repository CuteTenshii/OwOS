/* keymap.h: pure tables from PS/2 set-1 extended scancodes and USB HID usages to KEY_* codes (0 = no key). */
#ifndef OWOS_KEYMAP_H
#define OWOS_KEYMAP_H

#include "keyboard.h"
#include <stdbool.h>
#include <stdint.h>

/* Byte after a 0xE0 prefix. 0x6B/0x6C are My Computer/Mail on the AT standard but brightness on the laptops we
 * target, so they map to brightness and KEY_LAUNCH_FILES/KEY_LAUNCH_MAIL are unreachable over PS/2. */
static inline uint8_t kbd_e0_key(uint8_t sc)
{
  switch (sc) {
  case 0x10:
    return KEY_MEDIA_PREV;
  case 0x19:
    return KEY_MEDIA_NEXT;
  case 0x20:
    return KEY_MUTE;
  case 0x21:
    return KEY_LAUNCH_CALC;
  case 0x22:
    return KEY_MEDIA_PLAY;
  case 0x24:
    return KEY_MEDIA_STOP;
  case 0x2E:
    return KEY_VOL_DOWN;
  case 0x30:
    return KEY_VOL_UP;
  case 0x32:
    return KEY_LAUNCH_BROWSER;
  case 0x5E:
    return KEY_POWER;
  case 0x5F:
    return KEY_SLEEP;
  case 0x65:
    return KEY_SEARCH;
  case 0x67:
    return KEY_WWW_REFRESH;
  case 0x69:
    return KEY_WWW_FORWARD;
  case 0x6A:
    return KEY_WWW_BACK;
  case 0x6B:
    return KEY_BRIGHT_DOWN;
  case 0x6C:
    return KEY_BRIGHT_UP;
  }
  return 0;
}

/* Keyboard usage page (0x07) keys outside the typing block. */
static inline uint8_t hid_kbd_key(uint8_t usage) { return usage == 0x66 ? KEY_POWER : 0; }

/* Generic Desktop (0x01) system controls. */
static inline uint8_t hid_system_key(uint8_t usage)
{
  switch (usage) {
  case 0x81:
    return KEY_POWER;
  case 0x82:
    return KEY_SLEEP;
  }
  return 0;
}

/* Consumer (0x0C) usages. */
static inline uint8_t hid_consumer_key(uint16_t usage)
{
  switch (usage) {
  case 0x6F:
    return KEY_BRIGHT_UP;
  case 0x70:
    return KEY_BRIGHT_DOWN;
  case 0xB5:
    return KEY_MEDIA_NEXT;
  case 0xB6:
    return KEY_MEDIA_PREV;
  case 0xB7:
    return KEY_MEDIA_STOP;
  case 0xCD:
    return KEY_MEDIA_PLAY;
  case 0xE2:
    return KEY_MUTE;
  case 0xE9:
    return KEY_VOL_UP;
  case 0xEA:
    return KEY_VOL_DOWN;
  case 0x18A:
    return KEY_LAUNCH_MAIL;
  case 0x192:
    return KEY_LAUNCH_CALC;
  case 0x194:
    return KEY_LAUNCH_FILES;
  case 0x196:
  case 0x223:
    return KEY_LAUNCH_BROWSER;
  case 0x221:
    return KEY_SEARCH;
  case 0x224:
    return KEY_WWW_BACK;
  case 0x225:
    return KEY_WWW_FORWARD;
  case 0x227:
    return KEY_WWW_REFRESH;
  }
  return 0;
}

/* Telephony (0x0B) usages. */
static inline uint8_t hid_telephony_key(uint16_t usage) { return usage == 0x2F ? KEY_MIC_MUTE : 0; }

/* KEY_* for a special usage given as page << 16 | id (Consumer, Telephony, System Control). */
static inline uint8_t hid_special_key(uint32_t usage)
{
  uint16_t id = usage & 0xFFFF;
  switch (usage >> 16) {
  case 0x01:
    return hid_system_key((uint8_t)id);
  case 0x0B:
    return hid_telephony_key(id);
  case 0x0C:
    return hid_consumer_key(id);
  }
  return 0;
}

/* Keyboard-page F5/F6/F8/F9 are plain function keys over USB; their set-1 codes are laptop media keys. */
static inline uint8_t hid_fkey_key(uint8_t usage)
{
  switch (usage) {
  case 0x3E:
    return KEY_F5;
  case 0x3F:
    return KEY_F6;
  case 0x41:
    return KEY_F8;
  case 0x42:
    return KEY_F9;
  }
  return 0;
}

/* Typematic keys: not modifiers, locks or power; of the specials only volume and brightness (0x100 | KEY_*). */
static inline bool hid_key_repeats(uint16_t key)
{
  if (key >= 0x100)
    return key == (0x100 | KEY_VOL_UP) || key == (0x100 | KEY_VOL_DOWN) || key == (0x100 | KEY_BRIGHT_UP) ||
           key == (0x100 | KEY_BRIGHT_DOWN);
  return !(key >= 0xE0 && key <= 0xE7) && key != 0x39 && key != 0x47 && key != 0x53 && key != 0x66;
}

/* Keyboard-page usage to set-1 make code, 0x100 added for the 0xE0 prefix; 0 = none. Break = make | 0x80. */
static inline uint16_t hid_usage_set1(uint8_t usage)
{
  static const uint8_t main_block[0x40] = {
      0,    0,    0,    0,    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, /* 0x00 */
      0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x02, 0x03, /* 0x10 */
      0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, /* 0x20 */
      0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x00, 0x00, /* 0x30 */
  };
  if (usage < 0x40)
    return main_block[usage];
  switch (usage) {
  case 0x40: /* F7 */
    return 0x41;
  case 0x43: /* F10 */
    return 0x44;
  case 0x44: /* F11 */
    return 0x57;
  case 0x45: /* F12 */
    return 0x58;
  case 0x46: /* Print Screen */
    return 0x137;
  case 0x47: /* Scroll Lock */
    return 0x46;
  case 0x49: /* Insert */
    return 0x152;
  case 0x4A: /* Home */
    return 0x147;
  case 0x4B: /* Page Up */
    return 0x149;
  case 0x4C: /* Delete */
    return 0x153;
  case 0x4D: /* End */
    return 0x14F;
  case 0x4E: /* Page Down */
    return 0x151;
  case 0x4F: /* Right */
    return 0x14D;
  case 0x50: /* Left */
    return 0x14B;
  case 0x51: /* Down */
    return 0x150;
  case 0x52: /* Up */
    return 0x148;
  case 0x53: /* Num Lock */
    return 0x45;
  case 0x54: /* keypad / */
    return 0x135;
  case 0x55: /* keypad * */
    return 0x37;
  case 0x56: /* keypad - */
    return 0x4A;
  case 0x57: /* keypad + */
    return 0x4E;
  case 0x58: /* keypad Enter */
    return 0x11C;
  case 0x59: /* keypad 1..9, 0, . */
    return 0x4F;
  case 0x5A:
    return 0x50;
  case 0x5B:
    return 0x51;
  case 0x5C:
    return 0x4B;
  case 0x5D:
    return 0x4C;
  case 0x5E:
    return 0x4D;
  case 0x5F:
    return 0x47;
  case 0x60:
    return 0x48;
  case 0x61:
    return 0x49;
  case 0x62:
    return 0x52;
  case 0x63:
    return 0x53;
  case 0x64: /* ISO 102nd key */
    return 0x56;
  case 0xE0: /* modifiers */
    return 0x1D;
  case 0xE1:
    return 0x2A;
  case 0xE2:
    return 0x38;
  case 0xE3:
    return 0x15B;
  case 0xE4:
    return 0x11D;
  case 0xE5:
    return 0x36;
  case 0xE6:
    return 0x138;
  case 0xE7:
    return 0x15C;
  }
  return 0;
}

#endif /* OWOS_KEYMAP_H */
