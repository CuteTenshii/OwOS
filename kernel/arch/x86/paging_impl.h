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

#endif /* OWOS_PAGING_IMPL_H */
