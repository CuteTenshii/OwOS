/* arch.h: 32-bit ARM (ARMv7-A) support layer interface. */
#ifndef OWOS_ARCH_ARM_H
#define OWOS_ARCH_ARM_H

#include "cpu_defs.h"
#include "fdt.h"
#include "mmio.h"
#include "plat.h"
#include "pmm.h"
#include <stdint.h>

#define IRQ_VTIMER_PPI 27u
#define TICK_HZ 100

/* Register state pushed by the exception vectors; layout matches vectors.S. */
struct trap_frame {
  uint32_t r[13];
  uint32_t lr;   /* SVC-mode lr of the interrupted code */
  uint32_t pc;   /* where the return resumes */
  uint32_t cpsr; /* restored on return */
};

_Static_assert(sizeof(struct trap_frame) == 64, "vectors.S pushes 14 registers plus srsdb's pair");

/* CP15 access by name: CP15_READ(SCTLR), CP15_WRITE(TTBR0, v). */
#define CP15_READ(reg) CP15_READ_(reg)
#define CP15_READ_(op1, crn, crm, op2)                                                                                 \
  ({                                                                                                                   \
    uint32_t v_;                                                                                                       \
    __asm__ volatile("mrc p15, " #op1 ", %0, " #crn ", " #crm ", " #op2 : "=r"(v_));                                   \
    v_;                                                                                                                \
  })
#define CP15_WRITE(reg, val) CP15_WRITE_(reg, val)
#define CP15_WRITE_(op1, crn, crm, op2, val)                                                                           \
  __asm__ volatile("mcr p15, " #op1 ", %0, " #crn ", " #crm ", " #op2 : : "r"((uint32_t)(val)) : "memory")

#define MIDR 0, c0, c0, 0
#define MPIDR 0, c0, c0, 5
#define SCTLR 0, c1, c0, 0
#define ACTLR 0, c1, c0, 1
#define TTBR0 0, c2, c0, 0
#define TTBCR 0, c2, c0, 2
#define DACR 0, c3, c0, 0
#define DFSR 0, c5, c0, 0
#define IFSR 0, c5, c0, 1
#define DFAR 0, c6, c0, 0
#define IFAR 0, c6, c0, 2
#define ICIALLU 0, c7, c5, 0
#define BPIALL 0, c7, c5, 6
#define TLBIALL 0, c8, c7, 0
#define CNTFRQ 0, c14, c0, 0
#define CNTV_TVAL 0, c14, c3, 0
#define CNTV_CTL 0, c14, c3, 1

#define SCTLR_M (1u << 0)
#define SCTLR_A (1u << 1)
#define SCTLR_C (1u << 2)
#define SCTLR_Z (1u << 11)
#define SCTLR_I (1u << 12)
#define SCTLR_TRE (1u << 28)
#define SCTLR_AFE (1u << 29)

/* The virtual counter, which the tick timer counts on. */
static inline uint64_t counter_now(void)
{
  uint32_t lo, hi;
  __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
  return (uint64_t)hi << 32 | lo;
}

extern uint32_t boot_dtb;
extern uint32_t boot_mode; /* PSR mode the loader entered the kernel in */

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);

/* Identity-map the 4 GiB address space in 1 MiB sections (RAM normal and cacheable, the rest device) and turn on the
 * MMU and caches. */
void mmu_init(const void *dtb);

void gic_init(void);
/* Acknowledge the highest priority interrupt; the low 10 bits are its id, 1020 and up are spurious. */
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

/* Start the tick; returns -1 if the firmware left the counter frequency unset. */
int timer_init(void);
uint32_t timer_ticks(void);

/* Size RAM from the DTB, then bring up the frame allocator and the kernel heap. Returns 0 on success. */
int arch_mem_init(const void *dtb);

void arch_exception(struct trap_frame *tf, uint32_t kind);
void arch_poweroff(void) __attribute__((noreturn));

/* SVCs taken from kernel mode; with no user mode, the self-test is the only caller. */
extern volatile uint32_t svc_count;

/* Armed by the self-test to survive one deliberate fault: the handler records it and skips the instruction. */
struct fault_probe {
  volatile int armed;
  volatile uint32_t kind;   /* TRAP_UNDEF, TRAP_PABT or TRAP_DABT */
  volatile uint32_t status; /* DFSR or IFSR, 0 for an undefined instruction */
  volatile uint32_t addr;   /* DFAR or IFAR, the instruction address for an undefined instruction */
};
extern struct fault_probe fault_probe;

/* Switch kernel stacks: saves callee-saved registers on the current stack,
 * stores sp in *old_sp, then resumes the context whose sp is new_sp. */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);
/* Build the initial context for a thread that starts at entry() on first switch. */
uintptr_t context_init(void *stack_top, void (*entry)(void *), void *arg);
void thread_trampoline(void);

/* Scheduler: round robin on the 100 Hz tick; a thread that returns from entry exits with code 0. */
int sched_init(void); /* returns -1 if the idle thread cannot be allocated */
void schedule(void);
int thread_create(const char *name, void (*entry)(void *), void *arg);
void thread_yield(void);
void thread_sleep(uint32_t ticks);
int thread_self(void);
int thread_join(int id); /* waits for exit, releases the thread, returns its exit code */
void thread_exit(int code) __attribute__((noreturn));

#endif
