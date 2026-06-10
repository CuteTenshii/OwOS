/* paging.h — enable 32-bit paging with the low memory identity-mapped. */
#ifndef OWOS_PAGING_H
#define OWOS_PAGING_H

#include <stdint.h>

void paging_init(void);

/* Physical address of the kernel's page directory (kernel threads' CR3). */
uint32_t paging_kernel_phys(void);

/* Page-aligned frame pool, carved from identity-mapped low memory so every
 * frame is directly addressable by the kernel (physical == virtual). */
void *palloc(void);                /* a zeroed 4 KiB frame, or NULL          */
void  pfree(void *page);

/* Per-process address spaces. A new directory shares the kernel/heap
 * mappings but owns a private user window [2 MiB, 4 MiB). */
uint32_t *paging_new_process_dir(void);
void      paging_map_user_page(uint32_t *dir, uint32_t vaddr, uint32_t frame);
void      paging_free_process_dir(uint32_t *dir);

/* Duplicate an address space for fork(): copies each mapped user page into a
 * fresh frame. Returns the child directory, or NULL. */
uint32_t *paging_fork_dir(uint32_t *parent);

/* Physical frame backing a mapped user virtual address (0 if not mapped). */
uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr);

/* Identity-map [phys, phys+len) into the current address space so the kernel
 * can read firmware tables (e.g. ACPI) that sit above the 16 MiB identity map.
 * Page tables are taken from the pool and not reclaimed — intended for one-shot
 * use just before power-off. Returns 0 on success, -1 if the pool is exhausted. */
int paging_identity_map(uint32_t phys, uint32_t len);

static inline void paging_load(uint32_t *dir)
{
	__asm__ volatile ("mov %0, %%cr3" : : "r"(dir) : "memory");
}
static inline uint32_t *paging_current(void)
{
	uint32_t *dir;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(dir));
	return dir;
}

#endif /* OWOS_PAGING_H */
