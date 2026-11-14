/* serial_keys.c: turn the bytes a terminal sends over a serial line into console key codes (see serial.h). */
#include "keyboard.h"
#include "serial.h"

int serial_key(uint8_t c, char *out)
{
  /* 0: plain bytes; 1: after ESC; 2: after ESC [ or ESC O; 3: after ESC [ 3, expecting '~'. */
  static int esc;

  if (esc == 1) { /* after ESC: a CSI introducer or give up */
    esc = (c == '[' || c == 'O') ? 2 : 0;
    return 0;
  }
  if (esc == 2) {   /* ESC [ <final or digit> */
    if (c == '3') { /* ESC [ 3 ~ is Delete */
      esc = 3;
      return 0;
    }
    esc = 0;
    switch (c) {
    case 'A':
      *out = KEY_UP;
      return 1;
    case 'B':
      *out = KEY_DOWN;
      return 1;
    case 'C':
      *out = KEY_RIGHT;
      return 1;
    case 'D':
      *out = KEY_LEFT;
      return 1;
    case 'H':
      *out = KEY_HOME;
      return 1;
    case 'F':
      *out = KEY_END;
      return 1;
    }
    return 0; /* unrecognized CSI: swallow it */
  }
  if (esc == 3) {
    esc = 0;
    if (c == '~') {
      *out = KEY_DELETE;
      return 1;
    }
    return 0;
  }

  switch (c) {
  case 0x1B:
    esc = 1;
    return 0;
  case '\r': /* Enter sends CR */
    *out = '\n';
    return 1;
  case 0x7F: /* the DEL key is backspace */
    *out = '\b';
    return 1;
  case 0x03: /* Ctrl+C */
    keyboard_signal_interrupt();
    return 0;
  case 0x0C: /* Ctrl+L */
    *out = KEY_CLEAR;
    return 1;
  case 0x15: /* Ctrl+U */
    *out = KEY_KILL;
    return 1;
  default:
    *out = (char)c;
    return 1;
  }
}
