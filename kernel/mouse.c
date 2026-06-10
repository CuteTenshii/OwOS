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
#include "irq.h"
#include "io.h"

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
static volatile uint32_t seq;
static int               bound_x, bound_y;

static uint8_t           packet[3];
static uint8_t           cycle;

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

	int nx = cur_x + dx;
	int ny = cur_y - dy;           /* screen Y grows downward                  */
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx > bound_x) nx = bound_x;
	if (ny > bound_y) ny = bound_y;

	cur_x = nx;
	cur_y = ny;
	buttons = flags & (PKT_LEFT | PKT_RIGHT | PKT_MIDDLE);
	seq++;
}

static void mouse_handler(struct registers *r)
{
	(void)r;
	uint8_t st = inb(PS2_STATUS);
	if (!(st & STAT_OUTPUT_FULL) || !(st & STAT_AUX_DATA))
		return;

	uint8_t data = inb(PS2_DATA);
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
	mouse_command(0xF4);           /* enable data reporting                    */

	irq_register(12, mouse_handler);
	irq_unmask(2);                 /* the slave PIC is cascaded onto IRQ2      */
	irq_unmask(12);
}

int      mouse_x(void)        { return cur_x; }
int      mouse_y(void)        { return cur_y; }
int      mouse_buttons(void)  { return buttons; }
uint32_t mouse_seq(void)      { return seq; }
