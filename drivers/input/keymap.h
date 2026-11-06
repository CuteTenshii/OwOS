/* keymap.h: pure tables from PS/2 set-1 extended scancodes and USB HID usages to KEY_* codes (0 = no key). */
#ifndef OWOS_KEYMAP_H
#define OWOS_KEYMAP_H

#include "keyboard.h"
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

#endif /* OWOS_KEYMAP_H */
