#ifndef BROWSER_GAMEPAD_STATE_H
#define BROWSER_GAMEPAD_STATE_H

#include <stdbool.h>

/* How a controller's buttons and axes map onto the W3C "standard" layout; generic pads are exposed as they are. */
enum gp_kind { GP_GENERIC, GP_DS4, GP_DS5, GP_SWITCH_PRO, GP_XBOX };

#define GP_STD_BUTTONS 17
#define GP_STD_AXES 4
#define GP_RAW_AXES 6
#define GP_RAW_PAD_BUTTONS 16
#define GP_RAW_BUTTONS (GP_RAW_PAD_BUTTONS + 4) /* the pad's buttons, then d-pad up, down, left, right from the hat */

struct gp_state {
  unsigned seq, buttons;
  int hat; /* 0 centered, 1 = up then clockwise to 8 */
  int axes[8];
};

struct gp_id {
  enum gp_kind kind;
  unsigned vid, pid;
  char label[40];
};

/* Parse a /sys/class/input/js0/state line; false if malformed. */
bool gp_parse(const char *line, struct gp_state *st);
/* Parse a /sys/class/input/js0/id line ("KIND VID PID LABEL"); false if malformed. */
bool gp_parse_id(const char *line, struct gp_id *id);

bool gp_standard(enum gp_kind kind);
int gp_nbuttons(enum gp_kind kind);
int gp_naxes(enum gp_kind kind);
/* Axis i as -1..1. */
double gp_axis(enum gp_kind kind, const struct gp_state *st, int i);
/* Button i as 0..1 (analog triggers on standard layouts). */
double gp_button(enum gp_kind kind, const struct gp_state *st, int i);
/* True if anything is pressed, which is what lets a page see the pad (as in other browsers). */
bool gp_active(const struct gp_state *st);

#endif
