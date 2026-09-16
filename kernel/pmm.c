/* pmm.c — track physical RAM one 4 KiB frame at a time with a bitmap. */

#include "pmm.h"
#include "console.h"
#include "irqflags.h"
#include "multiboot.h"

#define FRAME_SIZE PMM_FRAME_SIZE
#define MAX_FRAMES (1u << 20) /* support up to 4 GiB                 */
#define BITMAP_DWORDS (MAX_FRAMES / 32)

/* The low 16 MiB is identity-mapped and owned by the kernel + heap; the PMM
 * hands out only frames above it. Must match the identity map (paging.c) and
 * the heap window (heap.c). */
#define RESERVED_BYTES (16u * 1024 * 1024)
#define RESERVED_FRAMES (RESERVED_BYTES / FRAME_SIZE)

static uint32_t bitmap[BITMAP_DWORDS];
static uint16_t refs[MAX_FRAMES];
static uint32_t total_frames;
static uint32_t used_frames;

/* RAM-scaled low region, computed in pmm_init (see there). low_end_b is the end
 * of the identity-mapped, permanently-reserved low memory (kernel image + heap
 * + page-table pool); pool_start_b splits it into heap [.., pool_start_b) and
 * pool [pool_start_b, low_end_b). Default to the legacy fixed layout so any use
 * before pmm_init still works. */
static uint32_t low_end_b = RESERVED_BYTES;                  /* 16 MiB */
static uint32_t pool_start_b = RESERVED_BYTES - 0x00400000u; /* 12 MiB */

uint32_t pmm_low_end(void) { return low_end_b; }
uint32_t pmm_pool_start(void) { return pool_start_b; } /* = kernel heap end */

/* Search cursor: the lowest frame that might be free. pmm_alloc_frame scans up
 * from here instead of from 0, so allocating N frames in a row is O(N) total
 * rather than O(N^2) — without it, loading a program with a multi-MiB .bss (or
 * forking) re-scanned every already-taken low frame on each allocation. A free
 * below the cursor pulls it back so the freed frame is reused. */
static uint32_t alloc_hint;
static volatile uint32_t pmm_lock;

static uintptr_t pmm_lock_acquire(void)
{
  uintptr_t flags = irq_save();
  while (__sync_lock_test_and_set(&pmm_lock, 1))
    __asm__ volatile("pause");
  return flags;
}

static void pmm_lock_release(uintptr_t flags)
{
  __sync_lock_release(&pmm_lock);
  irq_restore(flags);
}

/* Frames backed by *real, allocatable RAM* (the available regions below 4 GiB).
 * Distinct from total_frames, which also spans the PCI/MMIO hole and reserved
 * regions just under 4 GiB — those are marked "used" in the bitmap but were
 * never RAM. On real hardware that hole is ~1 GiB, so reporting used_frames as
 * "memory in use" wildly overstates it; pmm_ram_used_kib() measures against
 * this instead, to show what the OS actually allocated. */
static uint32_t ram_frames;

/* Total available RAM the firmware reported, in KiB — independent of the
 * allocator, which the bitmap and 32-bit paging cap at MAX_FRAMES (4 GiB). On a
 * machine with more than 4 GiB this is larger than total_frames*4: we *detect*
 * all of it but can only *manage* the low 4 GiB without PAE. KiB (not bytes) so
 * 16 GiB fits in a uint32 (16 GiB = 16777216 KiB; bytes would overflow). */
static uint32_t installed_kib;

static int frame_test(uint32_t f) { return (bitmap[f / 32] >> (f % 32)) & 1u; }
static void frame_set(uint32_t f) { bitmap[f / 32] |= (1u << (f % 32)); }
static void frame_clear(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }

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
    const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)addr;
    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint64_t top = e->addr + e->len;
      if (top > highest)
        highest = top;
      avail += e->len;
    }
    addr += e->size + 4;
  }
  installed_kib = (uint32_t)(avail / 1024);

  /* Size the identity-mapped low region (kernel heap + page-table pool) from
   * detected RAM instead of a fixed 16 MiB, so the kmalloc heap — and thus the
   * number of tasks/windows whose 32 KiB kernel stacks come from it, plus the
   * palloc pool that backs per-process page dirs — scales with the machine.
   * ~1/16 of RAM, clamped to [16 MiB, 192 MiB] (must stay under SCRATCH_VADDR
   * at 256 MiB) and 4 MiB-granular (one identity page table each). A 256 MiB
   * box keeps the original 16 MiB layout; 4 GiB gets 192 MiB. The pool takes
   * the top ~1/8; the heap gets the rest. */
  {
    uint32_t mib = installed_kib / 1024;
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
    low_end_b = le << 20;
    pool_start_b = (uint32_t)(le - pl) << 20;
  }

  total_frames = highest / FRAME_SIZE;
  if (total_frames > MAX_FRAMES)
    total_frames = MAX_FRAMES;

  /* Start with everything marked used, then free the available regions. */
  for (uint32_t i = 0; i < BITMAP_DWORDS; i++)
    bitmap[i] = 0xFFFFFFFF;
  used_frames = total_frames;

  addr = mbi->mmap_addr;
  while (addr < end) {
    const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)addr;
    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
      uint32_t f0 = (e->addr + FRAME_SIZE - 1) / FRAME_SIZE; /* round up   */
      uint32_t f1 = (e->addr + e->len) / FRAME_SIZE;         /* round down */
      for (uint32_t f = f0; f < f1 && f < total_frames; f++) {
        if (frame_test(f)) {
          frame_clear(f);
          used_frames--;
          ram_frames++; /* a frame of real, usable RAM */
        }
      }
    }
    addr += e->size + 4;
  }

  /* Reserve the low 16 MiB (kernel image + identity-mapped heap). */
  for (uint32_t f = 0; f < (low_end_b / FRAME_SIZE) && f < total_frames; f++) {
    if (!frame_test(f)) {
      frame_set(f);
      used_frames++;
    }
  }

  /* Everything below here is reserved, so that's where the search starts. */
  alloc_hint = (low_end_b / FRAME_SIZE);
}

