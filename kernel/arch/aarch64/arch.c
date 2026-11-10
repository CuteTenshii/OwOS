/* arch.c: AArch64 platform support for the QEMU virt machine: PL011 UART,
 * stage-1 MMU, GICv2, generic timer and exception dispatch. */
#include "arch.h"

#define UARTDR 0x00
#define UARTFR 0x18
#define UARTFR_TXFF (1u << 5)
#define UARTIBRD 0x24
#define UARTFBRD 0x28
#define UARTLCRH 0x2c
#define UARTCR 0x30

void uart_init(void)
{
  mmio_write32(UART0_BASE + UARTCR, 0);
  mmio_write32(UART0_BASE + UARTLCRH, 3 << 5); /* 8N1, FIFOs off */
  mmio_write32(UART0_BASE + UARTCR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(char c)
{
  if (c == '\n')
    uart_putc('\r');
  while (mmio_read32(UART0_BASE + UARTFR) & UARTFR_TXFF)
    ;
  mmio_write32(UART0_BASE + UARTDR, (uint32_t)c);
}

void uart_puts(const char *s)
{
  while (*s)
    uart_putc(*s++);
}

void uart_puthex(uint64_t v)
{
  uart_puts("0x");
  for (int i = 60; i >= 0; i -= 4)
    uart_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

#define PTE_BLOCK 0x1ULL
#define PTE_ATTR(n) ((uint64_t)(n) << 2)
#define PTE_SH_ISH (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_XN ((1ULL << 53) | (1ULL << 54))
#define MAIR_DEVICE 0
#define MAIR_NORMAL 1

/* 39-bit VA, 4 KiB granule: the L1 table maps 1 GiB per entry, identity. */
uint64_t kernel_l1[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
  for (int i = 0; i < 4; i++) {
    uint64_t base = (uint64_t)i << 30;
    kernel_l1[i] = i == 1 ? (base | PTE_BLOCK | PTE_ATTR(MAIR_NORMAL) | PTE_SH_ISH | PTE_AF)
                          : (base | PTE_BLOCK | PTE_ATTR(MAIR_DEVICE) | PTE_AF | PTE_XN);
  }

  uint64_t mair = (0xffULL << (8 * MAIR_NORMAL));
  uint64_t ips = SYSREG_READ(id_aa64mmfr0_el1) & 7;
  uint64_t tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (ips << 32);

  __asm__ volatile("dsb ish");
  SYSREG_WRITE(mair_el1, mair);
  SYSREG_WRITE(tcr_el1, tcr);
  SYSREG_WRITE(ttbr0_el1, kernel_l1);
  __asm__ volatile("tlbi vmalle1\n\tdsb ish\n\tisb");

  uint64_t sctlr = SYSREG_READ(sctlr_el1);
  sctlr |= (1 << 0) | (1 << 2) | (1 << 12); /* M, C, I */
  SYSREG_WRITE(sctlr_el1, sctlr);
  __asm__ volatile("isb");
}

#define GICD_CTLR 0x000
#define GICD_ISENABLER0 0x100
#define GICD_IPRIORITYR 0x400
#define GICC_CTLR 0x000
#define GICC_PMR 0x004
#define GICC_IAR 0x00c
#define GICC_EOIR 0x010

void gic_init(void)
{
  mmio_write32(GICD_BASE + GICD_CTLR, 0);
  mmio_write32(GICD_BASE + GICD_IPRIORITYR + (IRQ_TIMER_PPI & ~3), 0x80u << (8 * (IRQ_TIMER_PPI & 3)));
  mmio_write32(GICD_BASE + GICD_ISENABLER0, 1u << IRQ_TIMER_PPI);
  mmio_write32(GICD_BASE + GICD_CTLR, 1);
  mmio_write32(GICC_BASE + GICC_PMR, 0xff);
  mmio_write32(GICC_BASE + GICC_CTLR, 1);
}

static volatile uint64_t ticks;
static uint64_t tick_interval;

void timer_init(void)
{
  tick_interval = SYSREG_READ(cntfrq_el0) / TICK_HZ;
  SYSREG_WRITE(cntp_tval_el0, tick_interval);
  SYSREG_WRITE(cntp_ctl_el0, 1);
}

uint64_t timer_ticks(void) { return ticks; }

static void irq_dispatch(void)
{
  uint32_t iar = mmio_read32(GICC_BASE + GICC_IAR);
  uint32_t id = iar & 0x3ff;
  if (id == IRQ_TIMER_PPI) {
    SYSREG_WRITE(cntp_tval_el0, tick_interval);
    ticks++;
  }
  if (id < 1020)
    mmio_write32(GICC_BASE + GICC_EOIR, iar);
}

static void exception_fatal(struct trap_frame *tf, uint64_t kind)
{
  uart_puts("\nexception kind=");
  uart_puthex(kind);
  uart_puts(" esr=");
  uart_puthex(SYSREG_READ(esr_el1));
  uart_puts(" elr=");
  uart_puthex(tf->elr);
  uart_puts(" far=");
  uart_puthex(SYSREG_READ(far_el1));
  uart_putc('\n');
  for (;;)
    __asm__ volatile("wfe");
}

volatile uint64_t svc_count;

/* kind = vector index: 0-3 current EL SP0, 4-7 current EL SPx, 8-11 lower EL A64, 12-15 lower EL A32;
 * within each group 0 sync, 1 irq, 2 fiq, 3 serror. */
void arch_exception(struct trap_frame *tf, uint64_t kind)
{
  uint64_t type = kind & 3;
  if (type == 1) {
    irq_dispatch();
    return;
  }
  uint64_t ec = SYSREG_READ(esr_el1) >> 26;
  if (type == 0 && kind >= 8 && kind < 12) {
    if (ec == 0x15)
      task_syscall(tf);
    else
      task_fault(tf);
    return;
  }
  if (type == 0 && ec == 0x15) { /* SVC from EL1 */
    svc_count++;
    return;
  }
  exception_fatal(tf, kind);
}

void arch_poweroff(void)
{
  register uint64_t x0 __asm__("x0") = 0x84000008; /* PSCI SYSTEM_OFF */
  if (boot_el == 2)
    __asm__ volatile("smc #0" : "+r"(x0));
  else
    __asm__ volatile("hvc #0" : "+r"(x0));
  for (;;)
    __asm__ volatile("wfi");
}

uintptr_t context_init(void *stack_top, void (*entry)(void))
{
  uint64_t *sp = (uint64_t *)((uintptr_t)stack_top - 96);
  for (int i = 0; i < 12; i++)
    sp[i] = 0;
  sp[11] = (uint64_t)entry; /* x30 */
  return (uintptr_t)sp;
}
