/* serial.h — polled COM1 (16550 UART) output for headless logging.
 *
 * The VGA console mirrors everything here, so a full session can be captured
 * over `qemu ... -serial stdio` without screenshotting the framebuffer.
 */
#ifndef OWOS_SERIAL_H
#define OWOS_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Non-blocking: translate one waiting COM1 input byte into a console key code
 * (see serial.c) and store it in *out, returning 1; or return 0 if none. Lets
 * the host terminal type into the shell over `qemu -serial stdio`. */
int serial_poll(char *out);

#endif /* OWOS_SERIAL_H */
