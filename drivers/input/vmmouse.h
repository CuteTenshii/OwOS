/* vmmouse.h — VMware absolute pointing device, over the backdoor.
 *
 * When running under VMware/QEMU-vmport with the vmmouse device present, the
 * guest can read an *absolute* cursor position instead of decoding PS/2 relative
 * deltas — so the host and guest cursors stay glued together with no capture.
 * The device still drives IRQ12 through the i8042 to signal new data; mouse.c
 * keeps owning that interrupt and drains us from it when we're active. */
#ifndef OWOS_VMMOUSE_H
#define OWOS_VMMOUSE_H

/* Detect + enable absolute mode. Coordinates are reported scaled into
 * [0,max_x] x [0,max_y]. Returns 0 if the device came up (vmmouse_active() will
 * read 1), -1 if there's no VMware backdoor / vmmouse — leave PS/2 in charge. */
int vmmouse_init(int max_x, int max_y);

/* 1 once vmmouse_init succeeded. */
int vmmouse_active(void);

/* Rescale the absolute range onto a new screen size after a resolution change. */
void vmmouse_set_bounds(int max_x, int max_y);

/* Drain the device's packet queue. On a state change, writes the latest absolute
 * position into x and y, the button mask into btn, and the summed wheel delta
 * into dz, then returns 1. Returns 0 if the
 * queue was empty (no change). Call from the mouse IRQ handler.
 *   x, y  — latest absolute cursor position
 *   btn   — button mask (bit0 left, bit1 right, bit2 middle)
 *   dz    — summed wheel delta */
int vmmouse_poll(int *x, int *y, int *btn, int *dz);

#endif /* OWOS_VMMOUSE_H */
