/* mem.c: AArch64 memory bring-up on top of the generic pmm and heap. */
#include "arch.h"
#include "heap.h"

#define HEAP_BYTES (16UL << 20)
#define ALIGN_2M(n) (((n) + (2UL << 20) - 1) & ~((2UL << 20) - 1))

extern char kernel_end[];

int arch_mem_init(const void *dtb)
{
  uint64_t base, size;
  if (fdt_memory(dtb, &base, &size))
    return -1;
  if (base + size > (1ULL << 32))
    size = (1ULL << 32) - base; /* the pmm tracks physical addresses below 4 GiB */

  uintptr_t heap_end = ALIGN_2M((uintptr_t)kernel_end + HEAP_BYTES);
  if (heap_end >= base + size)
    return -1;

  pmm_begin(base + size, size);
  pmm_add_region(base, size);
  pmm_reserve_low((uint32_t)heap_end, (uint32_t)heap_end);
  heap_init();
  return 0;
}
