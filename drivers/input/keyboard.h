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
#define KEY_INTR    0x18   /* Ctrl+C: interrupt (delivered to the focused app) */
#define KEY_KILL    0x0B   /* Ctrl+U: erase the whole line                     */
#define KEY_CLEAR   0x0C   /* Ctrl+L: clear the screen                         */
#define KEY_F5      0x0E   /* F5: app-defined refresh/reload                   */
#define KEY_SUPER   0x11   /* left/right Windows (GUI) key: toggle Start menu  */

/* KEY_KILL/KEY_CLEAR remain ONLY as line-editor codes for the serial recovery
 * console (serial.c maps Ctrl+U/L to them — serial has no modifier channel). The
 * PS/2 keyboard no longer emits them: it sends the bare letter + KMOD_CTRL and
 * the consumer (shell/editor) decides. */
/* Menu accelerators: Ctrl+<letter> chords a windowed app binds to its menu
 * items (the keyboard forwards them like any other editing key; the focused
 * app decides what each means -- typically New/Open/Save). The shell ignores
 * them, as it does any unrecognised control byte. */
#define KEY_NEW     0x0F   /* Ctrl+N                                           */
#define KEY_OPEN    0x10   /* Ctrl+O                                           */
#define KEY_SAVE    0x13   /* Ctrl+S                                           */
/* Ctrl+C is not queued here -- it is a latch read via keyboard_take_interrupt
 * so it can interrupt a running command, not just the line being typed.       */
/* Text-editing chords for a windowed editor (Ctrl+A select-all, Ctrl+X cut).
 * The shell's line editor treats KEY_SELALL like Home (beginning of line). */
#define KEY_SELALL  0x12   /* Ctrl+A: select all                               */
#define KEY_CUT     0x14   /* Ctrl+X: cut the selection                        */
/* Shift+navigation: the same arrow/Home/End cluster with Shift held, so an app
 * can extend a selection toward the new caret position. The shell ignores them. */
#define KEY_SLEFT   0x15
#define KEY_SRIGHT  0x16
#define KEY_SUP     0x17
#define KEY_SDOWN   0x19
#define KEY_SHOME   0x1A
#define KEY_SEND    0x1C
#define KEY_TAB         '\t'   /* WM keys off KEY_TAB + KMOD_ALT for Alt+Tab      */

/* Alt+Tab is delivered as KEY_TAB+KMOD_ALT (+KMOD_SHIFT reverses); the WM owns
 * switching. KEY_ALT_UP is emitted on Alt-release to commit the selection. */
#define KEY_ALT_UP      0x1F

/* Modifier state delivered alongside each key. Every key event carries the set
 * of modifiers held when it was pressed, so an app can act on chords (e.g. a
 * browser turns Alt+Left into "go back") without the keyboard inventing a
 * dedicated code per chord. Delivered to GUI apps in win_event.b (see owos.h);
 * keep these values in sync with the KMOD_* in owos.h. */
#define KMOD_SHIFT 0x01
#define KMOD_CTRL  0x02
#define KMOD_ALT   0x04
#define KMOD_SUPER 0x08
#define KMOD_ALTGR 0x10

void keyboard_init(void);

/* Non-blocking: stores the next buffered character in *out and returns 1,
 * or returns 0 if no input is waiting. When `mods` is non-NULL it receives the
 * KMOD_* modifier set held when that key was pressed (0 for injected paste). */
int keyboard_poll(char *out, unsigned char *mods);

/* Switch the active keyboard layout by name ("us" or "fr"). Returns 0 on
 * success, -1 if the name matches no known layout. */
int keyboard_set_layout(const char *name);
const char *keyboard_layout_name(void);   /* current layout name ("us"/"fr")    */

/* Returns 1 and clears the latch if Ctrl+C has been pressed since the last
 * call; 0 otherwise. */
int keyboard_take_interrupt(void);

/* Raise the interrupt latch from non-IRQ context (used by the serial console
 * so a Ctrl+C typed in the host terminal interrupts a running command). */
void keyboard_signal_interrupt(void);

/* Arm a clipboard paste from outside the keyboard (the WM, after the Super+V
 * history viewer picks a clip) — same injection path as Ctrl+V. */
void keyboard_request_paste(void);

#endif /* OWOS_KEYBOARD_H */
