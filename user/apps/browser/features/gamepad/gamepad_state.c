#include "features/gamepad/gamepad_state.h"

#define KERNEL_FIELDS 11 /* SEQ BUTTONS HAT then X Y Z Rx Ry Rz Slider Dial */
#define ID_LABEL_MAX 40

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
  for (int i = 0; i < 8; i++)
    st->axes[i] = (int)v[3 + i];
  return true;
}

static const char *next_hex(const char *p, unsigned *v)
{
  int digits = 0;
  *v = 0;
  while (*p == ' ')
    p++;
  for (; digits < 4; digits++, p++) {
    char c = *p;
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0)
      break;
    *v = *v * 16 + (unsigned)d;
  }
  return digits ? p : 0;
}

bool gp_parse_id(const char *line, struct gp_id *id)
{
  static const struct {
    const char *token;
    enum gp_kind kind;
  } kinds[] = {
      {"generic", GP_GENERIC}, {"ds4", GP_DS4}, {"ds5", GP_DS5}, {"switchpro", GP_SWITCH_PRO}, {"xbox", GP_XBOX}};
  int k;
  for (k = 0; k < (int)(sizeof kinds / sizeof kinds[0]); k++) {
    int n = 0;
    while (kinds[k].token[n] && line[n] == kinds[k].token[n])
      n++;
    if (!kinds[k].token[n] && line[n] == ' ') {
      id->kind = kinds[k].kind;
      line += n;
      break;
    }
  }
  if (k == (int)(sizeof kinds / sizeof kinds[0]) || !(line = next_hex(line, &id->vid)) ||
      !(line = next_hex(line, &id->pid)))
    return false;
  while (*line == ' ')
    line++;
  int n = 0;
  for (; line[n] && line[n] != '\n' && n < ID_LABEL_MAX - 1; n++)
    id->label[n] = line[n];
  id->label[n] = 0;
  return true;
}

bool gp_standard(enum gp_kind kind) { return kind != GP_GENERIC; }
int gp_nbuttons(enum gp_kind kind) { return gp_standard(kind) ? GP_STD_BUTTONS : GP_RAW_BUTTONS; }
int gp_naxes(enum gp_kind kind) { return gp_standard(kind) ? GP_STD_AXES : GP_RAW_AXES; }

static double unit(int v)
{
  double a = v / 32768.0;
  return a > 1 ? 1 : a;
}

double gp_axis(enum gp_kind kind, const struct gp_state *st, int i)
{
  if (gp_standard(kind) && i == 3)
    i = 5; /* right stick Y is Rz in both layouts */
  return unit(st->axes[i]);
}

static bool hat_button(const struct gp_state *st, int dir)
{
  switch (dir) {
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

/* Raw button bit behind each standard button, -1 where the layout has none at that spot. */
static const signed char ds4_bits[GP_STD_BUTTONS] = {1, 2, 0, 3, 4, 5, -1, -1, 8, 9, 10, 11, -1, -1, -1, -1, 12};
static const signed char switch_bits[GP_STD_BUTTONS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, -1, -1, -1, 12};
static const signed char xbox_bits[GP_STD_BUTTONS] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, -1, -1, -1, -1, 10};

double gp_button(enum gp_kind kind, const struct gp_state *st, int i)
{
  if (!gp_standard(kind)) {
    if (i < GP_RAW_PAD_BUTTONS)
      return st->buttons >> i & 1;
    return hat_button(st, i - GP_RAW_PAD_BUTTONS);
  }
  if (i >= 12 && i <= 15)
    return hat_button(st, i - 12);
  if (kind != GP_SWITCH_PRO && (i == 6 || i == 7))
    return (st->axes[i - 3] + 32768) / 65535.0; /* the triggers are the Rx and Ry axes */
  int bit = (kind == GP_XBOX ? xbox_bits : kind == GP_SWITCH_PRO ? switch_bits : ds4_bits)[i];
  return bit >= 0 && (st->buttons >> bit & 1);
}

bool gp_active(const struct gp_state *st) { return st->buttons || st->hat; }
