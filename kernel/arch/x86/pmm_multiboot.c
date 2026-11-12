/* pmm_multiboot.c: feed the physical allocator from the multiboot memory map and size the low region. */

#include "console.h"
#include "multiboot.h"
#include "pmm.h"

void pmm_init(const struct multiboot_info *mbi)
{
  /* Without a memory map we can't know what's safe; refuse to guess. */
  if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {
    con_write("pmm: no multiboot memory map -- cannot init.\n");
    return;
  }

  /* First pass: find the highest usable address to size the bitmap, and sum
   * every available region (in 64-bit, so RAM above 4 GiB still counts) to
   * report the true installed total even though we can only manage 4 GiB. */
  uint64_t highest = 0;
  uint64_t avail = 0;
  uint32_t addr = mbi->mmap_addr;
  uint32_t end = mbi->mmap_addr + mbi->mmap_length;
  while (addr < end) {
    const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)(uintptr_t)addr;
    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint64_t top = e->addr + e->len;
      if (top > highest)
        highest = top;
      avail += e->len;
    }
    addr += e->size + 4;
  }

  /* Size the identity-mapped low region (kernel heap + page-table pool) from
   * detected RAM instead of a fixed 16 MiB, so the kmalloc heap, and thus the
   * number of tasks/windows whose 32 KiB kernel stacks come from it, plus the
   * palloc pool that backs per-process page dirs, scales with the machine.
   * ~1/16 of RAM, clamped to [16 MiB, 192 MiB] (must stay under SCRATCH_VADDR
   * at 256 MiB) and 4 MiB-granular (one identity page table each). A 256 MiB
   * box keeps the original 16 MiB layout; 4 GiB gets 192 MiB. The pool takes
   * the top ~1/8; the heap gets the rest. */
  uint32_t mib = (uint32_t)(avail / 1024 / 1024);
  uint32_t le = mib / 16;
  if (le < 16)
    le = 16;
  if (le > 192)
    le = 192;
  le &= ~3u;
  uint32_t pl = le / 8;
  if (pl < 4)
    pl = 4;
  if (pl > 24)
    pl = 24;
  pl &= ~3u;

  pmm_begin(highest, avail);
  addr = mbi->mmap_addr;
  while (addr < end) {
    const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)(uintptr_t)addr;
    if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
      pmm_add_region(e->addr, e->len);
    addr += e->size + 4;
  }
  pmm_reserve_low(le << 20, (le - pl) << 20);
}
