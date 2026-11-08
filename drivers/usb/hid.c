/* hid.c: USB HID class driver. Binds class-03 interfaces, parses their report descriptors (hid_desc.c) and feeds the
 * decoded reports to the keyboard (as PS/2 set-1 codes), mouse and gamepad state. */

#include "hid.h"
#include "console.h"
#include "heap.h"
#include "hid_desc.h"
#include "hid_kbd.h"
#include "keyboard.h"
#include "keymap.h"
#include "kstring.h"
#include "mouse.h"
#include "timer.h"
#include "usbhost.h"
#include <stdbool.h>
#include <stdint.h>

#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

struct hid_dev {
  struct hid_dev *next;
  int slot, iface;
  uint8_t subclass, protocol;
  bool boot;
  uint32_t roles;
  struct hid_desc desc;
  struct hid_decoded dec;
  uint8_t *raw;
  int raw_len;
  /* Pressed-key state carried between reports. */
  uint8_t mods;
  uint16_t *keys;
  uint32_t nkeys;
  uint16_t *cons_prev, *cons_cur;
  uint32_t ncons;
  uint32_t buttons;
  bool abs_valid;
  int abs_x, abs_y;
  /* LED output report. */
  bool has_led;
  uint8_t led_id;
  uint32_t led_len;
  uint32_t led_seq;
};

static struct hid_dev *hids;
static struct hid_typematic typematic;
static uint32_t pad_seq;
static uint32_t pad_buttons;
static int32_t pad_axes[HID_PAD_AXES];
static uint8_t pad_hat;
static int pad_devices;

static uint32_t now_ms(void) { return timer_ticks() * (1000 / TIMER_HZ); }

/* Deliver one key edge (a typematic repeat is another press). */
static void emit_key(uint16_t key, bool down)
{
  if (key >= HID_KEY_SPECIAL) {
    uint8_t code = (uint8_t)key;
    if (down)
      keyboard_special_key(code);
    return;
  }
  uint8_t usage = (uint8_t)key;
  uint8_t special = hid_kbd_key(usage);
  if (!special)
    special = hid_fkey_key(usage);
  if (special) {
    if (down) {
      if (special == KEY_POWER)
        keyboard_special_key(special);
      else
        keyboard_inject_key(special);
    }
    return;
  }
  uint16_t code = hid_usage_set1(usage);
  if (!code)
    return;
  keyboard_inject_scancode(down ? code : (uint16_t)((code & 0x100) | (code & 0xFF) | 0x80));
}

static void key_event(uint16_t key, bool down, void *ctx)
{
  (void)ctx;
  emit_key(key, down);
  if (down && hid_key_repeats(key))
    hid_tm_press(&typematic, key, now_ms());
  else if (!down)
    hid_tm_release(&typematic, key);
}

static void kbd_report(struct hid_dev *h, const struct hid_decoded *d)
{
  if (d->phantom)
    return;
  hid_kbd_diff(h->mods, d->mods, h->keys, h->nkeys, d->keys, d->nkeys, key_event, h);
  h->mods = d->mods;
  h->nkeys = d->nkeys;
  for (uint32_t i = 0; i < d->nkeys; i++)
    h->keys[i] = d->keys[i];
}

static void cons_report(struct hid_dev *h, const struct hid_decoded *d)
{
  uint32_t n = 0;
  for (uint32_t i = 0; i < d->ncons; i++) {
    uint8_t code = hid_special_key(d->cons[i]);
    if (code)
      h->cons_cur[n++] = (uint16_t)(HID_KEY_SPECIAL | code);
  }
  hid_kbd_diff(0, 0, h->cons_prev, h->ncons, h->cons_cur, n, key_event, h);
  for (uint32_t i = 0; i < n; i++)
    h->cons_prev[i] = h->cons_cur[i];
  h->ncons = n;
}

static uint32_t total_buttons(void)
{
  uint32_t b = 0;
  for (struct hid_dev *h = hids; h; h = h->next)
    b |= h->buttons;
  return b;
}

