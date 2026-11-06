/* synaptics.c: Synaptics TouchPad via extended PS/2 (IRQ12), a sub-driver of mouse.c.
 * Special commands encode an argument byte as four Set Resolution (0xE8) writes, MSB pair first.
 * Absolute W-mode packet is 6 bytes: byte0 = 1 0 W3 W2 0 1 R L, byte3 = 1 1 W1 W0 0 1 R L. */

#include "synaptics.h"
#include "console.h"
#include "io.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

#define STAT_OUTPUT_FULL 0x01
#define STAT_INPUT_FULL 0x02

/* Mode-byte bits committed via "Set Sample Rate 20". */
#define MODE_ABSOLUTE 0x80
#define MODE_HIGH_RATE 0x40
#define MODE_DISGEST 0x04
#define MODE_W_MODE 0x01

/* Information-query argument selectors (loaded before a Status Request). */
#define QUE_IDENTIFY 0x00
#define QUE_CAPABILITIES 0x02

/* The constant every Synaptics information query echoes in its middle byte. */
#define SYN_MAGIC 0x47

/* Nominal absolute pad range (Synaptics defaults), mapped to the screen. */
#define PAD_XMIN 1472
#define PAD_XMAX 5472
#define PAD_YMIN 1408
#define PAD_YMAX 4448

/* Tuning; timer_ticks() runs at 250 Hz (4 ms/tick). */
#define Z_TOUCH 24
#define Z_PALM 220
#define TAP_TIME_MAX 45
#define TAP_MOVE_MAX 120
#define TAP_HOLD 20
#define SCROLL_STEP 110

/* Button bits in the public mask (matches mouse_buttons()). */
#define BTN_LEFT 0x01
#define BTN_RIGHT 0x02
#define BTN_MIDDLE 0x04

static bool active;
static int bound_x, bound_y;

static int cur_x, cur_y;
static int phys_buttons;
static int rep_wheel;

/* Finger tracking across packets. mode: 0 none, 1 one finger, 2 two fingers. */
static int mode;
static int prev_fx, prev_fy;
static int scroll_accum;

static uint32_t touch_start;
static int touch_x, touch_y;
static bool touch_moved;
static int touch_max_fingers;

static uint32_t tap_start;
static int tap_btn;

static char status[112] = "synaptics: not probed";

/* Tiny formatters (no libc snprintf in the kernel). */

static char *put_str(char *p, const char *s)
{
  while (*s)
    *p++ = *s++;
  return p;
}

static char *put_hex2(char *p, uint8_t v)
{
  const char *h = "0123456789abcdef";
  *p++ = h[(v >> 4) & 0xf];
  *p++ = h[v & 0xf];
  return p;
}

