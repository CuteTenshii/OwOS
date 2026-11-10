/* arch.h: AArch64 support layer interface (QEMU virt machine). */
#ifndef OWOS_ARCH_AARCH64_H
#define OWOS_ARCH_AARCH64_H

#include "cpu_defs.h"
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
extern uint64_t mmu_regs[3]; /* MAIR, TCR, TTBR0 handed to secondary CPUs */

static inline int cpu_id(void) { return (int)(SYSREG_READ(mpidr_el1) & 0xff); }

/* Secondary CPUs: psci_cpu_on starts one at secondary_entry; smp_start returns how many CPUs are online. */
void secondary_entry(void);
void secondary_main(int cpu);
int psci_cpu_on(int cpu);
int smp_start(void);
int smp_online(void);
void gic_cpu_init(void);
void sched_cpu_init(int cpu);
void sched_new_thread(void);

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
static inline int user_range(uint64_t ptr, uint64_t len)
{
  return ptr >= USER_BASE && ptr < USER_END && len <= USER_END - ptr;
}

#define USER_STACK_PAGES 4
#define USER_STACK_BOTTOM (USER_END - USER_STACK_PAGES * 0x1000UL)

#define VM_USER_RX 1
#define VM_USER_RW 2
#define VM_USER_RO 3

/* Per-process translation tables: the kernel identity map plus a user window at USER_BASE. */
uint64_t *vm_create(void);
int vm_map(uint64_t *root, uintptr_t va, uintptr_t pa, int prot);
void vm_switch(uint64_t *root);
void vm_destroy(uint64_t *root);

/* A ready-to-run user address space: page tables with the program and stack mapped. */
struct user_image {
  uint64_t *root;
  uint64_t entry, sp;
};

/* Build an image from a flat code blob mapped at USER_BASE, or from an ELF64 AArch64 executable.
 * Both return 0 on success and leave nothing allocated on failure. */
int user_image_code(struct user_image *img, const void *code, uint32_t len);
int elf_load(struct user_image *img, const void *file, uint64_t size);

/* EL0 tasks. task_run returns the exit code, or -1 if the task faulted. */
int task_run(const void *code, uint32_t len);
/* Same for an ELF file; returns 0 and stores the exit code in *code, or -1 if the file was rejected. */
int task_run_elf(const void *file, uint64_t size, int *code);
void syscall_dispatch(struct trap_frame *tf);
void task_fault(struct trap_frame *tf);

void arch_exception(struct trap_frame *tf, uint64_t kind);
void arch_poweroff(void);

/* Switch kernel stacks: saves callee-saved registers on the current stack,
 * stores sp in *old_sp, then resumes the context whose sp is new_sp. */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);
/* Build the initial context for a thread that starts at entry() on first switch. */
uintptr_t context_init(void *stack_top, void (*entry)(void *), void *arg);
void thread_trampoline(void);

/* Scheduler: round robin on the 100 Hz tick; a thread that returns from entry exits with code 0. */
void sched_init(void);
void schedule(void);
int thread_create(const char *name, void (*entry)(void *), void *arg);
/* Takes ownership of img->root: it is destroyed when the thread is joined, or here on failure. */
int thread_create_user(const char *name, const struct user_image *img);
void thread_yield(void);
void thread_sleep(uint32_t ticks);
int thread_self(void);
int thread_join(int id); /* waits for exit, releases the thread, returns its exit code */
void thread_exit(int code) __attribute__((noreturn));

#endif
