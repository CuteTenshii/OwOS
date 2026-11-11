/* main.c: ARMv7-A bring-up: MMU, console, memory, scheduler and interrupts, then the self-test. */
#include "arch.h"
#include "kstring.h"

int selftest_run(void);

static const char *mode_name(uint32_t mode)
{
  return mode == PSR_MODE_HYP ? "HYP" : mode == PSR_MODE_SVC ? "SVC" : "an unexpected mode";
}

static const char *psci_conduit(void)
{
  if (plat.psci_smc < 0)
    return "by boot mode";
  return plat.psci_smc ? "smc" : "hvc";
}

static void halt_with(const char *why)
{
  uart_puts(why);
  arch_poweroff();
}

/* The MMU goes on before the first print: the console lock uses exclusive loads and stores, which are only defined
 * on normal cacheable memory. */
void arch_main(void)
{
  const void *dtb = (const void *)(uintptr_t)boot_dtb;
  char line[112];

  plat_init(dtb);
  mmu_init(dtb);
  uart_init();
  ksnprintf(line, sizeof line, "OwOS arm boot, %s -> SVC, MIDR %x, DTB %x\n", mode_name(boot_mode), CP15_READ(MIDR),
            boot_dtb);
  uart_puts(line);
  ksnprintf(line, sizeof line, "GICv2 dist %x cpu %x, uart pl011 %x, psci %s\n", plat.gicd_base, plat.gicc_base,
            plat.uart_base, psci_conduit());
  uart_puts(line);

  if (arch_mem_init(dtb))
    halt_with("no usable memory map in the DTB\n");
  ksnprintf(line, sizeof line, "RAM %u KiB, free %u KiB\n", pmm_installed_kib(), pmm_free_kib());
  uart_puts(line);
  if (sched_init())
    halt_with("no memory for the idle thread\n");
  gic_init();
  if (timer_init())
    halt_with("the firmware left the generic timer frequency unset\n");
  __asm__ volatile("cpsie i" ::: "memory");

  selftest_run();
  arch_poweroff();
}
