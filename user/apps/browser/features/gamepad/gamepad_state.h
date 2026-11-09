#ifndef BROWSER_GAMEPAD_STATE_H
#define BROWSER_GAMEPAD_STATE_H

#include <stdbool.h>

#define GP_AXES 6
#define GP_PAD_BUTTONS 16
#define GP_BUTTONS (GP_PAD_BUTTONS + 4) /* the pad's buttons, then d-pad up, down, left, right from the hat */

struct gp_state {
  unsigned seq, buttons;
  int hat; /* 0 centered, 1 = up then clockwise to 8 */
  int axes[GP_AXES];
};

/* Parse a /sys/class/input/js0/state line; false if malformed. */
bool gp_parse(const char *line, struct gp_state *st);
/* Axis i as -1..1. */
double gp_axis(const struct gp_state *st, int i);
bool gp_button(const struct gp_state *st, int i);
/* True if anything is pressed, which is what lets a page see the pad (as in other browsers). */
bool gp_active(const struct gp_state *st);

#endif
