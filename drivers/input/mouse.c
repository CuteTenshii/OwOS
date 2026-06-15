/* mouse.c — PS/2 mouse on the i8042 auxiliary port (IRQ12).
 *
 * The controller delivers a 3-byte movement packet on every motion or button
 * change: a flags byte (button state + sign/overflow bits) and signed X/Y
 * deltas. We accumulate an absolute cursor position, clamped to the screen, so
 * the compositor can read mouse_x/mouse_y directly. Y is inverted (the mouse
 * reports +Y as "up"), so screen Y decreases as the mouse moves up.
 */

#include <stdint.h>
#include "mouse.h"
#include "vmmouse.h"
#include "synaptics.h"
#include "irq.h"
#include "io.h"
#include "console.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STAT_OUTPUT_FULL 0x01   /* data is waiting to be read                  */
#define STAT_INPUT_FULL  0x02   /* controller still busy; don't write yet      */
#define STAT_AUX_DATA    0x20   /* the pending byte came from the mouse         */

/* Movement-packet flag bits (byte 0). */
#define PKT_LEFT     0x01
#define PKT_RIGHT    0x02
#define PKT_MIDDLE   0x04
#define PKT_ALWAYS1  0x08
#define PKT_X_SIGN   0x10
#define PKT_Y_SIGN   0x20
#define PKT_X_OVF    0x40
#define PKT_Y_OVF    0x80

static volatile int      cur_x, cur_y;
static volatile int      buttons;
static volatile int      wheel;        /* cumulative wheel notches              */
static volatile uint32_t seq;
static int               bound_x, bound_y;

static uint8_t           packet[4];
static uint8_t           cycle;
static int               wheel_mode;   /* 1 once IntelliMouse 4-byte mode is on  */
static int               syn_mode;     /* 1 once a Synaptics TouchPad took over   */

/* Pointer-speed scale applied to PS/2 relative deltas: a level 1..5 (3 = the
 * native 1x), each mapping to a num/den factor. A fractional residual carries
 * the bit lost to integer division into the next packet, so a slow setting still
 * moves smoothly instead of snapping in whole steps. Settable from user space via
 * /sys/class/input/mouse0/speed (the Settings app's Mouse/Touchpad category). */
static int               speed = 3;
static int               res_x, res_y;
static const int         spd_num[5] = { 1, 3, 1, 3, 2 };
static const int         spd_den[5] = { 2, 4, 1, 2, 1 };

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
	outb(PS2_CMD, 0xD4);            /* "next byte goes to the aux device"      */
	ps2_wait_write();
	outb(PS2_DATA, cmd);
	ps2_wait_read();
	(void)inb(PS2_DATA);           /* expect 0xFA ACK                          */
}

static void mouse_apply(void)
{
	uint8_t flags = packet[0];
	if (!(flags & PKT_ALWAYS1))    /* lost sync: ignore this packet            */
		return;

	int dx = packet[1];
	int dy = packet[2];
	if (flags & PKT_X_SIGN) dx -= 256;
	if (flags & PKT_Y_SIGN) dy -= 256;
	if (flags & (PKT_X_OVF | PKT_Y_OVF))   /* discard overflowed deltas        */
		dx = dy = 0;

	/* Scale the raw deltas by the pointer-speed factor, accumulating the
	 * truncated remainder so slow settings stay smooth. */
	int num = spd_num[speed - 1], den = spd_den[speed - 1];
	int sx = dx * num + res_x, sy = dy * num + res_y;
	int adx = sx / den;  res_x = sx - adx * den;
	int ady = sy / den;  res_y = sy - ady * den;

	int nx = cur_x + adx;
	int ny = cur_y - ady;          /* screen Y grows downward                  */
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx > bound_x) nx = bound_x;
	if (ny > bound_y) ny = bound_y;

	cur_x = nx;
	cur_y = ny;
	buttons = flags & (PKT_LEFT | PKT_RIGHT | PKT_MIDDLE);

	if (wheel_mode) {
		/* Byte 3 carries the wheel delta in its low nibble, sign-extended. */
		int dz = packet[3] & 0x0F;
		if (dz & 0x08) dz -= 16;
		wheel += dz;
	}
	seq++;
}

