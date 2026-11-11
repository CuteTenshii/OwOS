/* mem.c: AArch64 memory bring-up on top of the generic pmm and heap. */
#include "arch.h"
#include "heap.h"
#include "memmap.h"

#define HEAP_MIN (16UL << 20)
#define HEAP_MAX (256UL << 20)
#define ALIGN_2M(n) (((n) + (2UL << 20) - 1) & ~((2UL << 20) - 1))
#define MAX_RESERVED 128
#define MAX_PIECES (MAX_RESERVED + 1)

extern char kernel_end[];

int arch_mem_init(const void *dtb)
{
  uint64_t base, size;
  if (fdt_memory(dtb, &base, &size))
    return -1;
  if (base + size > (1ULL << 32))
    size = (1ULL << 32) - base; /* the pmm tracks physical addresses below 4 GiB */

  struct fdt_range rsv[MAX_RESERVED], free_ram[MAX_PIECES];
  int nrsv = fdt_reserved(dtb, rsv, MAX_RESERVED - 1);
  if (nrsv < 0 || nrsv == MAX_RESERVED - 1)
    return -1;
  rsv[nrsv++] = (struct fdt_range){(uintptr_t)dtb, fdt_totalsize(dtb)};
  if (plat.fb.format != FB_NONE && nrsv < MAX_RESERVED)
    rsv[nrsv++] = (struct fdt_range){plat.fb.base, (uint64_t)plat.fb.stride * plat.fb.height};

  uintptr_t heap_bytes = size / 4 < HEAP_MIN ? HEAP_MIN : size / 4 > HEAP_MAX ? HEAP_MAX : size / 4;
  uintptr_t heap_end = ALIGN_2M((uintptr_t)kernel_end + heap_bytes);
  for (int i = 0; i < nrsv; i++) {
    uintptr_t start = rsv[i].base & ~0xfffUL;
    if (start < heap_end && rsv[i].base + rsv[i].size > (uintptr_t)kernel_end)
      heap_end = start;
  }
  if (heap_end < (uintptr_t)kernel_end + HEAP_MIN || heap_end >= base + size)
    return -1;

  int pieces = memmap_subtract(base, size, rsv, nrsv, free_ram, MAX_PIECES);
  if (pieces < 0)
    return -1;
  pmm_begin(base + size, size);
  for (int i = 0; i < pieces; i++)
    pmm_add_region(free_ram[i].base, free_ram[i].size);
  pmm_reserve_low((uint32_t)heap_end, (uint32_t)heap_end);
  heap_init();
  return 0;
}
