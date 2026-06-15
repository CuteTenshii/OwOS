/* mouse.h — PS/2 mouse driver. */
#ifndef OWOS_MOUSE_H
#define OWOS_MOUSE_H
#include <stdint.h>
void mouse_init(int max_x, int max_y);
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);     /* bit0 left, bit1 right, bit2 middle */
/* Cumulative scroll-wheel position in notches (IntelliMouse). Grows positive as
 * the wheel turns away from the user ("up"); read it and diff against a previous
 * value to get a delta. Stuck at 0 if the device has no wheel. */
int  mouse_wheel(void);
/* Monotonic counter bumped on every movement/button packet, so the compositor
 * can tell when state changed without polling each field. */
uint32_t mouse_seq(void);
/* Pointer-speed scale, a level 1..5 (3 = native 1x; lower slower, higher faster)
 * applied to PS/2 relative motion. Settable from ring 3 via /proc/mouse (the
 * Settings app). set clamps into range; get returns the current level. */
void mouse_set_speed(int level);
int  mouse_speed(void);
#endif
