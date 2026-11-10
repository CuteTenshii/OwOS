/* main.c: AArch64 bring-up: console, MMU, memory, scheduler, interrupts and the other CPUs. */
#include "arch.h"

int selftest_run(void);

void arch_main(void)
{
  uart_init();
  uart_puts("OwOS aarch64 boot, EL");
  uart_putc('0' + (char)boot_el);
  uart_puts(" -> EL1, MIDR ");
  uart_puthex(SYSREG_READ(midr_el1));
  uart_putc('\n');

  mmu_init();
  if (arch_mem_init((const void *)boot_dtb)) {
    uart_puts("no usable memory map in the DTB\n");
    arch_poweroff();
  }
  sched_init();
  gic_init();
  timer_init();
  __asm__ volatile("msr daifclr, #2");
  smp_start();

  selftest_run();
  arch_poweroff();
}
