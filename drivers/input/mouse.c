/* mouse.c: PS/2 mouse on the i8042 aux port (IRQ12), accumulating a clamped absolute cursor. */

#include "mouse.h"
#include "console.h"
#include "io.h"
#include "irq.h"
#include "synaptics.h"
#include "vmmouse.h"
#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

#define STAT_OUTPUT_FULL 0x01 /* data is waiting to be read */
#define STAT_INPUT_FULL 0x02  /* controller still busy; don't write yet      */
#define STAT_AUX_DATA 0x20    /* the pending byte came from the mouse         */

/* Movement-packet flag bits (byte 0). */
#define PKT_LEFT 0x01
#define PKT_RIGHT 0x02
#define PKT_MIDDLE 0x04
#define PKT_ALWAYS1 0x08
#define PKT_X_SIGN 0x10
#define PKT_Y_SIGN 0x20
#define PKT_X_OVF 0x40
#define PKT_Y_OVF 0x80

static volatile int cur_x, cur_y;
static volatile int buttons;
static volatile int wheel; /* cumulative wheel notches              */
static volatile uint32_t seq;
static int bound_x, bound_y;

static uint8_t packet[4];
static uint8_t cycle;
static bool wheel_mode; /* 1 once IntelliMouse 4-byte mode is on  */
static bool syn_mode;   /* 1 once a Synaptics TouchPad took over   */

/* Pointer-speed level 1..5 (3 = 1x) as a num/den factor; the fractional residual carries over. */
static int speed = 3;
static int res_x, res_y;
static const int spd_num[5] = {1, 3, 1, 3, 2};
static const int spd_den[5] = {2, 4, 1, 2, 1};

/* Spin until the controller can accept a write (input buffer empties). */
static void ps2_wait_write(void)
{
  for (int i = 0; i < 100000; i++)
    if (!(inb(PS2_STATUS) & STAT_INPUT_FULL))
      return;
}

/* Spin until a byte is available to read. */
static void ps2_wait_read(void)
{
  for (int i = 0; i < 100000; i++)
    if (inb(PS2_STATUS) & STAT_OUTPUT_FULL)
      return;
}

/* Send a command byte to the mouse (prefixed with 0xD4) and swallow its ACK. */
static void mouse_command(uint8_t cmd)
{
  ps2_wait_write();
  outb(PS2_CMD, 0xD4); /* "next byte goes to the aux device"      */
  ps2_wait_write();
  outb(PS2_DATA, cmd);
  ps2_wait_read();
  (void)inb(PS2_DATA); /* expect 0xFA ACK                          */
}

static void mouse_apply(void)
{
  uint8_t flags = packet[0];
  if (!(flags & PKT_ALWAYS1)) /* lost sync: ignore this packet            */
    return;

  int dx = packet[1];
  int dy = packet[2];
  if (flags & PKT_X_SIGN)
    dx -= 256;
  if (flags & PKT_Y_SIGN)
    dy -= 256;
  if (flags & (PKT_X_OVF | PKT_Y_OVF)) /* discard overflowed deltas        */
    dx = dy = 0;

  /* Scale deltas by the speed factor, carrying the truncated remainder. */
  int num = spd_num[speed - 1], den = spd_den[speed - 1];
  int sx = dx * num + res_x, sy = dy * num + res_y;
  int adx = sx / den;
  res_x = sx - adx * den;
  int ady = sy / den;
  res_y = sy - ady * den;

  int nx = cur_x + adx;
  int ny = cur_y - ady; /* screen Y grows downward                  */
  if (nx < 0)
    nx = 0;
  if (ny < 0)
    ny = 0;
  if (nx > bound_x)
    nx = bound_x;
  if (ny > bound_y)
    ny = bound_y;

  cur_x = nx;
  cur_y = ny;
  buttons = flags & (PKT_LEFT | PKT_RIGHT | PKT_MIDDLE);

  if (wheel_mode) {
    /* Byte 3: signed wheel nibble; negate so wheel follows the +up WEV_SCROLL convention. */
    int dz = packet[3] & 0x0F;
    if (dz & 0x08)
      dz -= 16;
    wheel -= dz;
  }
  seq++;
}

