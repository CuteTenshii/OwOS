/* pmm.h — physical memory manager: a bitmap allocator over 4 KiB frames. */
#ifndef OWOS_PMM_H
#define OWOS_PMM_H

#include <stdint.h>

struct multiboot_info;

#define PMM_FRAME_SIZE 4096

/* Parse the multiboot memory map, then reserve the low region the kernel and
 * the identity-mapped heap occupy. Frames above that become allocatable. */
void pmm_init(const struct multiboot_info *mbi);

uint32_t pmm_alloc_frame(void);        /* physical addr of a free frame, or 0 */
void     pmm_free_frame(uint32_t phys);

/* Reserve a physically contiguous run of `bytes` (rounded up to whole frames)
 * and return its base physical address, or 0 if no run that large is free. For
 * large kernel-side buffers (e.g. a framebuffer back buffer) that need to be
 * contiguous and don't fit in the small identity-mapped heap. Not freeable. */
uint32_t pmm_alloc_contig(uint32_t bytes);

uint32_t pmm_total_bytes(void);
uint32_t pmm_used_bytes(void);
uint32_t pmm_free_bytes(void);

#endif /* OWOS_PMM_H */
