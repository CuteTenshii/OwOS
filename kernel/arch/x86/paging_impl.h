/* paging_impl.h: definitions shared by paging_32.c and paging_64.c. Private to them; a build compiles exactly one, so
 * the static state below exists once. */
#ifndef OWOS_PAGING_IMPL_H
#define OWOS_PAGING_IMPL_H

#include "creg.h"
#include "irqflags.h"
#include "kstring.h"
#include "loader.h" /* USER_BASE / USER_REGION_END: the per-process window */
#include "paging.h"
#include "pmm.h"
#include "smp.h"
#include <stdint.h>

/* IF-off bracket around scratch-slot use so a fork copy can't be preempted by another scratch user. */

#define PAGE_PRESENT 0x1
#define PAGE_RW 0x2
#define PAGE_USER 0x4
#define PAGE_PWT 0x8   /* write-through bit; selects PAT entry 1 here */
#define PAGE_PCD 0x10  /* cache-disable bit; UC- mapping for MMIO regs */
#define PAGE_COW 0x200 /* software bit: user page is shared read-only */

/* Scratch window: one PT in the shared kernel mapping to transiently map any pmm frame. */
#define SCRATCH_VADDR 0x10000000u

static volatile uint32_t page_pool_lock;
static volatile uint32_t scratch_lock;

static void page_pool_acquire(void)
{
  while (__sync_lock_test_and_set(&page_pool_lock, 1))
    __asm__ volatile("pause");
}

static void page_pool_release(void) { __sync_lock_release(&page_pool_lock); }

static void scratch_acquire(void)
{
  while (__sync_lock_test_and_set(&scratch_lock, 1))
    __asm__ volatile("pause");
}

static void scratch_release(void) { __sync_lock_release(&scratch_lock); }

/* Frames unmapped in a batch are freed only after every CPU has dropped its cached translations of them. */
#define UNMAP_BATCH 64

static void free_frames_flushed(const uint32_t *frames, int n)
{
  if (n == 0)
    return;
  smp_tlb_shootdown();
  for (int i = 0; i < n; i++)
    pmm_free_frame(frames[i]);
}

/* Page tables, likewise, go back to the pool only once no CPU can still walk them. */
static void free_tables_flushed(uint32_t *dir, void *const *tables, int n)
{
  if (n == 0)
    return;
  if ((read_cr3() & ~(uintptr_t)0xFFF) == (uintptr_t)dir)
    write_cr3(read_cr3());
  smp_tlb_shootdown();
  for (int i = 0; i < n; i++)
    pfree(tables[i]);
}

#endif /* OWOS_PAGING_IMPL_H */