static char *put_dec(char *p, uint32_t v)
{
  char t[10];
  int n = 0;
  if (!v) {
    *p++ = '0';
    return p;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    *p++ = t[--n];
  return p;
}

static void ps2_wait_write(void)
{
  for (int i = 0; i < 100000; i++)
    if (!(inb(PS2_STATUS) & STAT_INPUT_FULL))
      return;
}

static void ps2_wait_read(void)
{
  for (int i = 0; i < 100000; i++)
    if (inb(PS2_STATUS) & STAT_OUTPUT_FULL)
      return;
}

static uint8_t ps2_read(void)
{
  ps2_wait_read();
  return inb(PS2_DATA);
}

/* Send one byte to the aux device (0xD4 prefix) and return its ACK byte. */
static uint8_t aux_cmd(uint8_t b)
{
  ps2_wait_write();
  outb(PS2_CMD, 0xD4);
  ps2_wait_write();
  outb(PS2_DATA, b);
  return ps2_read();
}

/* Encode an 8-bit argument as four Set-Resolution writes (high pair first). */
static void syn_set_arg(uint8_t arg)
{
  for (int sh = 6; sh >= 0; sh -= 2) {
    aux_cmd(0xE8);
    aux_cmd((arg >> sh) & 0x03);
  }
}

/* Information query: selector, Status Request (0xE9), 3 result bytes; the ACK may or may not come first. */
static void syn_query(uint8_t selector, uint8_t out[3])
{
  syn_set_arg(selector);
  ps2_wait_write();
  outb(PS2_CMD, 0xD4);
  ps2_wait_write();
  outb(PS2_DATA, 0xE9);
  uint8_t b = ps2_read();
  if (b == 0xFA)
    out[0] = ps2_read();
  else
    out[0] = b;
  out[1] = ps2_read();
  out[2] = ps2_read();
}

/* Commit a mode byte: encode it as the argument, then Set Sample Rate 20. */
static void syn_set_mode(uint8_t modebyte)
{
  syn_set_arg(modebyte);
  aux_cmd(0xF3);
  aux_cmd(0x14);
}

int synaptics_init(int max_x, int max_y)
{
  bound_x = max_x;
  bound_y = max_y;
  cur_x = max_x / 2;
  cur_y = max_y / 2;
  active = false;

  for (int i = 0; i < 16 && (inb(PS2_STATUS) & STAT_OUTPUT_FULL); i++)
    (void)inb(PS2_DATA);

  aux_cmd(0xF5);

  uint8_t id[3] = {0, 0, 0};
  syn_query(QUE_IDENTIFY, id);

  if (id[1] != SYN_MAGIC) {
    /* Not a Synaptics: record raw bytes for debugging and leave PS/2 to take over. */
    char *p = status;
    p = put_str(p, "synaptics: absent (id ");
    p = put_hex2(p, id[0]);
    *p++ = ' ';
    p = put_hex2(p, id[1]);
    *p++ = ' ';
    p = put_hex2(p, id[2]);
    p = put_str(p, ")");
    *p = 0;
    return -1;
  }

  /* id[0] = firmware minor, id[2] low nibble = firmware major. */
  uint8_t fw_minor = id[0];
  uint8_t fw_major = id[2] & 0x0f;

  uint8_t cap[3] = {0, 0, 0};
  syn_query(QUE_CAPABILITIES, cap);
  int caps_ok = (cap[1] == SYN_MAGIC);
  uint32_t capword = ((uint32_t)cap[0] << 16) | ((uint32_t)cap[1] << 8) | cap[2];
  int multifinger = caps_ok && (capword & (1u << 13));
  int palmdetect = caps_ok && (capword & (1u << 12));

  syn_set_mode(MODE_ABSOLUTE | MODE_HIGH_RATE | MODE_DISGEST | MODE_W_MODE);
  aux_cmd(0xF4);

  mode = 0;
  phys_buttons = 0;
  rep_wheel = 0;
  scroll_accum = 0;
  tap_start = 0;
  active = true;

  char *p = status;
  p = put_str(p, "synaptics: TouchPad fw ");
  p = put_dec(p, fw_major);
  *p++ = '.';
  p = put_dec(p, fw_minor);
  p = put_str(p, ", absolute W-mode");
  if (multifinger)
    p = put_str(p, ", multifinger");
  if (palmdetect)
    p = put_str(p, ", palm-detect");
  *p = 0;
  return 0;
}

bool synaptics_active(void) { return active; }

/* Move the clamp box after a resolution change. */
void synaptics_set_bounds(int max_x, int max_y)
{
  bound_x = max_x;
  bound_y = max_y;
  if (cur_x > bound_x)
    cur_x = bound_x;
  if (cur_y > bound_y)
    cur_y = bound_y;
}

const char *synaptics_status(void) { return status; }

/* Pointer acceleration: speed-dependent gain, never 0 for non-zero input. */
static int accel(int d)
{
  int a = d < 0 ? -d : d;
  int factor = 90 + a * 4;
  if (factor > 320)
    factor = 320;
  int r = (d * factor) >> 8;
  if (r == 0 && d != 0)
    r = d > 0 ? 1 : -1;
  return r;
}

static void clamp_cursor(void)
{
  if (cur_x < 0)
    cur_x = 0;
  if (cur_y < 0)
    cur_y = 0;
  if (cur_x > bound_x)
    cur_x = bound_x;
  if (cur_y > bound_y)
    cur_y = bound_y;
}

static void touch_release(void)
{
  uint32_t dur = timer_ticks() - touch_start;
  if (!touch_moved && dur <= TAP_TIME_MAX) {
    tap_btn = (touch_max_fingers >= 2) ? BTN_RIGHT : BTN_LEFT;
    tap_start = timer_ticks();
    if (tap_start == 0)
      tap_start = 1;
  }
}

bool synaptics_feed(uint8_t byte, int *x, int *y, int *btn, int *dz)
{
  static uint8_t buf[6];
  static int idx;

  /* Resync on signature bits: byte0 & 0xC8 == 0x80, byte3 & 0xC8 == 0xC0. */
  if (idx == 0 && (byte & 0xC8) != 0x80)
    return false;
  if (idx == 3 && (byte & 0xC8) != 0xC0) {
    idx = 0;
    if ((byte & 0xC8) == 0x80) {
      buf[0] = byte;
      idx = 1;
    }
    return false;
  }

  buf[idx++] = byte;
  if (idx < 6)
    return false;
  idx = 0;

  int fx = ((buf[1] & 0x0f) << 8) | buf[4];
  int fy = ((buf[1] & 0xf0) << 4) | buf[5];
  int z = buf[2];
  int w = ((buf[0] & 0x30) >> 2) | ((buf[0] & 0x04) >> 1) | ((buf[3] & 0x04) >> 2);

  /* ClickPad click is the byte0/byte3 low-bit XOR; discrete buttons use byte0. OR both. */
  int left = (buf[0] & 0x01) | ((buf[0] ^ buf[3]) & 0x01);
  int right = (buf[0] & 0x02) ? 1 : 0;
  phys_buttons = (left ? BTN_LEFT : 0) | (right ? BTN_RIGHT : 0);

  int finger = (z >= Z_TOUCH) && (z < Z_PALM) && (w != 2);
  int fingers = !finger ? 0 : (w == 0 ? 2 : 1);

  bool moved = false;
  int wheel_delta = 0;

  if (fingers == 0) {
    if (mode != 0)
      touch_release();
    mode = 0;
    scroll_accum = 0;
  } else {
    if (mode == 0) {
      touch_start = timer_ticks();
      touch_x = fx;
      touch_y = fy;
      touch_moved = false;
      touch_max_fingers = fingers;
      scroll_accum = 0;
    } else {
      if (fingers > touch_max_fingers)
        touch_max_fingers = fingers;
      int adx = fx - touch_x;
      if (adx < 0)
        adx = -adx;
      int ady = fy - touch_y;
      if (ady < 0)
        ady = -ady;
      if (adx > TAP_MOVE_MAX || ady > TAP_MOVE_MAX)
        touch_moved = true;
    }

    if (fingers == 1) {
      /* Skip the packet where the finger count changed so a second finger can't jump the cursor. */
      if (mode == 1) {
        int dx = accel(fx - prev_fx);
        int dy = accel(fy - prev_fy);
        if (dx || dy) {
          cur_x += dx;
          cur_y -= dy;
          clamp_cursor();
          moved = true;
        }
      }
    } else {
      if (mode == 2) {
        scroll_accum += fy - prev_fy;
        while (scroll_accum >= SCROLL_STEP) {
          wheel_delta += 1;
          scroll_accum -= SCROLL_STEP;
        }
        while (scroll_accum <= -SCROLL_STEP) {
          wheel_delta -= 1;
          scroll_accum += SCROLL_STEP;
        }
      }
    }

    prev_fx = fx;
    prev_fy = fy;
    mode = fingers;
  }

  rep_wheel += wheel_delta;

  int buttons = synaptics_buttons();
  static int last_buttons = 0;
  bool changed = moved || wheel_delta || (buttons != last_buttons);
  last_buttons = buttons;
  if (!changed)
    return false;

  *x = cur_x;
  *y = cur_y;
  *btn = buttons;
  *dz = wheel_delta;
  return true;
}

int synaptics_buttons(void)
{
  int b = phys_buttons;
  if (tap_start) {
    if (timer_ticks() - tap_start < (uint32_t)TAP_HOLD)
      b |= tap_btn;
    else
      tap_start = 0;
  }
  return b;
}
