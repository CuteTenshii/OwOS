/* keyboard.h — PS/2 keyboard driver (IRQ1).
 *
 * keyboard_init() claims IRQ1 and unmasks it. The interrupt handler only
 * translates each keypress to ASCII and pushes it into an internal ring
 * buffer; consumers (e.g. the shell) drain it from normal context with
 * keyboard_poll().
 */
#ifndef OWOS_KEYBOARD_H
#define OWOS_KEYBOARD_H

/* Editing keys are delivered through keyboard_poll() as single bytes, using
 * control-code values that none of the keymaps ever emit (so they can't be
 * confused with real typed characters). The shell's line editor interprets
 * them; any other consumer can ignore bytes below 0x20. */
#define KEY_UP      0x01
#define KEY_DOWN    0x02
#define KEY_LEFT    0x03
#define KEY_RIGHT   0x04
#define KEY_HOME    0x05
#define KEY_END     0x06
#define KEY_DELETE  0x07
#define KEY_KILL    0x0B   /* Ctrl+U: erase the whole line                     */
#define KEY_CLEAR   0x0C   /* Ctrl+L: clear the screen                         */
/* Ctrl+C is not queued here -- it is a latch read via keyboard_take_interrupt
 * so it can interrupt a running command, not just the line being typed.       */

void keyboard_init(void);

/* Non-blocking: stores the next buffered character in *out and returns 1,
 * or returns 0 if no input is waiting. */
int keyboard_poll(char *out);

/* Switch the active keyboard layout by name ("us" or "fr"). Returns 0 on
 * success, -1 if the name matches no known layout. */
int keyboard_set_layout(const char *name);

/* Returns 1 and clears the latch if Ctrl+C has been pressed since the last
 * call; 0 otherwise. */
int keyboard_take_interrupt(void);

/* Raise the interrupt latch from non-IRQ context (used by the serial console
 * so a Ctrl+C typed in the host terminal interrupts a running command). */
void keyboard_signal_interrupt(void);

#endif /* OWOS_KEYBOARD_H */
