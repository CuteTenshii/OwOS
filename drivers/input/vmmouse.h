/* vmmouse.h: VMware absolute pointing device over the backdoor.
 * mouse.c owns IRQ12 and drains us from it when active. */
#ifndef OWOS_VMMOUSE_H
#define OWOS_VMMOUSE_H

#include <stdbool.h>

/* Detect and enable absolute mode scaled into [0,max_x] x [0,max_y]; 0 on success, -1 if absent. */
int vmmouse_init(int max_x, int max_y);

/* 1 once vmmouse_init succeeded. */
bool vmmouse_active(void);

/* Rescale the absolute range after a resolution change. */
void vmmouse_set_bounds(int max_x, int max_y);

/* Drain the packet queue; on change fills x, y, btn (bit0 left, bit1 right, bit2 middle), dz and returns 1. */
bool vmmouse_poll(int *x, int *y, int *btn, int *dz);

#endif
