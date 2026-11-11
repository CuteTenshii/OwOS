/* main.c: AArch64 bring-up: console, MMU, memory, scheduler, interrupts and the other CPUs. */
#include "arch.h"

int selftest_run(void);

void arch_main(void)
{
  plat_init((const void *)boot_dtb);
  uart_init();
  uart_puts("OwOS aarch64 boot, EL");
  uart_putc('0' + (char)boot_el);
  uart_puts(" -> EL1, MIDR ");
  uart_puthex(SYSREG_READ(midr_el1));
  uart_putc('\n');
  uart_puts(plat.gic_kind == GIC_V3 ? "GICv3, cpus " : "GICv2, cpus ");
  uart_puthex((uint64_t)plat.ncpus);
  uart_puts(plat.uart_kind == UART_PL011 ? ", uart pl011, " : ", uart mtk, ");
  uart_puts(plat.psci_smc < 0 ? "psci by exception level" : plat.psci_smc ? "psci smc" : "psci hvc");
  uart_putc('\n');

  mmu_init();
  if (arch_mem_init((const void *)boot_dtb)) {
    uart_puts("no usable memory map in the DTB\n");
    arch_poweroff();
  }
  uart_puts("RAM free KiB ");
  uart_puthex(pmm_free_kib());
  uart_putc('\n');
  sched_init();
  gic_init();
  timer_init();
  __asm__ volatile("msr daifclr, #2");
  smp_start();

  selftest_run();
  arch_poweroff();
}
