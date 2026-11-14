/* serial.h: the polled console UART (COM1 on x86, the board's UART on AArch64), for headless logging and input.
 *
 * The console mirrors everything here, so a full session can be captured
 * over `qemu ... -serial stdio` without screenshotting the framebuffer.
 */
#ifndef OWOS_SERIAL_H
#define OWOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Non-blocking: translate waiting UART input into a console key code and
 * store it in *out, returning 1; or return 0 if none. Lets the host terminal
 * type into the shell over `qemu -serial stdio`. */
int serial_poll(char *out);

/* Feed one received byte through the terminal decoder (serial_keys.c): 1 with
 * a key code in *out, or 0 when the byte only advanced an escape sequence or
 * raised the Ctrl+C latch (keyboard_signal_interrupt). */
int serial_key(uint8_t c, char *out);

#endif /* OWOS_SERIAL_H */
