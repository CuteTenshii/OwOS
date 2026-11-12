/* pmm_test.c: the physical frame allocator (kernel/pmm.c): region setup and rounding, reservation, exhaustion,
 * reference counts, contiguous runs, the 4 GiB cap, and concurrent use from several threads. */
#define OWOS_SUITE_TEST
#include "../kernel/pmm.h"
#include "test.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024ULL * 1024)
#define FRAME 4096ULL

/* One 8 MiB region at 1 MiB, the first 2 MiB reserved: 1792 allocatable frames. */
static void standard(void)
{
  pmm_begin(9 * MIB, 8 * MIB);
  pmm_add_region(1 * MIB, 8 * MIB);
  pmm_reserve_low(2 * MIB, 2 * MIB);
}

static void check_setup(void)
{
  standard();
  expect_eq(pmm_total_kib(), 9 * 1024);
  expect_eq(pmm_installed_kib(), 8 * 1024);
  expect_eq(pmm_free_kib(), 7 * 1024);
  expect_eq(pmm_used_kib(), 2 * 1024);
  expect_eq(pmm_low_end(), 2 * MIB);
  expect_eq(pmm_pool_start(), 2 * MIB);

  phys_t first = pmm_alloc_frame();
  expect_eq(first, 2 * MIB);
  expect_eq(pmm_alloc_frame(), 2 * MIB + FRAME);
  expect_eq(pmm_free_kib(), 7 * 1024 - 8);
  expect_eq(pmm_ram_used_kib(), 1 * 1024 + 8); /* 1 MiB of region below the reserve, plus two frames */
}

static void check_exhaustion(void)
{
  standard();
  phys_t first = pmm_alloc_frame();
  uint8_t *seen = calloc(9 * MIB / FRAME, 1);
  seen[first / FRAME] = 1;
  int n = 1;
  for (phys_t f; (f = pmm_alloc_frame()); n++) {
    expect(f % FRAME == 0 && f >= 2 * MIB && f < 9 * MIB);
    expect(!seen[f / FRAME]);
    seen[f / FRAME] = 1;
  }
  expect_eq(n, 1792);
  expect_eq(pmm_free_kib(), 0);
  expect_eq(pmm_alloc_frame(), 0);

  pmm_free_frame(2 * MIB + 100 * FRAME);
  expect_eq(pmm_alloc_frame(), 2 * MIB + 100 * FRAME);
  expect_eq(pmm_alloc_frame(), 0);
  free(seen);
}

static void check_rounding_and_holes(void)
{
  pmm_begin(8 * MIB, 3 * MIB);
  pmm_add_region(0x1800, 0x3000); /* frames 2 and 3 only: start rounds up, end rounds down */
  pmm_add_region(3 * MIB, 1 * MIB);
  pmm_reserve_low(0, 0);
  expect_eq(pmm_alloc_frame(), 0x2000);
  expect_eq(pmm_alloc_frame(), 0x3000);
  for (int i = 0; i < 256; i++) {
    phys_t f = pmm_alloc_frame();
    expect(f >= 3 * MIB && f < 4 * MIB); /* nothing from the unmapped gap */
  }
  expect_eq(pmm_alloc_frame(), 0);
}

static void check_refcounts(void)
{
  standard();
  phys_t f = pmm_alloc_frame();
  expect_eq(pmm_frame_refs(f), 1);
  pmm_retain_frame(f);
  pmm_retain_frame(f);
  expect_eq(pmm_frame_refs(f), 3);
  uint32_t free_before = pmm_free_kib();
  pmm_free_frame(f);
  pmm_free_frame(f);
  expect_eq(pmm_frame_refs(f), 1);
  expect_eq(pmm_free_kib(), free_before);
  pmm_free_frame(f);
  expect_eq(pmm_frame_refs(f), 0);
  expect_eq(pmm_free_kib(), free_before + 4);

  pmm_free_frame(f); /* double free, unaligned, null and out-of-range are all ignored */
  pmm_free_frame(f + 1);
  pmm_free_frame(0);
  pmm_free_frame(64 * MIB);
  pmm_retain_frame(f);
  expect_eq(pmm_free_kib(), free_before + 4);
  expect_eq(pmm_frame_refs(f), 0);
  expect_eq(pmm_frame_refs(64 * MIB), 0);
}

