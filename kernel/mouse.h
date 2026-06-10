/* mouse.h — PS/2 mouse driver. */
#ifndef OWOS_MOUSE_H
#define OWOS_MOUSE_H
#include <stdint.h>
void mouse_init(int max_x, int max_y);
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);     /* bit0 left, bit1 right, bit2 middle */
/* Monotonic counter bumped on every movement/button packet, so the compositor
 * can tell when state changed without polling each field. */
uint32_t mouse_seq(void);
#endif