static void pointer_report(struct hid_dev *h, const struct hid_decoded *d)
{
  uint32_t btn = d->buttons | (d->tip ? 1u : 0);
  bool moved = d->dx || d->dy || d->wheel || d->pan;
  if (d->has_abs) {
    int x = h->abs_x, y = h->abs_y;
    if (!d->has_range || d->in_range) {
      x = hid_scale_abs(d->ax, d->ax_min, d->ax_max, mouse_bound_x());
      y = hid_scale_abs(d->ay, d->ay_min, d->ay_max, mouse_bound_y());
    } else if (!h->abs_valid) {
      return;
    }
    if (h->abs_valid && x == h->abs_x && y == h->abs_y && !moved && btn == h->buttons)
      return;
    h->abs_valid = true;
    h->abs_x = x;
    h->abs_y = y;
    h->buttons = btn;
    mouse_inject_abs(x, y, d->wheel, d->pan, (int)total_buttons());
    return;
  }
  if (!moved && btn == h->buttons)
    return;
  h->buttons = btn;
  mouse_inject_rel(d->dx, d->dy, d->wheel, d->pan, (int)total_buttons());
}

static void pad_report(const struct hid_decoded *d)
{
  pad_buttons = d->pad_buttons;
  pad_hat = d->hat;
  for (int i = 0; i < HID_PAD_AXES; i++)
    pad_axes[i] = hid_scale_i16(d->axes[i], d->axes_min[i], d->axes_max[i]);
  pad_seq++;
}

static void hid_report(void *ctx, const uint8_t *data, int len)
{
  struct hid_dev *h = ctx;
  if (len <= 0)
    return;
  hid_decode(&h->desc, data, (size_t)len, &h->dec);
  const struct hid_decoded *d = &h->dec;
  if (!d->valid)
    return;
  if (d->has_kbd)
    kbd_report(h, d);
  if (d->has_cons)
    cons_report(h, d);
  if (d->has_rel || d->has_abs || d->has_buttons || d->has_touch)
    pointer_report(h, d);
  if (d->has_pad)
    pad_report(d);
}

/* Push the Num/Caps state to a keyboard's LED output report over EP0. */
static void send_leds(struct hid_dev *h)
{
  uint32_t n = h->led_len + (h->led_id ? 1 : 0);
  uint8_t *buf = kmalloc(n);
  if (!buf)
    return;
  memset(buf, 0, n);
  uint8_t id;
  uint8_t *payload = buf + (h->led_id ? 1 : 0);
  if (hid_pack_leds(&h->desc, keyboard_leds(), payload, h->led_len, &id) >= 0) {
    if (h->led_id)
      buf[0] = h->led_id;
    usbh_control_out(h->slot, 0x21, HID_REQ_SET_REPORT, (uint16_t)((2u << 8) | h->led_id), (uint16_t)h->iface, buf,
                     (int)n);
  }
  kfree(buf);
}

void hid_tick(void)
{
  uint16_t key;
  if (hid_tm_due(&typematic, now_ms(), &key))
    emit_key(key, true);
  uint32_t seq = keyboard_led_seq();
  for (struct hid_dev *h = hids; h; h = h->next) {
    if (h->has_led && h->led_seq != seq) {
      h->led_seq = seq;
      send_leds(h);
    }
  }
}

static void free_hid(struct hid_dev *h)
{
  hid_decoded_free(&h->dec);
  hid_desc_free(&h->desc);
  kfree(h->raw);
  kfree(h->keys);
  kfree(h->cons_prev);
  kfree(h->cons_cur);
  kfree(h);
}

static bool alloc_state(struct hid_dev *h)
{
  if (hid_decoded_init(&h->dec, &h->desc) != 0)
    return false;
  if (h->dec.keys_cap)
    h->keys = kmalloc(h->dec.keys_cap * sizeof *h->keys);
  if (h->dec.cons_cap) {
    h->cons_prev = kmalloc(h->dec.cons_cap * sizeof *h->cons_prev);
    h->cons_cur = kmalloc(h->dec.cons_cap * sizeof *h->cons_cur);
  }
  return (!h->dec.keys_cap || h->keys) && (!h->dec.cons_cap || (h->cons_prev && h->cons_cur));
}

