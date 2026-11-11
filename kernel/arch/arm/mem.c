/* mem.c: ARMv7-A memory bring-up on top of the generic pmm and heap. */
#include "arch.h"
#include "heap.h"
#include "memmap.h"

#define HEAP_MIN (16u << 20)
#define HEAP_MAX (256u << 20)
#define ALIGN_1M(n) (((n) + (1u << 20) - 1) & ~((1u << 20) - 1))
#define MAX_RESERVED 128
#define MAX_PIECES (MAX_RESERVED + 1)

extern char kernel_end[];

int arch_mem_init(const void *dtb)
{
  uint64_t base, size;
  if (fdt_memory(dtb, &base, &size) || base >= (1ull << 32))
    return -1;
  if (base + size > (1ull << 32))
    size = (1ull << 32) - base; /* without LPAE nothing above 4 GiB is reachable */

  struct fdt_range rsv[MAX_RESERVED], free_ram[MAX_PIECES];
  int nrsv = fdt_reserved(dtb, rsv, MAX_RESERVED - 1);
  if (nrsv < 0 || nrsv == MAX_RESERVED - 1)
    return -1;
  rsv[nrsv++] = (struct fdt_range){(uintptr_t)dtb, fdt_totalsize(dtb)};

  /* The heap runs from the end of the image to heap_end; everything below heap_end stays out of the pmm. */
  uint64_t quarter = size / 4;
  uint64_t heap_bytes = quarter < HEAP_MIN ? HEAP_MIN : quarter > HEAP_MAX ? HEAP_MAX : quarter;
  uint64_t heap_end = ALIGN_1M((uint64_t)(uintptr_t)kernel_end + heap_bytes);
  for (int i = 0; i < nrsv; i++) {
    uint64_t start = rsv[i].base & ~0xfffull;
    if (start < heap_end && rsv[i].base + rsv[i].size > (uintptr_t)kernel_end)
      heap_end = start;
  }
  if (heap_end < (uint64_t)(uintptr_t)kernel_end + HEAP_MIN || heap_end >= base + size)
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
