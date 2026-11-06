/* paging.h, enable 32-bit paging with the low memory identity-mapped. */
#ifndef OWOS_PAGING_H
#define OWOS_PAGING_H

#include <stdbool.h>
#include <stdint.h>

void paging_init(void);

/* Physical address of the kernel's page directory (kernel threads' CR3). */
uintptr_t paging_kernel_phys(void);

/* Page-aligned frame pool in identity-mapped low memory (physical == virtual). */
void *palloc(void); /* a zeroed 4 KiB frame, or NULL */
void pfree(void *page);

/* Per-process address spaces: shares kernel mappings, owns a private user window. */
uint32_t *paging_new_process_dir(void);
void paging_free_process_dir(uint32_t *dir);

/* Duplicate an address space for fork() with copy-on-write user pages; NULL on failure. */
uint32_t *paging_fork_dir(uint32_t *parent);

/* Resolve a write fault on a COW user page; 1 if handled and the instruction may retry. */
bool paging_handle_page_fault(uint32_t vaddr, uint32_t error);

/* Physical frame backing a mapped user virtual address (0 if not mapped). */
uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr);

/* 1 if vaddr is a present user page in the current address space (safe probe). */
bool paging_user_mapped(uint32_t vaddr);

/* Validate every page of a current-process user range; `write` also requires writable. */
bool paging_user_range(uint32_t vaddr, uint32_t len, bool write);

/* Identity-map [phys, phys+len) into the current space to read firmware tables; not reclaimed; 0/-1. */
int paging_identity_map(uint32_t phys, uint32_t len);

/* Map/unmap a physical range into the kernel master directory (supervisor RW). */
int paging_kmap(uint32_t phys, uint32_t len);
void paging_kunmap(uint32_t phys, uint32_t len);

/* Map/unmap user pages at an arbitrary vaddr in `dir` with private USER page tables. */
int paging_map_user_at(uint32_t *dir, uint32_t vaddr, uint32_t phys, int npages);
void paging_unmap_user_at(uint32_t *dir, uint32_t vaddr, uint32_t bytes);

/* Grow/shrink the user heap (SYS_SBRK) over a page-aligned range; grow rolls back and returns -1. */
int paging_user_grow(uint32_t *dir, uint32_t old_top, uint32_t new_top);
void paging_user_shrink(uint32_t *dir, uint32_t new_top, uint32_t old_top);

/* Transient supervisor-only mapping of a physical frame in scratch `slot`; always unmap after use. */
#define PAGING_SCRATCH_SLOTS 4
void *paging_scratch_map(int slot, uint32_t phys);
void paging_scratch_unmap(int slot);

/* Set PWT on every PTE of [virt, virt+len) so pages use PAT entry 1 (write-combining). */
void paging_mark_wc(uint32_t virt, uint32_t len);

/* Set PCD on every PTE of [virt, virt+len) for UC- device register mappings (e.g. e1000 BAR0). */
void paging_mark_uc(uint32_t virt, uint32_t len);

static inline void paging_load(uint32_t *dir) { __asm__ volatile("mov %0, %%cr3" : : "r"(dir) : "memory"); }
static inline uint32_t *paging_current(void)
{
  uint32_t *dir;
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir));
  return dir;
}

#endif /* OWOS_PAGING_H */
