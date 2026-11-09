#include "features/gamepad/gamepad_state.h"

#define KERNEL_FIELDS 11 /* SEQ BUTTONS HAT then X Y Z Rx Ry Rz Slider Dial */

static const char *next_int(const char *p, long *v)
{
  bool neg = false;
  long n = 0;
  while (*p == ' ')
    p++;
  if (*p == '-') {
    neg = true;
    p++;
  }
  if (*p < '0' || *p > '9')
    return 0;
  for (; *p >= '0' && *p <= '9'; p++)
    n = n * 10 + (*p - '0');
  *v = neg ? -n : n;
  return p;
}

bool gp_parse(const char *line, struct gp_state *st)
{
  long v[KERNEL_FIELDS];
  for (int i = 0; i < KERNEL_FIELDS; i++)
    if (!(line = next_int(line, &v[i])))
      return false;
  st->seq = (unsigned)v[0];
  st->buttons = (unsigned)v[1];
  st->hat = (int)v[2];
  for (int i = 0; i < GP_AXES; i++)
    st->axes[i] = (int)v[3 + i];
  return true;
}

double gp_axis(const struct gp_state *st, int i)
{
  double a = st->axes[i] / 32768.0;
  return a > 1 ? 1 : a;
}

bool gp_button(const struct gp_state *st, int i)
{
  if (i < GP_PAD_BUTTONS)
    return st->buttons >> i & 1;
  switch (i - GP_PAD_BUTTONS) {
  case 0:
    return st->hat == 8 || st->hat == 1 || st->hat == 2;
  case 1:
    return st->hat >= 4 && st->hat <= 6;
  case 2:
    return st->hat >= 6 && st->hat <= 8;
  default:
    return st->hat >= 2 && st->hat <= 4;
  }
}

bool gp_active(const struct gp_state *st) { return st->buttons || st->hat; }
