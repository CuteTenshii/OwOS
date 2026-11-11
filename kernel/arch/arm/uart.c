/* uart.c: boot console on the PL011, polled. */
#include "arch.h"
#include "spinlock.h"

#define UARTDR 0x00
#define UARTFR 0x18
#define UARTFR_TXFF (1u << 5)
#define UARTLCRH 0x2c
#define UARTCR 0x30
#define UARTCR_UARTEN (1u << 0)
#define UARTCR_TXE (1u << 8)
#define UARTCR_RXE (1u << 9)
#define UARTLCRH_8BIT (3u << 5)

static spinlock_t uart_lock;

/* The baud rate divisors are left as the firmware set them. */
void uart_init(void)
{
  mmio_write32(plat.uart_base + UARTCR, 0);
  mmio_write32(plat.uart_base + UARTLCRH, UARTLCRH_8BIT);
  mmio_write32(plat.uart_base + UARTCR, UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE);
}

static void uart_write(char c)
{
  if (c == '\n')
    uart_write('\r');
  while (mmio_read32(plat.uart_base + UARTFR) & UARTFR_TXFF)
    ;
  mmio_write32(plat.uart_base + UARTDR, (uint32_t)(unsigned char)c);
}

void uart_putc(char c)
{
  uintptr_t f = spin_lock_irqsave(&uart_lock);
  uart_write(c);
  spin_unlock_irqrestore(&uart_lock, f);
}

void uart_puts(const char *s)
{
  uintptr_t f = spin_lock_irqsave(&uart_lock);
  while (*s)
    uart_write(*s++);
  spin_unlock_irqrestore(&uart_lock, f);
}

void uart_puthex(uint32_t v)
{
  uintptr_t f = spin_lock_irqsave(&uart_lock);
  uart_write('0');
  uart_write('x');
  for (int i = 28; i >= 0; i -= 4)
    uart_write("0123456789abcdef"[(v >> i) & 0xf]);
  spin_unlock_irqrestore(&uart_lock, f);
}