static void check_contiguous(void)
{
  standard();
  phys_t run = pmm_alloc_contig(5 * FRAME + 1); /* rounds up to six frames */
  expect(run != 0 && run % FRAME == 0 && run >= 2 * MIB);
  expect_eq(pmm_free_kib(), 7 * 1024 - 24);
  for (int i = 0; i < 6; i++)
    expect_eq(pmm_frame_refs(run + i * FRAME), 1);
  phys_t next = pmm_alloc_frame();
  expect(next < run || next >= run + 6 * FRAME);
  pmm_free_contig(run, 5 * FRAME + 1);
  expect_eq(pmm_free_kib(), 7 * 1024 - 4);

  expect_eq(pmm_alloc_contig(0), 0);
  expect_eq(pmm_alloc_contig(0xFFFFFFFFu), 0);
  expect_eq(pmm_alloc_contig(64 * MIB), 0);

  /* Fragment everything: no run of two exists, but singles do. */
  standard();
  phys_t f[1792];
  for (int i = 0; i < 1792; i++)
    f[i] = pmm_alloc_frame();
  for (int i = 0; i < 1792; i += 2)
    pmm_free_frame(f[i]);
  expect_eq(pmm_alloc_contig(2 * FRAME), 0);
  expect(pmm_alloc_contig(FRAME) != 0);
  pmm_free_frame(f[1]);
  expect(pmm_alloc_contig(2 * FRAME) != 0); /* frames 0,1,2 are now free */

  /* A 32-frame-aligned word that is fully used must not count as 32 free frames. */
  standard();
  for (int i = 0; i < 1792; i++)
    f[i] = pmm_alloc_frame();
  for (int i = 88; i < 96; i++) /* 8 free frames ending at an aligned, fully used word */
    pmm_free_frame(f[i]);
  for (int i = 128; i < 1792; i++)
    pmm_free_frame(f[i]);
  phys_t big = pmm_alloc_contig(40 * FRAME);
  expect_eq(big, 2 * MIB + 128 * FRAME);
}

static void check_cap_and_reinit(void)
{
  pmm_begin(8ULL << 30, 8ULL << 30);
  expect_eq(pmm_installed_kib(), 8U * 1024 * 1024);
  expect_eq(pmm_total_kib(), 4U * 1024 * 1024); /* tracked range stops at 4 GiB */

  standard();
  pmm_alloc_frame();
  pmm_begin(9 * MIB, 8 * MIB);
  expect_eq(pmm_used_kib(), 9 * 1024); /* nothing free until regions are added again */
  expect_eq(pmm_ram_used_kib(), 0);
  pmm_add_region(1 * MIB, 8 * MIB);
  pmm_reserve_low(2 * MIB, 2 * MIB);
  expect_eq(pmm_alloc_frame(), 2 * MIB);
  expect_eq(pmm_frame_refs(2 * MIB), 1);
  expect_eq(pmm_frame_refs(2 * MIB + FRAME), 0);
}

#define THREADS 4
#define ROUNDS 4000
#define HELD 8

static uint8_t *owner; /* frame -> 1 while some thread holds it */
static int dup_handouts;

static void *hammer(void *arg)
{
  unsigned seed = (unsigned)(uintptr_t)arg;
  phys_t held[HELD] = {0};
  for (int i = 0; i < ROUNDS; i++) {
    int slot = rand_r(&seed) % HELD;
    if (held[slot]) {
      __sync_lock_release(&owner[held[slot] / FRAME]);
      pmm_free_frame(held[slot]);
      held[slot] = 0;
    } else {
      phys_t f = pmm_alloc_frame();
      if (f && __sync_lock_test_and_set(&owner[f / FRAME], 1))
        __sync_fetch_and_add(&dup_handouts, 1);
      held[slot] = f;
    }
  }
  for (int i = 0; i < HELD; i++)
    if (held[i]) {
      __sync_lock_release(&owner[held[i] / FRAME]);
      pmm_free_frame(held[i]);
    }
  return 0;
}

static void check_threads(void)
{
  standard();
  owner = calloc(9 * MIB / FRAME, 1);
  pthread_t t[THREADS];
  for (long i = 0; i < THREADS; i++)
    pthread_create(&t[i], 0, hammer, (void *)(i + 1));
  for (int i = 0; i < THREADS; i++)
    pthread_join(t[i], 0);
  expect_eq(dup_handouts, 0);
  expect_eq(pmm_free_kib(), 7 * 1024);
  free(owner);
}

int main(void)
{
  test("setup and accounting", check_setup);
  test("exhaustion hands out each frame once", check_exhaustion);
  test("region rounding and holes", check_rounding_and_holes);
  test("reference counts and bad frees", check_refcounts);
  test("contiguous runs", check_contiguous);
  test("4 GiB cap and re-initialisation", check_cap_and_reinit);
  test("concurrent alloc and free", check_threads);
  return 0;
}
