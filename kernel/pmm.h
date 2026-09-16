/* pmm.h — physical memory manager: a bitmap allocator over 4 KiB frames. */
#ifndef OWOS_PMM_H
#define OWOS_PMM_H

#include <stdint.h>

struct multiboot_info;

#define PMM_FRAME_SIZE 4096

/* Parse the multiboot memory map, then reserve the low region the kernel and
 * the identity-mapped heap occupy. Frames above that become allocatable. */
void pmm_init(const struct multiboot_info *mbi);

uint32_t pmm_alloc_frame(void); /* physical addr of a free frame, or 0 */
void pmm_free_frame(uint32_t phys);
void pmm_retain_frame(uint32_t phys);
uint32_t pmm_frame_refs(uint32_t phys);

/* Reserve a physically contiguous run of `bytes` (rounded up to whole frames)
 * and return its base physical address, or 0 if no run that large is free. For
 * large kernel-side buffers (e.g. a framebuffer back buffer) that need to be
 * contiguous and don't fit in the small identity-mapped heap. Not freeable. */
uint32_t pmm_alloc_contig(uint32_t bytes);

/* Free a run returned by pmm_alloc_contig (frame by frame). */
void pmm_free_contig(uint32_t phys, uint32_t bytes);

/* The RAM-scaled low-memory layout (computed in pmm_init from detected RAM).
 * pmm_low_end()    = end of the identity-mapped/reserved low region (paging.c).
 * pmm_pool_start() = start of the palloc page-table pool == kernel heap end. */
uint32_t pmm_low_end(void);
uint32_t pmm_pool_start(void);

/* Reported in KiB rather than bytes so a ≥4 GiB machine doesn't overflow the
 * uint32 (4 GiB of bytes wraps to 0; 4 GiB is 4194304 KiB, which fits).
 * `installed` is the true firmware-reported RAM total (can exceed 4 GiB on a
 * 16 GiB machine); `total` is what the allocator actually manages, capped at
 * 4 GiB by the bitmap and 32-bit (non-PAE) paging. */
uint32_t pmm_installed_kib(void);
uint32_t pmm_total_kib(void);
uint32_t pmm_used_kib(void);
uint32_t pmm_free_kib(void);

/* RAM actually allocated by the OS, in KiB. Unlike pmm_used_kib() this excludes
 * the PCI/MMIO hole and reserved regions (which aren't RAM), so it's the honest
 * figure for a memory summary — ~16 MiB at boot, not ~1 GiB of hole. */
uint32_t pmm_ram_used_kib(void);

#endif /* OWOS_PMM_H */