static void mouse_handler(struct registers *r)
{
	(void)r;
	uint8_t st = inb(PS2_STATUS);
	if (!(st & STAT_OUTPUT_FULL) || !(st & STAT_AUX_DATA))
		return;

	uint8_t data = inb(PS2_DATA);     /* always consume, to clear the controller */

	/* Under VMware, the vmmouse device raises IRQ12 through the i8042 but the
	 * real data lives in the backdoor queue: read absolute position from there
	 * and ignore the (filler) PS/2 byte we just drained. */
	if (vmmouse_active()) {
		int nx = cur_x, ny = cur_y, btn = buttons, dz = 0;
		if (vmmouse_poll(&nx, &ny, &btn, &dz)) {
			cur_x = nx; cur_y = ny; buttons = btn;
			wheel += dz;
			seq++;
		}
		return;
	}

	/* A Synaptics TouchPad reassembles its own 6-byte absolute packets and
	 * synthesizes relative motion / tap / scroll. Feed it the raw byte. */
	if (syn_mode) {
		int nx = cur_x, ny = cur_y, btn = buttons, dz = 0;
		if (synaptics_feed(data, &nx, &ny, &btn, &dz)) {
			cur_x = nx; cur_y = ny; buttons = btn;
			wheel += dz;
			seq++;
		}
		return;
	}

	int last = wheel_mode ? 3 : 2;    /* index of the packet's final byte       */
	switch (cycle) {
	case 0:
		if (!(data & PKT_ALWAYS1))   /* resync on the always-1 bit             */
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
		if (last == 2) { cycle = 0; mouse_apply(); }
		else             cycle = 3;
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

	/* Drain any byte sitting in the output buffer so the config read below
	 * doesn't pick up stale data. */
	for (int i = 0; i < 16 && (inb(PS2_STATUS) & STAT_OUTPUT_FULL); i++)
		(void)inb(PS2_DATA);

	/* Enable the auxiliary (mouse) device. */
	ps2_wait_write();
	outb(PS2_CMD, 0xA8);

	/* Rewrite the controller config byte. Crucially we force BOTH interrupts on
	 * (bit0 keyboard, bit1 mouse) and BOTH clocks on (clear bit4/bit5) rather
	 * than trusting the read — a stale read here used to write the keyboard's
	 * own IRQ back as disabled, killing the keyboard. Bit 6 (scancode
	 * translation) is preserved so keyboard.c keeps seeing set-1 codes. */
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

	mouse_command(0xF6);           /* load default settings                    */

	/* A laptop Synaptics TouchPad rides this same aux port but speaks an
	 * extended absolute protocol. Detect and enable it here, before the
	 * IntelliMouse knock below — the knock's set-sample-rate sequence would
	 * disturb the pad's mode. When found, synaptics.c owns all decoding and we
	 * skip the relative-mouse wheel setup entirely. */
	syn_mode = (synaptics_init(bound_x, bound_y) == 0);
	con_write(synaptics_status());
	con_write("\n");

	if (!syn_mode) {
		/* Try to switch the mouse into IntelliMouse mode (adds a scroll-wheel
		 * byte to every packet). The "magic knock" is three set-sample-rate
		 * commands (200, 100, 80); afterwards the device reports ID 3 if it has
		 * a wheel. */
		mouse_command(0xF3); mouse_command(200);
		mouse_command(0xF3); mouse_command(100);
		mouse_command(0xF3); mouse_command(80);
		ps2_wait_write();
		outb(PS2_CMD, 0xD4);
		ps2_wait_write();
		outb(PS2_DATA, 0xF2);          /* get device ID                        */
		ps2_wait_read();
		(void)inb(PS2_DATA);           /* ACK                                  */
		ps2_wait_read();
		wheel_mode = (inb(PS2_DATA) == 3);

		mouse_command(0xF4);           /* enable data reporting                */
	}

	/* If we're under VMware (or QEMU's vmport + vmmouse), switch to the absolute
	 * pointer: the host and guest cursors then track exactly, with no PS/2 delta
	 * decoding. The PS/2 setup above stays — vmmouse still rides IRQ12 — and we
	 * keep the same bounds. Falls back silently to PS/2 if there's no backdoor. */
	/* Diagnostic plumbing for on-hardware (VMware) debugging: a journal line so a
	 * real-VMware boot shows whether the absolute pointer engaged. */
	vmmouse_init(bound_x, bound_y);
	con_write(vmmouse_active() ? "mouse: vmmouse absolute pointer active.\n"
	                           : "mouse: PS/2 relative (no VMware backdoor).\n");

	irq_register(12, mouse_handler);
	irq_unmask(2);                 /* the slave PIC is cascaded onto IRQ2      */
	irq_unmask(12);
}

int      mouse_x(void)        { return cur_x; }
int      mouse_y(void)        { return cur_y; }
/* When a Synaptics pad is driving, route to it so the time-limited tap-to-click
 * pulse is reflected even on frames with no new packet (finger already lifted). */
int      mouse_buttons(void)  { return syn_mode ? synaptics_buttons() : buttons; }
int      mouse_wheel(void)    { return wheel; }
uint32_t mouse_seq(void)      { return seq; }

void mouse_set_speed(int level)
{
	if (level < 1) level = 1;
	if (level > 5) level = 5;
	speed = level;
	res_x = res_y = 0;             /* drop stale fraction from the old scale */
}

int mouse_speed(void) { return speed; }
