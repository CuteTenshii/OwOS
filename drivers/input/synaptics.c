/* synaptics.c — Synaptics TouchPad over the extended PS/2 protocol (IRQ12).
 *
 * mouse.c owns the i8042 aux port and its IRQ; this module is a sub-driver it
 * detects and delegates to, exactly like vmmouse.c. The flow:
 *
 *   1. synaptics_init() runs the identification handshake. Synaptics special
 *      commands work by *encoding an argument byte* as four "Set Resolution"
 *      (0xE8) writes — two argument bits each, most-significant pair first —
 * then issuing a normal command that the pad reinterprets: 0xE9 (Status
 * Request) returns the 3-byte result of the query selected by the argument, and
 *      0xF3 0x14 (Set Sample Rate 20) commits the argument as the *mode byte*.
 *      Every information query echoes the constant 0x47 in its middle result
 *      byte — that's how we know we're really talking to a Synaptics.
 *
 *   2. We switch the pad into Absolute + W mode (high report rate, hardware
 *      gesture processing off so we see raw fingers) and enable reporting.
 *
 *   3. synaptics_feed() reassembles the 6-byte absolute packets mouse.c drains
 *      byte-by-byte and turns absolute finger positions into relative cursor
 *      motion, tap-to-click, and two-finger scroll.
 *
 * Packet (Absolute, W mode), 6 bytes:
 *   byte0: 1 0 W3 W2 0 1 R L        (the 1/0/0/1 pattern is the sync signature)
 *   byte1: Yhi[11:8] Xhi[11:8]
 *   byte2: Z[7:0]                    (pressure)
 *   byte3: 1 1 W1 W0 0 1 R L
 *   byte4: X[7:0]
 *   byte5: Y[7:0]
 *     X = (byte1 & 0x0f)<<8 | byte4 ;  Y = (byte1 & 0xf0)<<4 | byte5
 *     W = (byte0 & 0x30)>>2 | (byte0 & 0x04)>>1 | (byte3 & 0x04)>>2
 *     W==0 → two fingers, W==1 → three fingers, W>=4 → one finger (width=W).
 */

#include "synaptics.h"
#include "console.h"
#include "io.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

#define STAT_OUTPUT_FULL 0x01 /* a byte is waiting to be read */
#define STAT_INPUT_FULL 0x02  /* controller still busy; don't write yet */

/* Mode-byte bits committed via "Set Sample Rate 20". */
#define MODE_ABSOLUTE 0x80
#define MODE_HIGH_RATE 0x40 /* 80 packets/sec instead of 40                 */
#define MODE_DISGEST 0x04   /* disable the pad's own tap/drag gestures      */
#define MODE_W_MODE 0x01    /* enable the W (finger-width/count) field      */

/* Information-query argument selectors (loaded before a Status Request). */
#define QUE_IDENTIFY 0x00
#define QUE_CAPABILITIES 0x02

/* The constant every Synaptics information query echoes in its middle byte. */
#define SYN_MAGIC 0x47

/* Nominal absolute coordinate range (Synaptics defaults). We map this window to
 * the screen for absolute moves and use it to scale relative motion sanely. */
#define PAD_XMIN 1472
#define PAD_XMAX 5472
#define PAD_YMIN 1408
#define PAD_YMAX 4448

/* Tuning. timer_ticks() runs at TIMER_HZ (250 Hz → 4 ms/tick). */
#define Z_TOUCH 24       /* pressure at/above which a finger is "down"   */
#define Z_PALM 220       /* above this it's a palm: ignore for motion    */
#define TAP_TIME_MAX 45  /* ticks: a touch shorter than this can be a tap (~180 ms) */
#define TAP_MOVE_MAX 120 /* pad units of travel still allowed for a tap  */
#define TAP_HOLD 20      /* ticks the synthetic click is held (~80 ms)   */
#define SCROLL_STEP 110  /* pad-Y units of two-finger travel per notch   */

/* Button bits in the public mask (matches mouse_buttons()). */
#define BTN_LEFT 0x01
#define BTN_RIGHT 0x02
#define BTN_MIDDLE 0x04

static bool active;
static int bound_x, bound_y;

/* Live cursor + reported state (what synaptics_feed hands back to mouse.c). */
static int cur_x, cur_y;
static int phys_buttons; /* physical buttons decoded from the last packet */
static int rep_wheel;    /* cumulative wheel, for change detection         */

/* Finger tracking across packets. mode: 0 none, 1 one finger, 2 two fingers. */
static int mode;
static int prev_fx, prev_fy; /* last finger position, for relative deltas */
static int scroll_accum;     /* leftover two-finger travel below one notch     */

/* Tap / gesture state for the in-progress touch. */
static uint32_t touch_start;  /* tick the touch began */
static int touch_x, touch_y;  /* where it began (for the move-budget test) */
static bool touch_moved;       /* set once travel exceeds TAP_MOVE_MAX           */
static int touch_max_fingers; /* most fingers seen during this touch */

/* The synthetic tap pulse, evaluated lazily by synaptics_buttons(). */
static uint32_t tap_start; /* tick the pulse began (0 = inactive)            */
static int tap_btn;        /* BTN_LEFT or BTN_RIGHT                           */

static char status[112] = "synaptics: not probed";

/* ---- tiny formatters (no libc snprintf in the kernel) -------------------- */

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

/* ---- low-level i8042 aux access ------------------------------------------ */

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
  return ps2_read(); /* expect 0xFA */
}

