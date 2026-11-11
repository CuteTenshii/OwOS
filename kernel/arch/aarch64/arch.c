/* arch.c: AArch64 platform support: stage-1 MMU, generic timer, exception dispatch and PSCI. */
#include "arch.h"
#include <stdbool.h>

#define PTE_BLOCK 0x1ULL
#define PTE_ATTR(n) ((uint64_t)(n) << 2)
#define PTE_SH_ISH (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_XN ((1ULL << 53) | (1ULL << 54))
#define MAIR_DEVICE 0
#define MAIR_NORMAL 1

/* 39-bit VA, 4 KiB granule: the L1 table maps 1 GiB per entry, identity. */
uint64_t mmu_regs[3];
uint64_t kernel_l1[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
  uint64_t ram_base = 0x40000000, ram_size = 1ULL << 30;
  fdt_memory((const void *)boot_dtb, &ram_base, &ram_size);
  for (int i = 0; i < 4; i++) {
    uint64_t base = (uint64_t)i << 30;
    bool ram = ram_base < base + (1ULL << 30) && ram_base + ram_size > base;
    kernel_l1[i] = ram ? (base | PTE_BLOCK | PTE_ATTR(MAIR_NORMAL) | PTE_SH_ISH | PTE_AF)
                       : (base | PTE_BLOCK | PTE_ATTR(MAIR_DEVICE) | PTE_AF | PTE_XN);
  }

  uint64_t mair = (0xffULL << (8 * MAIR_NORMAL));
  uint64_t ips = SYSREG_READ(id_aa64mmfr0_el1) & 7;
  uint64_t tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (ips << 32);

  mmu_regs[0] = mair;
  mmu_regs[1] = tcr;
  mmu_regs[2] = (uint64_t)(uintptr_t)kernel_l1;
  for (int i = 0; i < 3; i++) /* secondaries read these with their MMU and caches off */
    __asm__ volatile("dc civac, %0" : : "r"(&mmu_regs[i]) : "memory");
  for (int i = 0; i < 4; i++)
    __asm__ volatile("dc civac, %0" : : "r"(&kernel_l1[i]) : "memory");
  __asm__ volatile("dsb sy");
  SYSREG_WRITE(mair_el1, mair);
  SYSREG_WRITE(tcr_el1, tcr);
  SYSREG_WRITE(ttbr0_el1, kernel_l1);
  __asm__ volatile("tlbi vmalle1\n\tdsb ish\n\tisb");

  uint64_t sctlr = SYSREG_READ(sctlr_el1);
  sctlr |= (1 << 0) | (1 << 2) | (1 << 12); /* M, C, I */
  SYSREG_WRITE(sctlr_el1, sctlr);
  __asm__ volatile("isb");
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
  uint32_t iar = gic_ack();
  uint32_t id = iar & 0x3ff;
  if (id == IRQ_TIMER_PPI) {
    SYSREG_WRITE(cntp_tval_el0, tick_interval);
    if (cpu_id() == 0)
      ticks++;
  }
  if (id < 1020)
    gic_eoi(iar);
  if (id == IRQ_TIMER_PPI)
    schedule();
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
      syscall_dispatch(tf);
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

#define PSCI_CPU_ON 0xc4000003UL
#define PSCI_SYSTEM_OFF 0x84000008UL

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
  register uint64_t x0 __asm__("x0") = fn;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  if (plat.psci_smc < 0 ? boot_el == 2 : plat.psci_smc)
    __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
  else
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
  return (int64_t)x0;
}

int psci_cpu_on(int cpu)
{
  return (int)psci_call(PSCI_CPU_ON, plat.cpu_mpidr[cpu], (uint64_t)(uintptr_t)secondary_entry, (uint64_t)cpu);
}

void (*arch_poweroff_fallback)(void);

void arch_poweroff(void)
{
  psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
  if (arch_poweroff_fallback)
    arch_poweroff_fallback();
  for (;;)
    __asm__ volatile("wfi");
}

uintptr_t context_init(void *stack_top, void (*entry)(void *), void *arg)
{
  uint64_t *sp = (uint64_t *)((uintptr_t)stack_top - 96);
  for (int i = 0; i < 12; i++)
    sp[i] = 0;
  sp[0] = (uint64_t)entry;              /* x19, called by the trampoline */
  sp[1] = (uint64_t)arg;                /* x20 */
  sp[11] = (uint64_t)thread_trampoline; /* x30 */
  return (uintptr_t)sp;
}
