/* pmm.c — track physical RAM one 4 KiB frame at a time with a bitmap. */

#include "pmm.h"
#include "spinlock.h"

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
static spinlock_t pmm_lock;

static uintptr_t pmm_lock_acquire(void) { return spin_lock_irqsave(&pmm_lock); }
static void pmm_lock_release(uintptr_t flags) { spin_unlock_irqrestore(&pmm_lock, flags); }

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

static uint32_t find_free_frame(uint32_t start, uint32_t end)
{
  if (start >= end)
    return end;
  uint32_t first = start / 32;
  uint32_t last = (end - 1) / 32;
  for (uint32_t word = first; word <= last; word++) {
    uint32_t mask = ~0u;
    if (word == first)
      mask &= ~0u << (start & 31);
    if (word == last && (end & 31))
      mask &= (1u << (end & 31)) - 1;
    uint32_t free_bits = ~bitmap[word] & mask;
    if (free_bits)
      return word * 32 + __builtin_ctz(free_bits);
  }
  return end;
}

void pmm_begin(uint64_t highest, uint64_t installed_bytes)
{
  installed_kib = (uint32_t)(installed_bytes / 1024);
  total_frames = highest / FRAME_SIZE;
  if (total_frames > MAX_FRAMES)
    total_frames = MAX_FRAMES;

  /* Start with everything marked used, then free the available regions. */
  for (uint32_t i = 0; i < BITMAP_DWORDS; i++)
    bitmap[i] = 0xFFFFFFFF;
  used_frames = total_frames;
}

void pmm_add_region(uint64_t addr, uint64_t len)
{
  uint64_t f0 = (addr + FRAME_SIZE - 1) / FRAME_SIZE; /* round up   */
  uint64_t f1 = (addr + len) / FRAME_SIZE;            /* round down */
  for (uint64_t f = f0; f < f1 && f < total_frames; f++) {
    if (frame_test((uint32_t)f)) {
      frame_clear((uint32_t)f);
      used_frames--;
      ram_frames++; /* a frame of real, usable RAM */
    }
  }
}

void pmm_reserve_low(uint32_t low_end, uint32_t pool_start)
{
  low_end_b = low_end;
  pool_start_b = pool_start;
  for (uint32_t f = 0; f < (low_end_b / FRAME_SIZE) && f < total_frames; f++) {
    if (!frame_test(f)) {
      frame_set(f);
      used_frames++;
    }
  }

  /* Everything below here is reserved, so that's where the search starts. */
  alloc_hint = (low_end_b / FRAME_SIZE);
}

phys_t pmm_alloc_frame(void)
{
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = find_free_frame(alloc_hint, total_frames);
  if (f == total_frames) {
    /* The cursor can outrun frames freed below it; sweep from the bottom once. */
    f = find_free_frame(low_end_b / FRAME_SIZE, alloc_hint);
    if (f == alloc_hint)
      f = total_frames;
  }
  if (f < total_frames) {
    frame_set(f);
    refs[f] = 1;
    used_frames++;
    alloc_hint = f + 1;
    pmm_lock_release(flags);
    return (phys_t)f * FRAME_SIZE;
  }
  pmm_lock_release(flags);
  return 0; /* out of physical memory */
}

phys_t pmm_alloc_contig(uint32_t bytes)
{
  /* Reject an absurd size before the round-up overflows uint32 and yields a
   * tiny `need` that would hand back a run far shorter than asked for. */
  if (bytes > 0xFFFFFFFFu - (FRAME_SIZE - 1))
    return 0;
  uint32_t need = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
  if (need == 0)
    return 0;
  uintptr_t flags = pmm_lock_acquire();

  /* Start at the allocation cursor and wrap once. The low region is permanently
   * reserved, so a wrapped search starts at low_end_b rather than frame zero. */
  uint32_t low = low_end_b / FRAME_SIZE;
  uint32_t start = alloc_hint < low || alloc_hint >= total_frames ? low : alloc_hint;
  for (int pass = 0; pass < 2; pass++) {
    uint32_t begin = pass == 0 ? start : low;
    uint32_t end = pass == 0 ? total_frames : start;
    for (uint32_t f = begin; f + need <= end;) {
      uint32_t run = 0;
      while (run < need) {
        uint32_t cur = f + run;
        if (!(cur & 31) && need - run >= 32 && bitmap[cur / 32] == ~0u) {
          run += 32;
          continue;
        }
        if (frame_test(cur))
          break;
        run++;
      }
      if (run == need) {
        for (uint32_t i = 0; i < need; i++) {
          frame_set(f + i);
          refs[f + i] = 1;
          used_frames++;
        }
        alloc_hint = f + need;
        pmm_lock_release(flags);
        return (phys_t)f * FRAME_SIZE;
      }
      f += run + 1;
    }
  }
  pmm_lock_release(flags);
  return 0;
}

void pmm_free_frame(phys_t phys)
{
  if (!phys || (phys & (FRAME_SIZE - 1)) != 0)
    return;
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = (uint32_t)(phys / FRAME_SIZE);
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

void pmm_retain_frame(phys_t phys)
{
  if (!phys || (phys & (FRAME_SIZE - 1)) != 0)
    return;
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = (uint32_t)(phys / FRAME_SIZE);
  if (f < total_frames && frame_test(f) && refs[f] != 0 && refs[f] != 0xFFFF)
    refs[f]++;
  pmm_lock_release(flags);
}

uint32_t pmm_frame_refs(phys_t phys)
{
  if (!phys || (phys & (FRAME_SIZE - 1)) != 0)
    return 0;
  uintptr_t flags = pmm_lock_acquire();
  uint32_t f = (uint32_t)(phys / FRAME_SIZE);
  uint32_t result = f < total_frames ? refs[f] : 0;
  pmm_lock_release(flags);
  return result;
}

/* Release a run previously handed out by pmm_alloc_contig. The bitmap allocator
 * tracks frames individually, so a contiguous run is just freed frame by frame.
 */
void pmm_free_contig(phys_t phys, uint32_t bytes)
{
  if (!phys || (phys & (FRAME_SIZE - 1)) != 0)
    return;
  if (bytes > 0xFFFFFFFFu - (FRAME_SIZE - 1)) /* never allocated (see alloc) */
    return;
  uint32_t need = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
  uintptr_t flags = pmm_lock_acquire();
  for (uint32_t i = 0; i < need; i++) {
    uint32_t f = (uint32_t)(phys / FRAME_SIZE) + i;
    if (f >= total_frames || !frame_test(f))
      continue;
    if (refs[f] > 1) {
      refs[f]--;
      continue;
    }
    refs[f] = 0;
    frame_clear(f);
    used_frames--;
    if (f < alloc_hint)
      alloc_hint = f;
  }
  pmm_lock_release(flags);
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
