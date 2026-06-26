/* vmmouse.c — VMware absolute pointer over the hypervisor backdoor. See header.
 *
 * The protocol rides three backdoor commands: COMMAND (enable / pick absolute or
 * relative mode), STATUS (how many dwords are queued), and DATA (pull dwords).
 * A pointer packet is four dwords: flags/buttons, absolute X, absolute Y, and a
 * signed wheel delta. Absolute X/Y arrive in [0,0xFFFF] regardless of resolution
 * and we scale them onto the screen. */

#include <stdint.h>
#include "vmmouse.h"
#include "vmware.h"

/* Backdoor command numbers (ECX). */
#define ABSPOINTER_DATA    39
#define ABSPOINTER_STATUS  40
#define ABSPOINTER_COMMAND 41

/* Sub-commands passed in EBX to ABSPOINTER_COMMAND. READ_ID queues the protocol
 * version word (the handshake we verify); REQUEST_ABSOLUTE makes the device the
 * active pointer and switches it to absolute coordinates. */
#define CMD_READ_ID          0x45414552u
#define CMD_REQUEST_ABSOLUTE 0x53424152u

#define VERSION_ID 0x3442554au   /* what READ_ID then DATA(1) yields             */
#define STATUS_ERROR 0xFFFF0000u /* STATUS reads this when the device wedged      */

/* Set in a packet's flags dword when EBX/ECX carry relative deltas, not an
 * absolute position — VMware emits these on focus/grab transitions. */
#define RELATIVE_PACKET 0x00010000u

/* Button bits in a packet's flags dword. */
#define BTN_LEFT   0x20
#define BTN_RIGHT  0x10
#define BTN_MIDDLE 0x08

static int enabled;
static int bound_x, bound_y;

int vmmouse_active(void) { return enabled; }

/* Rescale absolute coordinates onto a new screen size (after a resolution
 * change). The device keeps reporting in [0,0xFFFF]; only the target box moves. */
void vmmouse_set_bounds(int max_x, int max_y)
{
	bound_x = max_x;
	bound_y = max_y;
}

int vmmouse_init(int max_x, int max_y)
{
	if (!vmware_present())
		return -1;

	bound_x = max_x;
	bound_y = max_y;

	struct vmware_regs r = { 0 };

	/* Kick off the version handshake: READ_ID queues the protocol version, then
	 * STATUS must report it queued and DATA(1) must yield the version word. */
	r.ebx = CMD_READ_ID;
	vmware_send(ABSPOINTER_COMMAND, &r);

	r.ebx = 0;
	vmware_send(ABSPOINTER_STATUS, &r);
	if ((r.eax & 0x0000FFFFu) == 0)
		return -1;

	r.ebx = 1;
	vmware_send(ABSPOINTER_DATA, &r);
	if (r.eax != VERSION_ID)
		return -1;

	/* Ask for absolute coordinates (the default after enable is relative). */
	r.ebx = CMD_REQUEST_ABSOLUTE;
	vmware_send(ABSPOINTER_COMMAND, &r);

	enabled = 1;
	return 0;
}

int vmmouse_poll(int *x, int *y, int *btn, int *dz)
{
	if (!enabled)
		return 0;

	struct vmware_regs r = { 0 };
	r.ebx = 0;
	vmware_send(ABSPOINTER_STATUS, &r);
	if (r.eax == STATUS_ERROR) {        /* device fell over: stop using it        */
		enabled = 0;
		return 0;
	}

	uint32_t words = r.eax & 0xFFFFu;
	if (words < 4)
		return 0;

	/* Drain whole 4-dword packets; the last one read wins for position/buttons,
	 * wheel deltas accumulate. x and y come in holding the current cursor
	 * position (mouse.c seeds them), which we update in place — so a relative
	 * packet adds to it rather than starting from zero. */
	int wheel = 0, updated = 0;
	int px = *x, py = *y;
	while (words >= 4) {
		r.ebx = 4;
		vmware_send(ABSPOINTER_DATA, &r);
		uint32_t flags = r.eax;
		wheel += (int)(int8_t)(r.edx & 0xFF);

		int b = 0;
		if (flags & BTN_LEFT)   b |= 1;
		if (flags & BTN_RIGHT)  b |= 2;
		if (flags & BTN_MIDDLE) b |= 4;
		*btn = b;

		/* The device can emit a *relative* packet on a focus/grab transition even
		 * in absolute mode (VMware does this on click-to-enter). EBX/ECX are then
		 * signed deltas, not [0,0xFFFF] absolutes — scaling them as absolute would
		 * snap the cursor to the top-left ("teleport"). Honor the flag. */
		if (flags & RELATIVE_PACKET) {
			px += (int)(int32_t)r.ebx;
			py += (int)(int32_t)r.ecx;
		} else {
			px = (int)((r.ebx * (uint32_t)bound_x) / 0xFFFFu);
			py = (int)((r.ecx * (uint32_t)bound_y) / 0xFFFFu);
		}
		if (px < 0) px = 0; else if (px > bound_x) px = bound_x;
		if (py < 0) py = 0; else if (py > bound_y) py = bound_y;
		updated = 1;
		words -= 4;
	}
	*x  = px;
	*y  = py;
	/* VMware reports +toward-the-user (like the raw PS/2 wheel byte); negate so
	 * the delta follows the kernel's +up/-down WEV_SCROLL convention, matching
	 * what mouse.c's PS/2 path does before it adds the delta. */
	*dz = -wheel;
	return updated;
}
