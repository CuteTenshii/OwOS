/* uart.c: boot console on a PL011 (QEMU virt) or a 16550-style UART with 32-bit register spacing (MediaTek, Tegra) left
 * configured by the loader, mirrored to the framebuffer. */
#include "arch.h"
#include "fbcon.h"
#include "spinlock.h"
#include <stdbool.h>

#define UARTDR 0x00
#define UARTFR 0x18
#define UARTFR_TXFF (1u << 5)
#define UARTIBRD 0x24
#define UARTFBRD 0x28
#define UARTLCRH 0x2c
#define UARTCR 0x30

#define NS_THR 0x00
#define NS_LSR 0x14
#define NS_LSR_THRE (1u << 5)
#define NS_TX_POLLS 100000

void uart_init(void)
{
  if (plat.uart_kind != UART_PL011)
    return;
  mmio_write32(plat.uart_base + UARTCR, 0);
  mmio_write32(plat.uart_base + UARTLCRH, 3 << 5); /* 8N1, FIFOs off */
  mmio_write32(plat.uart_base + UARTCR, (1 << 0) | (1 << 8) | (1 << 9));
}

static spinlock_t uart_lock;

static bool ns_dead;

/* A UART the loader left gated never raises THRE: give up on it instead of hanging the boot. */
static void ns_write(char c)
{
  if (ns_dead)
    return;
  for (int i = 0; i < NS_TX_POLLS; i++)
    if (mmio_read32(plat.uart_base + NS_LSR) & NS_LSR_THRE) {
      mmio_write32(plat.uart_base + NS_THR, (uint32_t)c);
      return;
    }
  ns_dead = true;
}

static void uart_write(char c)
{
  if (c == '\n')
    uart_write('\r');
  fbcon_putc(c);
  if (plat.uart_kind == UART_NONE)
    return;
  if (plat.uart_kind == UART_PL011) {
    while (mmio_read32(plat.uart_base + UARTFR) & UARTFR_TXFF)
      ;
    mmio_write32(plat.uart_base + UARTDR, (uint32_t)c);
  } else {
    ns_write(c);
  }
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

void uart_puthex(uint64_t v)
{
  uintptr_t f = spin_lock_irqsave(&uart_lock);
  uart_write('0');
  uart_write('x');
  for (int i = 60; i >= 0; i -= 4)
    uart_write("0123456789abcdef"[(v >> i) & 0xf]);
  spin_unlock_irqrestore(&uart_lock, f);
}