/* Encode an 8-bit argument as four Set-Resolution writes (high pair first). */
static void syn_set_arg(uint8_t arg)
{
  for (int sh = 6; sh >= 0; sh -= 2) {
    aux_cmd(0xE8); /* Set Resolution */
    aux_cmd((arg >> sh) & 0x03);
  }
}

/* Run an information query: load the selector, issue Status Request (0xE9),
 * read the 3 result bytes. Tolerates the device prefixing an ACK or not. */
static void syn_query(uint8_t selector, uint8_t out[3])
{
  syn_set_arg(selector);
  ps2_wait_write();
  outb(PS2_CMD, 0xD4);
  ps2_wait_write();
  outb(PS2_DATA, 0xE9); /* Status Request */
  uint8_t b = ps2_read();
  if (b == 0xFA) /* swallow an ACK if one came first */
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
  aux_cmd(0xF3); /* Set Sample Rate */
  aux_cmd(0x14); /* 20 → "load mode byte" */
}

/* ---- bring-up ------------------------------------------------------------ */

int synaptics_init(int max_x, int max_y)
{
  bound_x = max_x;
  bound_y = max_y;
  cur_x = max_x / 2;
  cur_y = max_y / 2;
  active = false;

  /* Drain any stale output byte so the query results aren't misaligned. */
  for (int i = 0; i < 16 && (inb(PS2_STATUS) & STAT_OUTPUT_FULL); i++)
    (void)inb(PS2_DATA);

  /* Quiet the device while we probe (0xF5 = disable data reporting). */
  aux_cmd(0xF5);

  uint8_t id[3] = {0, 0, 0};
  syn_query(QUE_IDENTIFY, id);

  if (id[1] != SYN_MAGIC) {
    /* Not a Synaptics — record the raw bytes for on-HW debugging and leave
     * the plain PS/2 path to take over (mouse.c re-enables the device). */
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

  /* Switch to absolute W-mode reporting and enable. */
  syn_set_mode(MODE_ABSOLUTE | MODE_HIGH_RATE | MODE_DISGEST | MODE_W_MODE);
  aux_cmd(0xF4); /* enable data reporting */

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

/* Move the clamp box after a resolution change (relative motion is scaled into
 * this box). */
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

/* ---- per-packet decoding ------------------------------------------------- */

/* Pointer-acceleration curve: scale a raw pad delta into a cursor delta, with a
 * mild speed-dependent gain so slow moves are precise and fast flicks cover
 * ground. Guarantees a non-zero step for any non-zero input so the cursor never
 * feels stuck during a slow drag. */
static int accel(int d)
{
  int a = d < 0 ? -d : d;
  int factor = 90 + a * 4; /* 90/256 ≈ 0.35 base gain, grows with speed */
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

/* Evaluate a finished touch for a tap (or two-finger tap → right click). */
static void touch_release(void)
{
  uint32_t dur = timer_ticks() - touch_start;
  if (!touch_moved && dur <= TAP_TIME_MAX) {
    tap_btn = (touch_max_fingers >= 2) ? BTN_RIGHT : BTN_LEFT;
    tap_start = timer_ticks();
    if (tap_start == 0) /* 0 means "inactive"; nudge off it */
      tap_start = 1;
  }
}

bool synaptics_feed(uint8_t byte, int *x, int *y, int *btn, int *dz)
{
  static uint8_t buf[6];
  static int idx;

  /* Resync on the position-specific signature bits before storing. byte0 is
   * 1 0 .. 0 1 .. (mask 0xC8 == 0x80); byte3 is 1 1 .. 0 1 .. (== 0xC0). */
  if (idx == 0 && (byte & 0xC8) != 0x80)
    return false;
  if (idx == 3 && (byte & 0xC8) != 0xC0) {
    idx = 0; /* lost sync; maybe this byte starts a packet */
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

  /* Decode the assembled packet. */
  int fx = ((buf[1] & 0x0f) << 8) | buf[4];
  int fy = ((buf[1] & 0xf0) << 4) | buf[5];
  int z = buf[2];
  int w = ((buf[0] & 0x30) >> 2) | ((buf[0] & 0x04) >> 1) | ((buf[3] & 0x04) >> 2);

  /* Buttons. The whole-pad click on a ClickPad shows up only as the byte0 vs
   * byte3 low-bit XOR; pads with discrete buttons report it in byte0 directly.
   * OR-ing both makes either kind click without needing a capability probe. */
  int left = (buf[0] & 0x01) | ((buf[0] ^ buf[3]) & 0x01);
  int right = (buf[0] & 0x02) ? 1 : 0;
  phys_buttons = (left ? BTN_LEFT : 0) | (right ? BTN_RIGHT : 0);

  int finger = (z >= Z_TOUCH) && (z < Z_PALM) && (w != 2);
  int fingers = !finger ? 0 : (w == 0 ? 2 : 1);

  bool moved = false;
  int wheel_delta = 0;

  if (fingers == 0) {
    if (mode != 0) /* a touch just ended */
      touch_release();
    mode = 0;
    scroll_accum = 0;
  } else {
    if (mode == 0) { /* leading edge of a new touch */
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
      /* One finger: relative cursor motion. Skip the packet where the
       * finger count changed so a landing/lifting second finger can't
       * jump the cursor. */
      if (mode == 1) {
        int dx = accel(fx - prev_fx);
        int dy = accel(fy - prev_fy);
        if (dx || dy) {
          cur_x += dx;
          cur_y -= dy; /* pad +Y is up; screen Y grows down */
          clamp_cursor();
          moved = true;
        }
      }
    } else { /* two fingers: vertical scroll */
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
      tap_start = 0; /* pulse expired */
  }
  return b;
}
