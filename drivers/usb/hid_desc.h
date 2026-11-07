/* hid_desc.h: USB HID report descriptor parser and report decoder. Pure logic (no hardware access) so host tests can
 * link it. Fields are a growable table; offsets are in bits from the start of the report payload (after the report
 * ID byte when the descriptor uses report IDs). */
#ifndef OWOS_HID_DESC_H
#define OWOS_HID_DESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_INPUT 0
#define HID_OUTPUT 1
#define HID_FEATURE 2

/* Main item data bits. */
#define HID_F_CONST 0x01
#define HID_F_VAR 0x02
#define HID_F_REL 0x04

#define HID_PAGE_DESKTOP 0x01
#define HID_PAGE_SIM 0x02
#define HID_PAGE_KEYBOARD 0x07
#define HID_PAGE_LED 0x08
#define HID_PAGE_BUTTON 0x09
#define HID_PAGE_TELEPHONY 0x0B
#define HID_PAGE_CONSUMER 0x0C
#define HID_PAGE_DIGITIZER 0x0D

/* Consumer usage of the horizontal wheel. */
#define HID_AC_PAN 0x238

#define HID_USAGE(page, id) (((uint32_t)(page) << 16) | (id))

struct hid_field {
  uint32_t usage;     /* page << 16 | id; first usage of the range for array fields */
  uint32_t usage_max; /* last usage of an array field's range */
  int32_t lmin, lmax;
  int32_t pmin, pmax;
  uint32_t bit_off;
  uint32_t flags;
  uint32_t app;       /* usage of the enclosing top-level collection */
  uint32_t col_usage; /* usage of the innermost collection */
  uint16_t col;       /* serial of the innermost collection */
  uint16_t size;      /* bits per element */
  uint16_t count;     /* elements: 1 for variable fields, N for arrays */
  uint8_t kind;
  uint8_t report_id;
};

struct hid_report_info {
  uint32_t bits;
  uint8_t kind;
  uint8_t id;
};

struct hid_desc {
  struct hid_field *fields;
  size_t nfields, cap_fields;
  struct hid_report_info *reports;
  size_t nreports, cap_reports;
  bool has_ids;
};

/* Parse a report descriptor; 0 on success, -1 if truncated, empty or out of memory (out is then empty). */
int hid_parse(const uint8_t *d, size_t n, struct hid_desc *out);
void hid_desc_free(struct hid_desc *desc);

/* Payload bytes of a report (excluding the ID byte), 0 if there is none. */
uint32_t hid_report_bytes(const struct hid_desc *desc, uint8_t kind, uint8_t id);

/* Read/write an element of 1..32 bits at bit_off; reads past len yield 0, writes past len are dropped. */
int32_t hid_get_bits(const uint8_t *p, size_t len, uint32_t bit_off, unsigned size, bool is_signed);
void hid_set_bits(uint8_t *p, size_t len, uint32_t bit_off, unsigned size, uint32_t v);

/* Roles a descriptor can serve. */
#define HID_ROLE_KEYBOARD 0x01
#define HID_ROLE_MOUSE 0x02
#define HID_ROLE_TABLET 0x04 /* absolute pointer or digitizer */
#define HID_ROLE_GAMEPAD 0x08
#define HID_ROLE_CONSUMER 0x10
uint32_t hid_roles(const struct hid_desc *desc);

/* Gamepad state: axes in usage order X Y Z Rx Ry Rz Slider Dial. */
#define HID_PAD_AXES 8

struct hid_decoded {
  bool valid; /* the report ID is described by the descriptor */
  bool has_kbd, has_cons, has_rel, has_abs, has_buttons, has_touch, has_pad;
  bool phantom; /* keyboard array full of ErrorRollOver */
  uint8_t report_id;
  uint8_t mods;
  uint16_t *keys; /* pressed keyboard-page usages 4..255 in report order */
  uint32_t nkeys, keys_cap;
  uint32_t *cons; /* pressed consumer, telephony and system usages (page << 16 | id) */
  uint32_t ncons, cons_cap;
  uint32_t buttons; /* pointer buttons, bit n = button n + 1 */
  int32_t dx, dy, wheel, pan;
  int32_t ax, ay;
  int32_t ax_min, ax_max, ay_min, ay_max;
  bool tip; /* digitizer contact (tip switch, or touch) */
  bool in_range;
  uint32_t pad_buttons;
  int32_t axes[HID_PAD_AXES];
  int32_t axes_min[HID_PAD_AXES], axes_max[HID_PAD_AXES];
  uint8_t hat; /* 0 centered, 1 = up then clockwise to 8 */
  uint16_t touch_col;
  bool touch_col_set;
};

int hid_decoded_init(struct hid_decoded *d, const struct hid_desc *desc);
void hid_decoded_free(struct hid_decoded *d);
/* Decode one input report (with its ID byte when the descriptor has IDs) into d. */
void hid_decode(const struct hid_desc *desc, const uint8_t *data, size_t len, struct hid_decoded *d);

/* Fill the LED output report (HID LED page bit n = usage n + 1) into buf; returns the report ID or -1 if the
 * descriptor has no LED output. buf must be hid_report_bytes(OUTPUT, id) bytes (payload only). */
int hid_pack_leds(const struct hid_desc *desc, uint8_t leds, uint8_t *buf, size_t len, uint8_t *id_out);

/* Map [lo, hi] onto [0, bound], clamped; a degenerate range maps to 0. */
static inline int32_t hid_scale_abs(int32_t v, int32_t lo, int32_t hi, int32_t bound)
{
  if (hi <= lo)
    return 0;
  if (v < lo)
    v = lo;
  if (v > hi)
    v = hi;
  return (int32_t)(((int64_t)(v - lo) * bound) / ((int64_t)hi - lo));
}

/* Map [lo, hi] onto -32768..32767, clamped. */
static inline int32_t hid_scale_i16(int32_t v, int32_t lo, int32_t hi)
{
  if (hi <= lo)
    return 0;
  if (v < lo)
    v = lo;
  if (v > hi)
    v = hi;
  return (int32_t)(((int64_t)(v - lo) * 65535) / ((int64_t)hi - lo)) - 32768;
}

/* The report descriptors of the HID boot protocol (keyboard and 3-byte mouse). */
extern const uint8_t hid_boot_kbd_desc[63];
extern const uint8_t hid_boot_mouse_desc[50];

#endif /* OWOS_HID_DESC_H */