static void mouse_handler(struct registers *r)
{
  (void)r;
  uint8_t st = inb(PS2_STATUS);
  if (!(st & STAT_OUTPUT_FULL) || !(st & STAT_AUX_DATA))
    return;

  uint8_t data = inb(PS2_DATA); /* always consume, to clear the controller */

  /* VMware vmmouse raises IRQ12 but the data is in the backdoor queue; ignore the PS/2 byte. */
  if (vmmouse_active()) {
    int nx = cur_x, ny = cur_y, btn = buttons, dz = 0;
    if (vmmouse_poll(&nx, &ny, &btn, &dz)) {
      cur_x = nx;
      cur_y = ny;
      buttons = btn;
      wheel += dz;
      seq++;
    }
    return;
  }

  /* A Synaptics TouchPad decodes its own absolute packets; feed it the raw byte. */
  if (syn_mode) {
    int nx = cur_x, ny = cur_y, btn = buttons, dz = 0;
    if (synaptics_feed(data, &nx, &ny, &btn, &dz)) {
      cur_x = nx;
      cur_y = ny;
      buttons = btn;
      wheel += dz;
      seq++;
    }
    return;
  }

  int last = wheel_mode ? 3 : 2; /* index of the packet's final byte       */
  switch (cycle) {
  case 0:
    if (!(data & PKT_ALWAYS1)) /* resync on the always-1 bit             */
      return;
    packet[0] = data;
    cycle = 1;
    break;
  case 1:
    packet[1] = data;
    cycle = 2;
    break;
  case 2:
    packet[2] = data;
    if (last == 2) {
      cycle = 0;
      mouse_apply();
    } else
      cycle = 3;
    break;
  case 3:
    packet[3] = data;
    cycle = 0;
    mouse_apply();
    break;
  }
}

void mouse_init(int max_x, int max_y)
{
  bound_x = max_x;
  bound_y = max_y;
  cur_x = max_x / 2;
  cur_y = max_y / 2;

  /* Drain the output buffer so the config read doesn't see stale data. */
  for (int i = 0; i < 16 && (inb(PS2_STATUS) & STAT_OUTPUT_FULL); i++)
    (void)inb(PS2_DATA);

  /* Enable the auxiliary (mouse) device. */
  ps2_wait_write();
  outb(PS2_CMD, 0xA8);

  /* Force both IRQs on (bit0/1) and both clocks on (clear bit4/5); keep bit6 so keyboard.c sees set-1 codes. */
  ps2_wait_write();
  outb(PS2_CMD, 0x20);
  ps2_wait_read();
  uint8_t cfg = inb(PS2_DATA);
  cfg |= 0x03;
  cfg &= ~0x30;
  ps2_wait_write();
  outb(PS2_CMD, 0x60);
  ps2_wait_write();
  outb(PS2_DATA, cfg);

  /* Make sure the keyboard port itself is enabled. */
  ps2_wait_write();
  outb(PS2_CMD, 0xAE);

  mouse_command(0xF6); /* load default settings                    */

  /* Probe Synaptics before the IntelliMouse knock, which would disturb the pad; synaptics.c then owns decoding. */
  syn_mode = (synaptics_init(bound_x, bound_y) == 0);
  con_write(synaptics_status());
  con_write("\n");

  if (!syn_mode) {
    /* IntelliMouse knock: set-sample-rate 200, 100, 80; ID 3 means a wheel. */
    mouse_command(0xF3);
    mouse_command(200);
    mouse_command(0xF3);
    mouse_command(100);
    mouse_command(0xF3);
    mouse_command(80);
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);
    ps2_wait_write();
    outb(PS2_DATA, 0xF2); /* get device ID                        */
    ps2_wait_read();
    (void)inb(PS2_DATA); /* ACK                                  */
    ps2_wait_read();
    wheel_mode = (inb(PS2_DATA) == 3);

    mouse_command(0xF4); /* enable data reporting                */
  }

  /* Under VMware (or QEMU vmmouse) switch to the absolute pointer; falls back to PS/2 without a backdoor. */
  vmmouse_init(bound_x, bound_y);
  con_write(vmmouse_active() ? "mouse: vmmouse absolute pointer active.\n"
                             : "mouse: PS/2 relative (no VMware backdoor).\n");

  irq_register(12, mouse_handler);
  irq_unmask(2); /* the slave PIC is cascaded onto IRQ2      */
  irq_unmask(12);
}

int mouse_x(void) { return cur_x; }
int mouse_y(void) { return cur_y; }
/* Route to a driving Synaptics pad so the timed tap-to-click pulse shows without a new packet. */
int mouse_buttons(void) { return syn_mode ? synaptics_buttons() : buttons; }
int mouse_wheel(void) { return wheel; }
uint32_t mouse_seq(void) { return seq; }

void mouse_set_speed(int level)
{
  if (level < 1)
    level = 1;
  if (level > 5)
    level = 5;
  speed = level;
  res_x = res_y = 0; /* drop stale fraction from the old scale */
}

int mouse_speed(void) { return speed; }

/* Update the clamp box after a resolution change and clamp the cursor into it. */
void mouse_set_bounds(int max_x, int max_y)
{
  bound_x = max_x;
  bound_y = max_y;
  if (cur_x > bound_x)
    cur_x = bound_x;
  if (cur_y > bound_y)
    cur_y = bound_y;
  /* Keep the absolute pointers' bounds in step with the new screen. */
  vmmouse_set_bounds(max_x, max_y);
  synaptics_set_bounds(max_x, max_y);
}
