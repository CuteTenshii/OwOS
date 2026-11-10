/* arch.h: AArch64 support layer interface (QEMU virt machine). */
#ifndef OWOS_ARCH_AARCH64_H
#define OWOS_ARCH_AARCH64_H

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

#define PMM_FRAME_SIZE 4096

extern uint64_t boot_dtb;

/* Base and size of the first /memory node. Returns 0 on success, -1 if the blob is invalid. */
int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size);

void pmm_init(uint64_t ram_base, uint64_t ram_size);
uintptr_t pmm_alloc_frame(void); /* 0 when out of memory */
void pmm_free_frame(uintptr_t frame);
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

void arch_exception(struct trap_frame *tf, uint64_t kind);
void arch_poweroff(void);

/* Switch kernel stacks: saves callee-saved registers on the current stack,
 * stores sp in *old_sp, then resumes the context whose sp is new_sp. */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);
/* Build the initial context for a thread that starts at entry() on first switch. */
uintptr_t context_init(void *stack_top, void (*entry)(void));

#endif
