/* keyboard.c: the console keyboard (keyboard.h) on boards whose only input is the UART: keys come from the terminal
 * on the other end of the line, decoded by serial_poll, so programs reading the console get what is typed there. */
#include "keyboard.h"
#include "serial.h"
#include <stdbool.h>

static volatile bool int_pending;

int keyboard_poll(char *out, unsigned char *mods)
{
  if (!serial_poll(out))
    return 0;
  if (mods)
    *mods = 0; /* a terminal sends characters, not modifier state */
  return 1;
}

int keyboard_poll_text(char *out, unsigned char *mods)
{
  while (keyboard_poll(out, mods))
    if ((unsigned char)*out < KEY_MEDIA_PLAY)
      return 1;
  return 0;
}

void keyboard_signal_interrupt(void) { int_pending = true; }

int keyboard_take_interrupt(void)
{
  bool v = int_pending;
  int_pending = false;
  return v;
}

/* No Print Screen key reaches a serial line. */
int keyboard_take_screenshot(void) { return 0; }

/* The terminal applies its own layout before sending, so only the identity layout makes sense here. */
int keyboard_set_layout(const char *name) { return name && name[0] == 'u' && name[1] == 's' && !name[2] ? 0 : -1; }
const char *keyboard_layout_name(void) { return "us"; }
