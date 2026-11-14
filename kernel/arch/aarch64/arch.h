/* arch.h: AArch64 support layer interface: the board (plat.h), system registers, CPUs, the interrupt controller and
 * the boot UART. The shared kernel's arch interfaces (paging.h, regs.h, smp.h, fpu.h, task_arch.h) sit beside it. */
#ifndef OWOS_ARCH_AARCH64_H
#define OWOS_ARCH_AARCH64_H

#include "cpu_defs.h"
#include "fdt.h"
#include "mmio.h"
#include "plat.h"
#include "pmm.h"
#include <stdint.h>

#define IRQ_TIMER_PPI 30

#define SYSREG_READ(reg)                                                                                               \
  ({                                                                                                                   \
    uint64_t v_;                                                                                                       \
    __asm__ volatile("mrs %0, " #reg : "=r"(v_));                                                                      \
    v_;                                                                                                                \
  })
#define SYSREG_WRITE(reg, val) __asm__ volatile("msr " #reg ", %0" : : "r"((uint64_t)(val)))

/* The kernel identity-maps physical memory below 4 GiB, so a frame is reached at its own address. */
#ifndef PHYS_TO_PTR
#define PHYS_TO_PTR(phys) ((void *)(uintptr_t)(phys))
#endif

extern uint64_t boot_el;
extern uint64_t boot_dtb;
extern uint64_t mmu_regs[3]; /* MAIR, TCR, TTBR0 handed to secondary CPUs */
extern uint64_t kernel_l1[512];

/* Index of this CPU in plat.cpu_mpidr; the boot CPU is 0. */
static inline int cpu_id(void)
{
  uint64_t self = SYSREG_READ(mpidr_el1) & FDT_MPIDR_AFFINITY;
  for (int i = 1; i < plat.ncpus; i++)
    if (plat.cpu_mpidr[i] == self)
      return i;
  return 0;
}

/* Secondary CPUs: psci_cpu_on starts one at secondary_entry (boot.S), which calls secondary_main; smp_start returns
 * how many CPUs are online. */
void secondary_entry(void);
void secondary_main(int cpu);
int psci_cpu_on(int cpu);
int smp_start(void);

void gic_init(void);
void gic_cpu_init(void);
/* Acknowledge the highest priority interrupt; the low 10 bits are its id, 1020 and up are spurious. */
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t v);
/* A received byte into *out: 1, or 0 if none is waiting. */
int uart_getc_nb(char *out);

void mmu_init(void);

/* Note this CPU's MIDR for cpu_brand (cpu.c); each CPU calls it once as it comes up. */
void cpu_record(int cpu);

/* Size RAM from the DTB, then bring up the frame allocator and the kernel heap. Returns 0 on success. */
int arch_mem_init(const void *dtb);

struct registers;
void arch_exception(struct registers *r, uint64_t kind);

/* PSCI SYSTEM_OFF / SYSTEM_RESET; each returns only if the firmware refused. */
void arch_poweroff(void);
void arch_reset(void);
/* Tried by arch_poweroff when PSCI SYSTEM_OFF returns; a board driver such as the PMIC's sets it. */
extern void (*arch_poweroff_fallback)(void);

#endif
