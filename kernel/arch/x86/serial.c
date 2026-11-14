/* serial.c: minimal polled 16550 UART driver for COM1. */

#include "serial.h"
#include "io.h"

#define COM1 0x3F8
#define COM1_DATA (COM1 + 0) /* data / divisor low (DLAB)                  */
#define COM1_IER (COM1 + 1)  /* interrupt enable / divisor high (DLAB)     */
#define COM1_FCR (COM1 + 2)  /* FIFO control                               */
#define COM1_LCR (COM1 + 3)  /* line control                               */
#define COM1_MCR (COM1 + 4)  /* modem control                              */
#define COM1_LSR (COM1 + 5)  /* line status                                */
#define COM1_SCR (COM1 + 7)  /* scratch register (16450/16550 only)        */

#define LSR_THR_EMPTY 0x20 /* transmit holding register empty            */
#define LSR_DATA_RDY 0x01  /* a received byte is waiting in the RBR       */

/* Whether a real UART actually answers at 0x3F8. Crucial off-QEMU: a machine
 * with no 16550 (most laptops; a VM with no serial port) floats the port to
 * 0xFF, so LSR reads "data ready" forever and serial_poll would spin handing
 * the shell phantom 0xFF bytes, never letting the main loop drain the keyboard
 * ring, so the keyboard appears dead while IRQ-driven mouse/LEDs still work. */
static int present;

void serial_init(void)
{
  /* Probe before trusting the port: the scratch register round-trips a byte
   * on a real UART but reads back garbage (typically 0xFF) when absent. */
  outb(COM1_SCR, 0xAE);
  if (inb(COM1_SCR) != 0xAE)
    return; /* no UART: leave present=0, stay silent   */
  present = 1;

  outb(COM1_IER, 0x00);  /* no interrupts; we poll                  */
  outb(COM1_LCR, 0x80);  /* DLAB on: next writes set the divisor    */
  outb(COM1_DATA, 0x01); /* divisor 1 => 115200 baud                */
  outb(COM1_IER, 0x00);
  outb(COM1_LCR, 0x03); /* DLAB off, 8 bits, no parity, 1 stop     */
  outb(COM1_FCR, 0xC7); /* enable + clear FIFOs, 14-byte trigger   */
  outb(COM1_MCR, 0x0B); /* DTR, RTS, OUT2                          */
}

static void wait_tx(void)
{
  while ((inb(COM1_LSR) & LSR_THR_EMPTY) == 0)
    ;
}

void serial_putc(char c)
{
  if (!present) /* no UART: don't block on a dead port     */
    return;
  if (c == '\n') { /* terminals expect CR before LF           */
    wait_tx();
    outb(COM1_DATA, '\r');
  }
  wait_tx();
  outb(COM1_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
  while (*s)
    serial_putc(*s++);
}

/* Poll COM1 for an input character so the host terminal (qemu -serial stdio)
 * can drive the shell, not just mirror its output. serial_key turns the bytes
 * into the console codes the PS/2 keyboard produces. */
int serial_poll(char *out)
{
  if (!present) /* no UART: a floating port reads 0xFF, which */
    return 0;   /* looks like an endless stream of bytes      */

  while (inb(COM1_LSR) & LSR_DATA_RDY)
    if (serial_key(inb(COM1_DATA), out))
      return 1;
  return 0;
}