static void print_roles(uint32_t roles)
{
  const char *sep = "";
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {{HID_ROLE_KEYBOARD, "keyboard"},
               {HID_ROLE_MOUSE, "mouse"},
               {HID_ROLE_TABLET, "tablet"},
               {HID_ROLE_GAMEPAD, "gamepad"},
               {HID_ROLE_CONSUMER, "consumer"}};
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
    if (roles & names[i].bit) {
      con_printf("%s%s", sep, names[i].name);
      sep = ",";
    }
}

/* Bind one interface: fetch and parse its report descriptor (boot descriptor as fallback), start its interrupt pipe. */
static void bind_iface(struct usbdev *d, int iface, uint8_t subclass, uint8_t protocol, int rd_len, uint8_t ep,
                       uint16_t mps, uint8_t interval)
{
  struct hid_dev *h = kmalloc(sizeof *h);
  if (!h)
    return;
  memset(h, 0, sizeof *h);
  h->slot = d->slot;
  h->iface = iface;
  h->subclass = subclass;
  h->protocol = protocol;

  bool parsed = false;
  if (rd_len > 0 && rd_len <= 4096) {
    h->raw = kmalloc((uint32_t)rd_len);
    if (h->raw && usbh_control_in(d->slot, 0x81, 0x06, 0x2200, (uint16_t)iface, h->raw, rd_len) == rd_len) {
      h->raw_len = rd_len;
      parsed = hid_parse(h->raw, (size_t)rd_len, &h->desc) == 0 && hid_roles(&h->desc) != 0;
      if (!parsed)
        hid_desc_free(&h->desc);
    }
  }
  if (!parsed) {
    if (subclass != 1 || (protocol != 1 && protocol != 2)) {
      free_hid(h);
      return;
    }
    const uint8_t *bd = protocol == 1 ? hid_boot_kbd_desc : hid_boot_mouse_desc;
    size_t bl = protocol == 1 ? sizeof hid_boot_kbd_desc : sizeof hid_boot_mouse_desc;
    if (hid_parse(bd, bl, &h->desc) != 0) {
      free_hid(h);
      return;
    }
    h->boot = true;
    usbh_control_nodata(d->slot, 0x21, HID_REQ_SET_PROTOCOL, 0, (uint16_t)iface);
  }
  h->roles = hid_roles(&h->desc);
  if (!alloc_state(h)) {
    free_hid(h);
    return;
  }

  for (size_t i = 0; i < h->desc.nfields && !h->has_led; i++) {
    const struct hid_field *f = &h->desc.fields[i];
    uint32_t n = f->usage & 0xFFFF;
    if (f->kind == HID_OUTPUT && (f->usage >> 16) == HID_PAGE_LED && n >= 1 && n <= 8) {
      h->has_led = true;
      h->led_id = f->report_id;
      h->led_len = hid_report_bytes(&h->desc, HID_OUTPUT, f->report_id);
    }
  }

  usbh_control_nodata(d->slot, 0x21, HID_REQ_SET_IDLE, 0, (uint16_t)iface);
  if (!usbh_open_intr(d->slot, ep, mps, interval, hid_report, h)) {
    free_hid(h);
    return;
  }
  con_write("hid: ");
  print_roles(h->roles);
  con_printf(" on slot %d if %d (%s protocol)\n", d->slot, iface, h->boot ? "boot" : "report");
  h->next = hids;
  hids = h;
  d->bound |= USBH_BOUND_HID;
  if (h->roles & HID_ROLE_GAMEPAD)
    pad_devices++;
  if (h->has_led)
    h->led_seq = keyboard_led_seq() - 1;
}

