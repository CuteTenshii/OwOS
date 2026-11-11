/* mmu.c: short-descriptor translation tables: a flat identity map in 1 MiB sections. */
#include "arch.h"
#include <stdbool.h>

#define SECTION 0x2u
#define SECT_B (1u << 2)
#define SECT_C (1u << 3)
#define SECT_XN (1u << 4)
#define SECT_AP_KERNEL_RW (1u << 10) /* AP[2:0] = 001: kernel read/write, no user access */
#define SECT_TEX(n) ((uint32_t)(n) << 12)
#define SECT_S (1u << 16)
#define SECTION_SHIFT 20
#define SECTIONS 4096

/* TEX=001 C=1 B=1: write-back, write-allocate. Shareable so the attributes still hold once more CPUs join. */
#define SECT_NORMAL (SECTION | SECT_TEX(1) | SECT_C | SECT_B | SECT_S | SECT_AP_KERNEL_RW)
#define SECT_DEVICE (SECTION | SECT_B | SECT_XN | SECT_AP_KERNEL_RW)

/* Table walks through the same write-back, write-allocate, shareable cache policy as the RAM they read. */
#define TTBR_IRGN_WBWA (1u << 6)
#define TTBR_RGN_WBWA (1u << 3)
#define TTBR_S (1u << 1)
#define DACR_D0_CLIENT 1u
#define ACTLR_SMP (1u << 6)

static uint32_t kernel_l1[SECTIONS] __attribute__((aligned(16384)));

void mmu_init(const void *dtb)
{
  uint64_t ram_base = 0x40000000u, ram_size = 1ull << 30;
  fdt_memory(dtb, &ram_base, &ram_size);
  uint64_t ram_end = ram_base + ram_size > (1ull << 32) ? 1ull << 32 : ram_base + ram_size;

  /* A section that only partly overlaps RAM is mapped as RAM: RAM banks start and end on 1 MiB boundaries. */
  for (uint32_t i = 0; i < SECTIONS; i++) {
    uint64_t start = (uint64_t)i << SECTION_SHIFT, end = start + (1u << SECTION_SHIFT);
    bool ram = start < ram_end && end > ram_base;
    kernel_l1[i] = (uint32_t)start | (ram ? SECT_NORMAL : SECT_DEVICE);
  }

  /* Cortex-A15 and A7 keep the data cache out of coherency until ACTLR.SMP is set; elsewhere the write is ignored. */
  CP15_WRITE(ACTLR, CP15_READ(ACTLR) | ACTLR_SMP);
  __asm__ volatile("dsb" ::: "memory");
  CP15_WRITE(TTBCR, 0);
  CP15_WRITE(DACR, DACR_D0_CLIENT);
  CP15_WRITE(TTBR0, (uint32_t)(uintptr_t)kernel_l1 | TTBR_IRGN_WBWA | TTBR_RGN_WBWA | TTBR_S);
  CP15_WRITE(TLBIALL, 0);
  CP15_WRITE(ICIALLU, 0);
  CP15_WRITE(BPIALL, 0);
  __asm__ volatile("dsb\n\tisb" ::: "memory");

  uint32_t sctlr = CP15_READ(SCTLR);
  sctlr &= ~(SCTLR_A | SCTLR_TRE | SCTLR_AFE);
  sctlr |= SCTLR_M | SCTLR_C | SCTLR_Z | SCTLR_I;
  CP15_WRITE(SCTLR, sctlr);
  __asm__ volatile("isb" ::: "memory");
}
