/* synaptics.h — Synaptics TouchPad, the extended PS/2 absolute protocol.
 *
 * A laptop Synaptics pad lives on the i8042 auxiliary port (IRQ12) just like a
 * plain PS/2 mouse, but speaks a richer protocol: special commands are encoded
 * as runs of "Set Resolution" writes, and once switched into *absolute* W-mode
 * it streams a 6-byte packet carrying an absolute (X,Y) finger position, a
 * pressure (Z) reading, and a finger-count/width (W) field. From that we
 * synthesize a normal relative cursor with tap-to-click and two-finger scroll —
 * the touchpad feel — while mouse.c keeps owning IRQ12 and just feeds us bytes.
 *
 * Real-hardware-only: QEMU exposes no Synaptics device, so detection always
 * fails there and mouse.c falls back to the plain PS/2 relative path. The
 * diagnostic plumbing (synaptics_status) is for bring-up on a real laptop. */
#ifndef OWOS_SYNAPTICS_H
#define OWOS_SYNAPTICS_H

#include <stdint.h>

/* Probe the aux device for a Synaptics TouchPad and, if found, switch it into
 * absolute (W-mode) reporting and enable it. Screen coordinates are mapped into
 * [0,max_x] x [0,max_y]. Returns 0 if a pad came up (synaptics_active()==1),
 * -1 if the aux device isn't a Synaptics — leave PS/2 in charge. Call once from
 * mouse_init, after the controller config but before the IntelliMouse knock
 * (which a Synaptics would misinterpret). */
int synaptics_init(int max_x, int max_y);

/* 1 once synaptics_init succeeded. */
int synaptics_active(void);

/* Feed one aux byte drained in the mouse IRQ. Accumulates a 6-byte absolute
 * packet; when a packet completes and the resulting cursor/buttons/wheel differ
 * from the last reported state, updates the cursor and returns 1, writing the
 * new screen position into *x,*y, the button mask into *btn (bit0 left, bit1
 * right, bit2 middle), and the wheel delta (two-finger scroll) into *dz.
 * Returns 0 mid-packet, on a dropped (out-of-sync) byte, or when nothing
 * changed. Call only from the mouse IRQ handler. */
int synaptics_feed(uint8_t byte, int *x, int *y, int *btn, int *dz);

/* Current button mask, including the time-limited tap-to-click pulse. mouse.c's
 * mouse_buttons() routes here when we're active so a tap registers a clean
 * press+release across WM frames even though no packet arrives on finger-lift
 * to clear the pulse. Bit0 left, bit1 right, bit2 middle. */
int synaptics_buttons(void);

/* One-line human status for on-hardware debugging: the firmware version + a
 * capability summary when a pad is found, or the raw identify bytes / reason
 * when it isn't. Surfaced through the boot journal by mouse_init. */
const char *synaptics_status(void);

#endif /* OWOS_SYNAPTICS_H */
