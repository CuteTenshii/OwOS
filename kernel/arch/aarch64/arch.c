/* arch.c: AArch64 platform support: the stage-1 MMU and PSCI. */
#include "arch.h"
#include <stdbool.h>

#define PTE_BLOCK 0x1ULL
#define PTE_ATTR(n) ((uint64_t)(n) << 2)
#define PTE_SH_ISH (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_UXN (1ULL << 54)
#define PTE_XN ((1ULL << 53) | PTE_UXN)
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
    /* EL0 fetches obey UXN alone, not the EL1-only access bits, so kernel memory must say it explicitly. */
    kernel_l1[i] = ram ? (base | PTE_BLOCK | PTE_ATTR(MAIR_NORMAL) | PTE_SH_ISH | PTE_AF | PTE_UXN)
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

#define PSCI_CPU_ON 0xc4000003UL
#define PSCI_SYSTEM_OFF 0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL

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
}

void arch_reset(void) { psci_call(PSCI_SYSTEM_RESET, 0, 0, 0); }
