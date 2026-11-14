/* paging.h: AArch64 address spaces behind the shared paging API (see kernel/arch/x86/paging.h). The kernel runs on
 * its identity map of the low 4 GiB (TTBR0 L1 slots 0..3, shared by every space); each process adds its private
 * user window and shm slots above that (common/uabi.h), in 4 KiB pages. */
#ifndef OWOS_PAGING_H
#define OWOS_PAGING_H

#include "uaccess.h"
#include <stdbool.h>
#include <stdint.h>

/* A translation table entry; a process's address space is its root (L1) table. */
typedef uint64_t pgdir_t;

/* Root table of the kernel-only address space (kernel threads run on it). */
uintptr_t paging_kernel_phys(void);

/* A zeroed 4 KiB page for page tables, or NULL. */
void *palloc(void);
void pfree(void *page);

pgdir_t *paging_new_process_dir(void);
void paging_free_process_dir(pgdir_t *dir);
pgdir_t *paging_fork_dir(pgdir_t *parent);

/* Resolve a fault on a copy-on-write user page; `error` carries the x86 page-fault bits (1 present, 2 write).
 * true if handled and the access may retry. */
bool paging_handle_page_fault(uaddr_t vaddr, uint32_t error);

uint32_t paging_user_phys(pgdir_t *dir, uaddr_t vaddr);
bool paging_user_mapped(uaddr_t vaddr);
bool paging_user_range(uaddr_t vaddr, uint32_t len, bool write);

/* Physical memory below 4 GiB is identity-mapped, so these only check that the range lies there; 0/-1. */
int paging_identity_map(uint32_t phys, uint32_t len);
int paging_kmap(uint32_t phys, uint32_t len);
void paging_kunmap(uint32_t phys, uint32_t len);

int paging_map_user_at(pgdir_t *dir, uaddr_t vaddr, uint32_t phys, int npages);
void paging_unmap_user_at(pgdir_t *dir, uaddr_t vaddr, uint32_t bytes);
int paging_user_grow(pgdir_t *dir, uaddr_t old_top, uaddr_t new_top);
void paging_user_shrink(pgdir_t *dir, uaddr_t new_top, uaddr_t old_top);

/* A kernel pointer to physical frame `phys`: the identity map, so the slots never run out and need no unmap. */
#define PAGING_SCRATCH_SLOTS 4
void *paging_scratch_map(int slot, uint32_t phys);
void paging_scratch_unmap(int slot);

void paging_load(pgdir_t *dir);
pgdir_t *paging_current(void);

#endif /* OWOS_PAGING_H */
