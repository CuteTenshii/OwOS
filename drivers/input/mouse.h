/* mouse.h - PS/2 mouse driver. */
#ifndef OWOS_MOUSE_H
#define OWOS_MOUSE_H
#include <stdint.h>
void mouse_init(int max_x, int max_y);
int mouse_x(void);
int mouse_y(void);
int mouse_buttons(void); /* bit0 left, bit1 right, bit2 middle */
/* Cumulative scroll-wheel position in notches (IntelliMouse). Grows positive as
 * the wheel turns away from the user ("up"); read it and diff against a
 * previous value to get a delta. Stuck at 0 if the device has no wheel. */
int mouse_wheel(void);
/* Monotonic counter bumped on every movement/button packet, so the compositor
 * can tell when state changed without polling each field. */
uint32_t mouse_seq(void);
/* Pointer-speed scale, a level 1..5 (3 = native 1x; lower slower, higher
 * faster) applied to PS/2 relative motion. Settable from ring 3 via the sysfs
 * attribute /sys/class/input/mouse0/speed (the Settings app). set clamps into
 * range; get returns the current level. */
void mouse_set_speed(int level);
int mouse_speed(void);
/* USB HID pointers. Relative motion is scaled by the pointer speed like PS/2 packets; absolute positions are in
 * framebuffer pixels (scale with mouse_bound_x/y). `btn` is the full button mask held across all USB pointers
 * (bit 0 left, 1 right, 2 middle, 3 and 4 side buttons). hwheel grows positive to the right. */
void mouse_inject_rel(int dx, int dy, int dwheel, int dhwheel, int btn);
void mouse_inject_abs(int x, int y, int dwheel, int dhwheel, int btn);
int mouse_hwheel(void);
int mouse_bound_x(void);
int mouse_bound_y(void);
/* Move the cursor's clamp box (called by the WM after a resolution change). */
void mouse_set_bounds(int max_x, int max_y);
#endif
