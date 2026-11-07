/* hid_kbd.h: pure keyboard state logic for USB HID: report-to-event diffing and software typematic. */
#ifndef OWOS_HID_KBD_H
#define OWOS_HID_KBD_H

#include <stdbool.h>
#include <stdint.h>

/* Key identities: keyboard-page usages 4..0xFF as is, special (KEY_*) codes as 0x100 | code. */
#define HID_KEY_SPECIAL 0x100

/* Modifier usages arrive as 0xE0..0xE7. */
typedef void (*hid_key_cb)(uint16_t key, bool down, void *ctx);

static inline bool hid_key_in(const uint16_t *list, uint32_t n, uint16_t key)
{
  for (uint32_t i = 0; i < n; i++)
    if (list[i] == key)
      return true;
  return false;
}

/* Emit the events that turn the previous pressed set into the current one. Order: modifier presses, key releases,
 * key presses (report order), modifier releases, so Shift+a in one report types a capital. */
static inline void hid_kbd_diff(uint8_t old_mods, uint8_t mods, const uint16_t *old, uint32_t nold, const uint16_t *cur,
                                uint32_t ncur, hid_key_cb cb, void *ctx)
{
  uint8_t pressed = mods & (uint8_t)~old_mods, released = old_mods & (uint8_t)~mods;
  for (int b = 0; b < 8; b++)
    if (pressed & (1u << b))
      cb((uint16_t)(0xE0 + b), true, ctx);
  for (uint32_t i = 0; i < nold; i++)
    if (!hid_key_in(cur, ncur, old[i]))
      cb(old[i], false, ctx);
  for (uint32_t i = 0; i < ncur; i++)
    if (!hid_key_in(old, nold, cur[i]))
      cb(cur[i], true, ctx);
  for (int b = 0; b < 8; b++)
    if (released & (1u << b))
      cb((uint16_t)(0xE0 + b), false, ctx);
}

#define HID_REPEAT_DELAY_MS 500
#define HID_REPEAT_PERIOD_MS 33

/* Software typematic: the last pressed key repeats after a delay, like the PS/2 keyboard controller does. */
struct hid_typematic {
  uint16_t key;
  bool active;
  uint32_t next;
};

static inline void hid_tm_press(struct hid_typematic *t, uint16_t key, uint32_t now_ms)
{
  t->key = key;
  t->active = true;
  t->next = now_ms + HID_REPEAT_DELAY_MS;
}

static inline void hid_tm_release(struct hid_typematic *t, uint16_t key)
{
  if (t->active && t->key == key)
    t->active = false;
}

/* True once per repeat that is due; catches up without bursting after a stall. */
static inline bool hid_tm_due(struct hid_typematic *t, uint32_t now_ms, uint16_t *key)
{
  if (!t->active || (int32_t)(now_ms - t->next) < 0)
    return false;
  *key = t->key;
  t->next += HID_REPEAT_PERIOD_MS;
  if ((int32_t)(now_ms - t->next) >= 0)
    t->next = now_ms + HID_REPEAT_PERIOD_MS;
  return true;
}

#endif /* OWOS_HID_KBD_H */
