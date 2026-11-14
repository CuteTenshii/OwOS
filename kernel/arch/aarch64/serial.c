/* serial.c: the shared serial.h API on the boot UART (uart.c), which mirrors output to the framebuffer console. */
#include "serial.h"
#include "arch.h"

/* uart.c sets the UART up while the platform is probed, before anything prints. */
void serial_init(void) {}

void serial_putc(char c) { uart_putc(c); }

void serial_write(const char *s) { uart_puts(s); }

int serial_poll(char *out)
{
  char c;
  while (uart_getc_nb(&c))
    if (serial_key((uint8_t)c, out))
      return 1;
  return 0;
}