void hid_bind(struct usbdev *d)
{
  const uint8_t *p = d->cfg;
  int len = d->cfg_len;
  for (int i = 0; i + 8 < len;) {
    uint8_t l = p[i], t = p[i + 1];
    if (l < 2)
      break;
    if (t == 0x04 && l >= 9 && p[i + 5] == 0x03 && p[i + 3] == 0) {
      int iface = p[i + 2];
      uint8_t subclass = p[i + 6], protocol = p[i + 7];
      int rd_len = 0, j = i + l;
      uint8_t ep = 0, interval = 0;
      uint16_t mps = 0;
      for (; j + 1 < len && p[j + 1] != 0x04;) {
        uint8_t jl = p[j];
        if (jl < 2)
          break;
        if (p[j + 1] == 0x21 && jl >= 9 && j + 8 < len) {
          for (int k = 0; k < p[j + 5] && j + 9 + k * 3 <= len && 6 + k * 3 + 2 < jl; k++)
            if (p[j + 6 + k * 3] == 0x22) {
              rd_len = p[j + 7 + k * 3] | (p[j + 8 + k * 3] << 8);
              break;
            }
        } else if (p[j + 1] == 0x05 && jl >= 7 && j + 6 < len && (p[j + 3] & 3) == 3 && (p[j + 2] & 0x80) && !ep) {
          ep = p[j + 2];
          mps = (uint16_t)((p[j + 4] | (p[j + 5] << 8)) & 0x7FF);
          interval = p[j + 6];
        }
        j += jl;
      }
      if (ep)
        bind_iface(d, iface, subclass, protocol, rd_len, ep, mps, interval);
      i = j;
      continue;
    }
    i += l;
  }
}

void hid_unbind(int slot)
{
  for (struct hid_dev **pp = &hids; *pp;) {
    struct hid_dev *h = *pp;
    if (h->slot != slot) {
      pp = &h->next;
      continue;
    }
    *pp = h->next;
    hid_kbd_diff(h->mods, 0, h->keys, h->nkeys, 0, 0, key_event, h);
    hid_kbd_diff(0, 0, h->cons_prev, h->ncons, 0, 0, key_event, h);
    if (h->buttons) {
      h->buttons = 0;
      mouse_inject_rel(0, 0, 0, 0, (int)total_buttons());
    }
    if (h->roles & HID_ROLE_GAMEPAD) {
      if (--pad_devices == 0) {
        pad_buttons = 0;
        pad_hat = 0;
        for (int i = 0; i < HID_PAD_AXES; i++)
          pad_axes[i] = 0;
        pad_seq++;
      }
    }
    free_hid(h);
  }
}

int hid_gamepad_state(char *buf, int max)
{
  if (!pad_devices)
    return -1;
  return ksnprintf(buf, (uint32_t)max, "%u %u %d %d %d %d %d %d %d %d %d\n", pad_seq, pad_buttons, pad_hat,
                   (int)pad_axes[0], (int)pad_axes[1], (int)pad_axes[2], (int)pad_axes[3], (int)pad_axes[4],
                   (int)pad_axes[5], (int)pad_axes[6], (int)pad_axes[7]);
}

void hid_print(int slot, bool detail)
{
  for (struct hid_dev *h = hids; h; h = h->next) {
    if (h->slot != slot)
      continue;
    if (!detail) {
      con_write(" [hid ");
      print_roles(h->roles);
      con_write(h->boot ? " boot]" : "]");
      continue;
    }
    con_printf("         hid if %d: ", h->iface);
    print_roles(h->roles);
    con_printf(", %s protocol, %u fields, %u report(s)\n", h->boot ? "boot" : "report", (unsigned)h->desc.nfields,
               (unsigned)h->desc.nreports);
    for (int i = 0; i < h->raw_len; i++)
      con_printf("%s%02x", i % 16 == 0 ? (i ? "\n" : "           rd ") : " ", h->raw[i]);
    if (h->raw_len)
      con_putc('\n');
  }
}
