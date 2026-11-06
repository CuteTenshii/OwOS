/* synaptics.h: Synaptics TouchPad, extended PS/2 absolute protocol (real hardware only).
 * Synthesizes relative motion, tap-to-click and two-finger scroll; mouse.c owns IRQ12 and feeds us bytes. */
#ifndef OWOS_SYNAPTICS_H
#define OWOS_SYNAPTICS_H

#include <stdbool.h>
#include <stdint.h>

/* Probe for a Synaptics pad and enable absolute W-mode; 0 on success, -1 otherwise. Call before the IntelliMouse knock.
 */
int synaptics_init(int max_x, int max_y);

/* 1 once synaptics_init succeeded. */
bool synaptics_active(void);

/* Move the clamp box onto a new screen size after a resolution change. */
void synaptics_set_bounds(int max_x, int max_y);

/* Feed one aux byte; returns 1 and fills *x,*y,*btn,*dz when state changed, else 0. IRQ context only. */
bool synaptics_feed(uint8_t byte, int *x, int *y, int *btn, int *dz);

/* Current button mask (bit0 left, bit1 right, bit2 middle), including the tap-click pulse. */
int synaptics_buttons(void);

/* One-line status for on-hardware debugging. */
const char *synaptics_status(void);

#endif
