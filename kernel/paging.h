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
void      paging_unmap_user_page(uint32_t *dir, uint32_t vaddr);
void      paging_free_process_dir(uint32_t *dir);

/* Duplicate an address space for fork(): copies each mapped user page into a
 * fresh frame. Returns the child directory, or NULL. */
uint32_t *paging_fork_dir(uint32_t *parent);

/* Physical frame backing a mapped user virtual address (0 if not mapped). */
uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr);

/* 1 if vaddr is a present user page in the current address space (safe probe). */
int paging_user_mapped(uint32_t vaddr);

/* Identity-map [phys, phys+len) into the current address space so the kernel
 * can read firmware tables (e.g. ACPI) that sit above the 16 MiB identity map.
 * Page tables are taken from the pool and not reclaimed — intended for one-shot
 * use just before power-off. Returns 0 on success, -1 if the pool is exhausted. */
int paging_identity_map(uint32_t phys, uint32_t len);

/* Set the PWT bit on every PTE backing [virt, virt+len) so those pages use PAT
 * entry 1. Paired with a PAT that programs entry 1 as write-combining, this
 * turns a slow framebuffer mapping into a fast WC one. Operates on the current
 * address space; the framebuffer is mapped in the shared kernel directory, so
 * the change propagates to every process. */
void paging_mark_wc(uint32_t virt, uint32_t len);

/* Set the PCD (cache-disable) bit on every PTE backing [virt, virt+len), giving
 * a UC- mapping. Used for memory-mapped device register files (e.g. the e1000
 * NIC's BAR0) where caching/reordering register writes would corrupt the device.
 * Like paging_identity_map, operates on the current (kernel) address space; the
 * mapping lands in the shared kernel directory so it is visible under any CR3. */
void paging_mark_uc(uint32_t virt, uint32_t len);

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
