/* arch.h: AArch64 support layer interface (QEMU virt machine). */
#ifndef OWOS_ARCH_AARCH64_H
#define OWOS_ARCH_AARCH64_H

#include "pmm.h"
#include <stdint.h>

#define UART0_BASE 0x09000000UL
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define IRQ_TIMER_PPI 30
#define TICK_HZ 100

/* Register state pushed by the exception vectors; layout matches vectors.S. */
struct trap_frame {
  uint64_t x[31];
  uint64_t sp_el0;
  uint64_t elr;
  uint64_t spsr;
};

static inline uint32_t mmio_read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void mmio_write32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#define SYSREG_READ(reg)                                                                                               \
  ({                                                                                                                   \
    uint64_t v_;                                                                                                       \
    __asm__ volatile("mrs %0, " #reg : "=r"(v_));                                                                      \
    v_;                                                                                                                \
  })
#define SYSREG_WRITE(reg, val) __asm__ volatile("msr " #reg ", %0" : : "r"((uint64_t)(val)))

extern uint64_t boot_el;

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t v);

void mmu_init(void);
void gic_init(void);
void timer_init(void);
uint64_t timer_ticks(void);

extern uint64_t boot_dtb;

/* Base and size of the first /memory node. Returns 0 on success, -1 if the blob is invalid. */
int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size);

/* Size RAM from the DTB, then bring up the frame allocator and the kernel heap. Returns 0 on success. */
int arch_mem_init(const void *dtb);

extern uint64_t kernel_l1[512];

#define USER_BASE 0x100000000UL
#define USER_END (USER_BASE + 0x200000UL)
#define VM_USER_RX 1
#define VM_USER_RW 2

/* Per-process translation tables: the kernel identity map plus a user window at USER_BASE. */
uint64_t *vm_create(void);
int vm_map(uint64_t *root, uintptr_t va, uintptr_t pa, int prot);
void vm_switch(uint64_t *root);
void vm_destroy(uint64_t *root);

/* EL0 tasks. task_run returns the exit code, or -1 if the task faulted. */
int task_run(const void *code, uint32_t len);
void task_syscall(struct trap_frame *tf);
void task_fault(struct trap_frame *tf);

#define SYS_WRITE 1
#define SYS_EXIT 2

void arch_exception(struct trap_frame *tf, uint64_t kind);
void arch_poweroff(void);

/* Switch kernel stacks: saves callee-saved registers on the current stack,
 * stores sp in *old_sp, then resumes the context whose sp is new_sp. */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);
/* Build the initial context for a thread that starts at entry() on first switch. */
uintptr_t context_init(void *stack_top, void (*entry)(void));

#endif