uint32_t pmm_alloc_frame(void)
{
  uintptr_t flags = pmm_lock_acquire();
  for (uint32_t f = alloc_hint; f < total_frames; f++) {
    if (!frame_test(f)) {
      frame_set(f);
      refs[f] = 1;
      used_frames++;
      alloc_hint = f + 1;
      pmm_lock_release(flags);
      return f * FRAME_SIZE;
    }
  }
  /* The cursor can outrun frames freed below it; sweep from the bottom once. */
  for (uint32_t f = (low_end_b / FRAME_SIZE); f < alloc_hint; f++) {
    if (!frame_test(f)) {
      frame_set(f);
      refs[f] = 1;
      used_frames++;
      alloc_hint = f + 1;
      pmm_lock_release(flags);
      return f * FRAME_SIZE;
    }
  }
  pmm_lock_release(flags);
  return 0; /* out of physical memory */
}

uint32_t pmm_alloc_contig(uint32_t bytes)
{
  /* Reject an absurd size before the round-up overflows uint32 and yields a
   * tiny `need` that would hand back a run far shorter than asked for. */
  if (bytes > 0xFFFFFFFFu - (FRAME_SIZE - 1))
    return 0;
  uint32_t need = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
  if (need == 0)
    return 0;
  uintptr_t flags = pmm_lock_acquire();

  /* Start past the always-reserved low 16 MiB: those frames are permanently
   * marked used, so no contiguous run can begin there. Scanning from 0 stepped
   * through all ~4096 reserved frames one at a time (run=0 -> f+=1) on every
   * call before reaching allocatable RAM. */
  for (uint32_t f = (low_end_b / FRAME_SIZE); f + need <= total_frames;) {
    uint32_t run = 0;
    while (run < need && !frame_test(f + run))
      run++;
    if (run == need) { /* found a long-enough free run     */
      for (uint32_t i = 0; i < need; i++) {
        frame_set(f + i);
        refs[f + i] = 1;
        used_frames++;
      }
      pmm_lock_release(flags);
      return f * FRAME_SIZE;
    }
    f += run + 1; /* skip past the blocking used frame */
  }
  pmm_lock_release(flags);
  return 0;
}

void pmm_free_frame(uint32_t phys)
{
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = phys / FRAME_SIZE;
  if (f < total_frames && frame_test(f)) {
    if (refs[f] > 1) {
      refs[f]--;
      pmm_lock_release(flags);
      return;
    }
    refs[f] = 0;
    frame_clear(f);
    used_frames--;
    if (f < alloc_hint) /* let the next alloc reuse this frame */
      alloc_hint = f;
  }
  pmm_lock_release(flags);
}

void pmm_retain_frame(uint32_t phys)
{
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = phys / FRAME_SIZE;
  if (f < total_frames && frame_test(f) && refs[f] != 0 && refs[f] != 0xFFFF)
    refs[f]++;
  pmm_lock_release(flags);
}

uint32_t pmm_frame_refs(uint32_t phys)
{
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = phys / FRAME_SIZE;
  uint32_t result = f < total_frames ? refs[f] : 0;
  pmm_lock_release(flags);
  return result;
}

/* Release a run previously handed out by pmm_alloc_contig. The bitmap allocator
 * tracks frames individually, so a contiguous run is just freed frame by frame.
 */
void pmm_free_contig(uint32_t phys, uint32_t bytes)
{
  if (bytes > 0xFFFFFFFFu - (FRAME_SIZE - 1)) /* never allocated (see alloc) */
    return;
  uint32_t need = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
  for (uint32_t i = 0; i < need; i++)
    pmm_free_frame(phys + i * FRAME_SIZE);
}

/* Reported in KiB, not bytes: a fully-populated 4 GiB machine is 4194304 KiB,
 * which fits in a uint32, whereas the byte count (4 GiB = 2^32) would wrap to
 * 0 — which is exactly why a real ≥4 GiB laptop printed "0 MiB RAM detected"
 * while QEMU's 128 MiB looked fine. (total_frames is clamped to MAX_FRAMES, so
 * total_frames * 4 never overflows.) */
uint32_t pmm_installed_kib(void) { return installed_kib; }
uint32_t pmm_total_kib(void) { return total_frames * (FRAME_SIZE / 1024); }
uint32_t pmm_used_kib(void) { return used_frames * (FRAME_SIZE / 1024); }
uint32_t pmm_free_kib(void) { return (total_frames - used_frames) * (FRAME_SIZE / 1024); }

/* RAM the OS has actually allocated, in KiB: manageable RAM minus what's still
 * free. Every currently-free frame is RAM (only RAM frames are ever cleared),
 * so free RAM is total_frames-used_frames and used RAM is the rest of
 * ram_frames. Unlike pmm_used_kib() this excludes the PCI/MMIO hole and
 * reserved regions, so it reflects real usage (~16 MiB at boot) instead of ~1
 * GiB of unusable hole. */
uint32_t pmm_ram_used_kib(void)
{
  uint32_t free_ram = total_frames - used_frames;
  uint32_t used_ram = ram_frames > free_ram ? ram_frames - free_ram : 0;
  return used_ram * (FRAME_SIZE / 1024);
}
