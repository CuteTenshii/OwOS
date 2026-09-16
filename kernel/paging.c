/* paging.c — bring up the kernel address space and turn paging on.
 *
 * Identity mapping (virtual == physical) means the kernel, stack, VGA buffer,
 * and these page tables all keep working the instant CR0.PG flips. We map the
 * RAM-scaled low region so the kernel heap and the palloc pool are addressable.
 *
 * Two builds share one file. The 32-bit (i386) path uses classic two-level
 * paging (a 1024-entry page directory of 4-byte PDEs). The 64-bit (x86_64) path
 * uses four-level long-mode paging (PML4 -> PDPT -> PD -> PT, 8-byte entries);
 * boot.S has already enabled long mode on a temporary identity map, so here we
 * build the real tables and just reload CR3. Every public function keeps the
 * same signature on both arches — all of our physical/virtual addresses live
 * below 4 GiB, so a `uint32_t` still names any address the kernel touches.
 */

#include "paging.h"
#include "creg.h"
#include "irqflags.h"
#include "loader.h" /* USER_BASE / USER_REGION_END: the per-process window  */
#include "pmm.h"
#include <stdint.h>

/* Bracket a scratch-mapping interval with IF off (restoring the prior flag), so
 * a fork copying through the shared scratch slots can't be preempted by another
 * scratch user. (irq_save/irq_restore live in irqflags.h.) */

#define PAGE_PRESENT 0x1
#define PAGE_RW 0x2
#define PAGE_USER 0x4
#define PAGE_PWT 0x8   /* write-through bit; selects PAT entry 1 here      */
#define PAGE_PCD 0x10  /* cache-disable bit; UC- mapping for MMIO regs     */
#define PAGE_COW 0x200 /* software bit: user page is shared read-only     */

/* Scratch mapping window: a single 4 KiB-granular page table installed into the
 * shared kernel mapping at boot, covering SCRATCH_VADDR.. so the kernel can
 * transiently map any pmm frame to write it. SCRATCH_VADDR (256 MiB) sits above
 * the identity map and below WIN_FB_VADDR/MMIO, so it collides with nothing.
 * The page table itself lives in low .bss (identity-mapped, so its
 * physaddr==addr). */
#define SCRATCH_VADDR 0x10000000u

#ifdef __x86_64__
/* ===========================================================================
 * 64-bit (long mode): four-level paging.
 *
 * Layout of the four PDPT slots that cover the low 4 GiB (every kernel and user
 * address lives here, so PML4[0] is the only PML4 entry we ever use):
 *   PDPT[0] -> kern_pd0   [0,1GiB)   identity map (2 MiB pages) + scratch PT
 * (kernel, shared) PDPT[1] -> (per-proc) [1,2GiB)   the user window, private
 * page tables      (user) PDPT[3] -> high_pd    [3,4GiB)   WIN_FB client buffer
 * (user) + device MMIO (mixed)
 *
 * Per-process isolation mirrors the 32-bit design: each process gets its own
 * PML4 + PDPT (so it can own a private user window) and a private COPY of the
 * mixed high PD (so it can map its own WIN_FB buffer at 3 GiB while sharing the
 * kernel's device-MMIO page tables by pointer). The purely-kernel PD (kern_pd0)
 * is shared by pointer across every address space. ===========================
 */

#define PAGE_PS 0x80                  /* page-size bit: PD entry = 2 MiB page */
#define PG_ADDR 0x000FFFFFFFFFF000ULL /* phys-address field of an entry */
#define LVL4(va) ((int)(((va) >> 39) & 0x1FF))
#define LVL3(va) ((int)(((va) >> 30) & 0x1FF))
#define LVL2(va) ((int)(((va) >> 21) & 0x1FF))
#define LVL1(va) ((int)(((va) >> 12) & 0x1FF))

/* Page-aligned, in low .bss (identity-mapped, so physaddr == virtaddr). */
static uint64_t master_pml4[512] __attribute__((aligned(4096)));
static uint64_t master_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kern_pd0[512] __attribute__((aligned(4096))); /* [0,1GiB)  */
static uint64_t high_pd[512] __attribute__((aligned(4096)));  /* [3,4GiB)  */
static uint64_t scratch_pt[512] __attribute__((aligned(4096)));

static uint32_t pool_next; /* next unused page (0 until primed)    */
static uint32_t pool_free; /* freelist head (0 = empty)           */

static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)va) : "memory"); }

void *palloc(void)
{
  uint32_t page;
  if (pool_free) {
    page = pool_free;
    pool_free = *(uint32_t *)(uintptr_t)page;
  } else if (pool_next + 4096 <= pmm_low_end()) {
    page = pool_next;
    pool_next += 4096;
  } else {
    return 0; /* pool exhausted                  */
  }
  uint64_t *p = (uint64_t *)(uintptr_t)page;
  for (int i = 0; i < 512; i++)
    p[i] = 0; /* hand out zeroed frames          */
  return p;
}

void pfree(void *page)
{
  *(uint32_t *)page = pool_free;
  pool_free = (uint32_t)(uintptr_t)page;
}

void paging_init(void)
{
  for (int i = 0; i < 512; i++) {
    master_pml4[i] = master_pdpt[i] = 0;
    kern_pd0[i] = high_pd[i] = scratch_pt[i] = 0;
  }

  /* Identity-map [0, pmm_low_end()) with 2 MiB pages (one PD entry each). */
  int n2m = (int)(pmm_low_end() >> 21);
  if (n2m < 8)
    n2m = 8; /* before pmm: legacy 16 MiB */
  if (n2m > 512)
    n2m = 512;
  for (int i = 0; i < n2m; i++)
    kern_pd0[i] = ((uint64_t)i << 21) | PAGE_PRESENT | PAGE_RW | PAGE_PS;

  /* Supervisor-only scratch PT, installed in the shared kernel PD so every
   * later address space inherits it (no PAGE_USER: ring 3 never reaches it). */
  kern_pd0[LVL2(SCRATCH_VADDR)] = ((uint64_t)(uintptr_t)scratch_pt) | PAGE_PRESENT | PAGE_RW;

  master_pdpt[0] = ((uint64_t)(uintptr_t)kern_pd0) | PAGE_PRESENT | PAGE_RW;
  /* high_pd carries user (WIN_FB) and kernel (MMIO) pages, so its parent must
   * permit user access; the kernel pages below stay supervisor-only. */
  master_pdpt[3] = ((uint64_t)(uintptr_t)high_pd) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  /* PML4[0] spans both the kernel identity map and the user window, so it must
   * permit user access; the kernel PDs/PTs below it stay supervisor-only. */
  master_pml4[0] = ((uint64_t)(uintptr_t)master_pdpt) | PAGE_PRESENT | PAGE_RW | PAGE_USER;

  pool_next = pmm_pool_start(); /* page-table pool: [pool_start, low_end) */

  /* Long mode is already on (boot.S); just install our tables. */
  __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)master_pml4) : "memory");
  {
    uintptr_t cr0 = read_cr0();
    write_cr0(cr0 | 0x10000); /* CR0.WP: supervisor writes honor RO PTEs */
  }
}

uint32_t paging_kernel_phys(void) { return (uint32_t)(uintptr_t)master_pml4; }

/* Walk pml4 down to the PTE for `va`, creating intermediate tables from the
 * pool with `iflags` (PAGE_USER set there propagates user access down the new
 * chain). If a 2 MiB large page already maps this PD slot, *large is set and 0
 * returned (the address is already mapped). Returns 0 with *large clear on pool
 * exhaustion. */
static uint64_t *map_walk(uint64_t *pml4, uint64_t va, uint64_t iflags, int *large)
{
  *large = 0;
  uint64_t *t = pml4;
  int idx[3] = {LVL4(va), LVL3(va), LVL2(va)};
  for (int l = 0; l < 3; l++) {
    int i = idx[l];
    if (!(t[i] & PAGE_PRESENT)) {
      void *p = palloc();
      if (!p)
        return 0;
      t[i] = ((uint64_t)(uintptr_t)p) | iflags;
    } else {
      if (t[i] & PAGE_PS) { /* already mapped by a large page  */
        *large = 1;
        return 0;
      }
      t[i] |= (iflags & PAGE_USER); /* widen a shared branch to user   */
    }
    t = (uint64_t *)(uintptr_t)(t[i] & PG_ADDR);
  }
  return &t[LVL1(va)];
}

/* Read-only walk: pointer to the PTE for `va`, or 0 if any level is absent (or
 * a large page covers it — callers here only probe 4 KiB user/kernel pages). */
static uint64_t *get_pte(uint64_t *pml4, uint64_t va)
{
  uint64_t *t = pml4;
  int idx[3] = {LVL4(va), LVL3(va), LVL2(va)};
  for (int l = 0; l < 3; l++) {
    int i = idx[l];
    if (!(t[i] & PAGE_PRESENT) || (t[i] & PAGE_PS))
      return 0;
    t = (uint64_t *)(uintptr_t)(t[i] & PG_ADDR);
  }
  return &t[LVL1(va)];
}

/* Pointer to the PD entry (2 MiB slot) for `va` in pml4, or 0 if absent. */
static uint64_t *get_pde(uint64_t *pml4, uint64_t va)
{
  uint64_t *t = pml4;
  int idx[2] = {LVL4(va), LVL3(va)};
  for (int l = 0; l < 2; l++) {
    int i = idx[l];
    if (!(t[i] & PAGE_PRESENT))
      return 0;
    t = (uint64_t *)(uintptr_t)(t[i] & PG_ADDR);
  }
  return &t[LVL2(va)];
}

static uint64_t *current_pml4(void)
{
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  return (uint64_t *)(uintptr_t)(cr3 & PG_ADDR);
}

/* --- per-process address spaces ------------------------------------------- */
uint32_t *paging_new_process_dir(void)
{
  uint64_t *pml4 = palloc();
  uint64_t *pdpt = palloc();
  uint64_t *hpd = palloc();
  if (!pml4 || !pdpt || !hpd) {
    if (hpd)
      pfree(hpd);
    if (pdpt)
      pfree(pdpt);
    if (pml4)
      pfree(pml4);
    return 0;
  }

  /* Share the kernel PDPT slots by pointer (kern_pd0 at [0]); the user window
   * at [1] starts empty and is filled per-process by paging_map_user_at. */
  for (int i = 0; i < 512; i++)
    pdpt[i] = master_pdpt[i];
  /* The mixed high PD is private (copy the shared MMIO PT pointers in; the
   * process adds its own WIN_FB page tables, which carry the PAGE_USER bit). */
  for (int i = 0; i < 512; i++)
    hpd[i] = high_pd[i];
  pdpt[3] = ((uint64_t)(uintptr_t)hpd) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  pml4[0] = ((uint64_t)(uintptr_t)pdpt) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  return (uint32_t *)(void *)pml4;
}

void paging_free_process_dir(uint32_t *dir)
{
  uint64_t *pml4 = (uint64_t *)(void *)dir;
  uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & PG_ADDR);

  /* User window [1,2GiB): free every present frame, then its page tables. */
  if (pdpt[1] & PAGE_PRESENT) {
    uint64_t *upd = (uint64_t *)(uintptr_t)(pdpt[1] & PG_ADDR);
    for (int d = 0; d < 512; d++) {
      if (!(upd[d] & PAGE_PRESENT) || (upd[d] & PAGE_PS))
        continue;
      uint64_t *pt = (uint64_t *)(uintptr_t)(upd[d] & PG_ADDR);
      for (int i = 0; i < 512; i++)
        if (pt[i] & PAGE_PRESENT)
          pmm_free_frame((uint32_t)(pt[i] & PG_ADDR));
      pfree(pt);
    }
    pfree(upd);
  }

  /* The private high PD: reclaim only the WIN_FB page tables (PAGE_USER bit);
   * MMIO entries point at shared kernel page tables — never free those. The
   * WIN_FB *frames* are window buffers owned elsewhere (freed by uwin), as in
   * the 32-bit path. */
  uint64_t *hpd = (uint64_t *)(uintptr_t)(pdpt[3] & PG_ADDR);
  for (int i = 0; i < 512; i++)
    if ((hpd[i] & PAGE_PRESENT) && (hpd[i] & PAGE_USER))
      pfree((void *)(uintptr_t)(hpd[i] & PG_ADDR));
  pfree(hpd);
  pfree(pdpt);
  pfree(pml4);
}

int paging_user_mapped(uint32_t vaddr)
{
  uint64_t *pte = get_pte(current_pml4(), vaddr);
  return pte && (*pte & PAGE_PRESENT) && (*pte & PAGE_USER);
}

int paging_user_range(uint32_t vaddr, uint32_t len, int write)
{
  uint64_t end;
  if (vaddr < USER_BASE || (uint64_t)vaddr + len > USER_REGION_END)
    return 0;
  if (!len)
    return 1;
  end = (uint64_t)vaddr + len;
  for (uint64_t a = vaddr & ~0xFFFULL; a < end; a += 4096) {
    uint64_t *pte = get_pte(current_pml4(), a);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER) || (write && !(*pte & PAGE_RW)))
      return 0;
  }
  return 1;
}

uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr)
{
  uint64_t *pte = get_pte((uint64_t *)(void *)dir, vaddr);
  return (pte && (*pte & PAGE_PRESENT)) ? (uint32_t)(*pte & PG_ADDR) : 0;
}

int paging_map_user_at(uint32_t *dir, uint32_t vaddr, uint32_t phys, int npages)
{
  uint64_t *pml4 = (uint64_t *)(void *)dir;
  for (int p = 0; p < npages; p++) {
    uint64_t a = (uint64_t)vaddr + (uint64_t)p * 4096;
    int large;
    uint64_t *pte = map_walk(pml4, a, PAGE_PRESENT | PAGE_RW | PAGE_USER, &large);
    if (!pte)
      return -1; /* pool exhausted (large can't happen here) */
    *pte = (((uint64_t)phys + (uint64_t)p * 4096) & PG_ADDR) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    invlpg(a);
  }
  return 0;
}

void paging_unmap_user_at(uint32_t *dir, uint32_t vaddr, uint32_t bytes)
{
  uint64_t *pml4 = (uint64_t *)(void *)dir;
  uint64_t start = vaddr & ~0x1FFFFFull; /* round to PD (2 MiB)    */
  uint64_t stop = ((uint64_t)vaddr + bytes + 0x1FFFFFull) & ~0x1FFFFFull;
  for (uint64_t base = start; base < stop; base += 0x200000ull) {
    uint64_t *pde = get_pde(pml4, base);
    if (!pde || !(*pde & PAGE_PRESENT) || (*pde & PAGE_PS))
      continue;
    pfree((void *)(uintptr_t)(*pde & PG_ADDR)); /* the private page table  */
    *pde = 0;
  }
}

int paging_handle_page_fault(uint32_t vaddr, uint32_t error)
{
  uint64_t *pte;
  uint32_t old, fresh;
  if (!(error & 2) || vaddr < USER_BASE || vaddr >= USER_REGION_END)
    return 0;
  pte = get_pte(current_pml4(), vaddr);
  if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER) || !(*pte & PAGE_COW))
    return 0;
  old = (uint32_t)(*pte & PG_ADDR);
  if (pmm_frame_refs(old) <= 1) {
    *pte = (*pte | PAGE_RW) & ~PAGE_COW;
    invlpg(vaddr);
    return 1;
  }
  fresh = pmm_alloc_frame();
  if (!fresh)
    return 0;
  {
    uint32_t fl = irq_save();
    uint32_t *src = paging_scratch_map(0, old);
    uint32_t *dst = paging_scratch_map(1, fresh);
    for (int i = 0; i < 1024; i++)
      dst[i] = src[i];
    paging_scratch_unmap(1);
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
  *pte = (fresh & PG_ADDR) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  invlpg(vaddr);
  pmm_free_frame(old);
  return 1;
}

uint32_t *paging_fork_dir(uint32_t *parent)
{
  uint64_t *pp = (uint64_t *)(void *)parent;
  uint32_t *cdir = paging_new_process_dir();
  if (!cdir)
    return 0;

  uint64_t *ppdpt = (uint64_t *)(uintptr_t)(pp[0] & PG_ADDR);
  if (ppdpt[1] & PAGE_PRESENT) {
    uint64_t *ppd = (uint64_t *)(uintptr_t)(ppdpt[1] & PG_ADDR);
    for (int d = 0; d < 512; d++) {
      if (!(ppd[d] & PAGE_PRESENT) || (ppd[d] & PAGE_PS))
        continue;
      uint64_t *ppt = (uint64_t *)(uintptr_t)(ppd[d] & PG_ADDR);
      for (int t = 0; t < 512; t++) {
        if (!(ppt[t] & PAGE_PRESENT))
          continue;
        uint64_t va = ((uint64_t)1 << 30) /* PDPT slot 1 = 1 GiB    */
                      + ((uint64_t)d << 21) + ((uint64_t)t << 12);
        uint32_t phys = (uint32_t)(ppt[t] & PG_ADDR);
        if (paging_map_user_at(cdir, (uint32_t)va, phys, 1) < 0) {
          paging_free_process_dir(cdir);
          return 0;
        }
        {
          uint64_t *cpte = get_pte((uint64_t *)(void *)cdir, va);
          ppt[t] = (ppt[t] & ~PAGE_RW) | PAGE_COW;
          *cpte = (*cpte & ~PAGE_RW) | PAGE_COW;
          invlpg(va);
          pmm_retain_frame(phys);
        }
      }
    }
  }
  return cdir;
}

int paging_identity_map(uint32_t phys, uint32_t len)
{
  uint64_t *pml4 = current_pml4();
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    int large;
    uint64_t *pte = map_walk(pml4, a, PAGE_PRESENT | PAGE_RW, &large);
    if (large)
      continue; /* already covered by a 2 MiB page */
    if (!pte)
      return -1;
    *pte = ((uint64_t)a & PG_ADDR) | PAGE_PRESENT | PAGE_RW;
    invlpg(a);
  }
  return 0;
}

int paging_kmap(uint32_t phys, uint32_t len)
{
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    int large;
    uint64_t *pte = map_walk(master_pml4, a, PAGE_PRESENT | PAGE_RW, &large);
    if (large)
      continue;
    if (!pte)
      return -1;
    *pte = ((uint64_t)a & PG_ADDR) | PAGE_PRESENT | PAGE_RW; /* supervisor */
    invlpg(a);
  }
  return 0;
}

void paging_kunmap(uint32_t phys, uint32_t len)
{
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint64_t *pte = get_pte(master_pml4, a);
    if (pte && (*pte & PAGE_PRESENT)) {
      *pte = 0;
      invlpg(a);
    }
  }
}

int paging_user_grow(uint32_t *dir, uint32_t old_top, uint32_t new_top)
{
  for (uint32_t page = old_top; page < new_top; page += 4096) {
    uint32_t phys = pmm_alloc_frame();
    int ok = phys != 0;
    if (ok) {
      uint32_t fl = irq_save();
      uint32_t *kv = paging_scratch_map(0, phys);
      for (int k = 0; k < 1024; k++)
        kv[k] = 0;
      paging_scratch_unmap(0);
      irq_restore(fl);
      if (paging_map_user_at(dir, page, phys, 1) < 0) {
        pmm_free_frame(phys);
        ok = 0;
      }
    }
    if (!ok) {
      paging_user_shrink(dir, old_top, page); /* undo this call */
      return -1;
    }
    invlpg(page);
  }
  return 0;
}

void paging_user_shrink(uint32_t *dir, uint32_t new_top, uint32_t old_top)
{
  uint64_t *pml4 = (uint64_t *)(void *)dir;
  for (uint32_t a = new_top; a < old_top; a += 4096) {
    uint64_t *pte = get_pte(pml4, a);
    if (pte && (*pte & PAGE_PRESENT)) {
      pmm_free_frame((uint32_t)(*pte & PG_ADDR));
      *pte = 0;
      invlpg(a);
    }
  }
}

void *paging_scratch_map(int slot, uint32_t phys)
{
  if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
    return 0;
  uint64_t va = SCRATCH_VADDR + (uint64_t)slot * 4096;
  scratch_pt[LVL1(va)] = ((uint64_t)phys & PG_ADDR) | PAGE_PRESENT | PAGE_RW;
  invlpg(va);
  return (void *)(uintptr_t)va;
}

void paging_scratch_unmap(int slot)
{
  if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
    return;
  uint64_t va = SCRATCH_VADDR + (uint64_t)slot * 4096;
  scratch_pt[LVL1(va)] = 0;
  invlpg(va);
}

void paging_mark_wc(uint32_t virt, uint32_t len)
{
  uint64_t *pml4 = current_pml4();
  uint32_t start = virt & ~0xFFFu;
  uint32_t stop = (virt + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint64_t *pte = get_pte(pml4, a);
    if (pte && (*pte & PAGE_PRESENT)) {
      *pte |= PAGE_PWT;
      invlpg(a);
    }
  }
}

void paging_mark_uc(uint32_t virt, uint32_t len)
{
  uint64_t *pml4 = current_pml4();
  uint32_t start = virt & ~0xFFFu;
  uint32_t stop = (virt + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint64_t *pte = get_pte(pml4, a);
    if (pte && (*pte & PAGE_PRESENT)) {
      *pte |= PAGE_PCD;
      invlpg(a);
    }
  }
}

#else
/* ===========================================================================
 * 32-bit (i386): classic two-level paging. Original, unchanged. =============
 */

/* Page-directory index range covering the user window [USER_BASE,
 * USER_REGION_END). Each spanned PDE holds a private user page table; nothing
 * in the shared kernel directory maps here, so a present PDE in this range is
 * always process-private. */
#define USER_PDI_LO (USER_BASE >> 22)
#define USER_PDI_HI (USER_REGION_END >> 22)

#define PD_ENTRIES 1024
#define PT_ENTRIES 1024
/* Max identity tables: 48 * 4 MiB = 192 MiB, the cap on the RAM-scaled low
 * region (pmm_low_end()). paging_init maps only as many as that region needs;
 * the rest sit unused in .bss (192 KiB). The region must stay below
 * SCRATCH_VADDR. */
#define IDENTITY_TABLES 48

/* Page-aligned so their physical addresses are valid PDE/PTE pointers. Living
 * in .bss (below ~4 MiB) they are themselves identity-mapped. */
static uint32_t page_directory[PD_ENTRIES] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));

static uint32_t scratch_pt[PT_ENTRIES] __attribute__((aligned(4096)));

/* Page-table pool cursor (primed in paging_init from pmm_pool_start). */
static uint32_t pool_next; /* next unused page (0 until primed)    */
static uint32_t pool_free; /* freelist head (0 = empty)           */

void paging_init(void)
{
  for (int i = 0; i < PD_ENTRIES; i++)
    page_directory[i] = 0; /* not present                         */

  /* Identity-map [0, pmm_low_end()) — the RAM-scaled kernel/heap/pool region.
   * One table per 4 MiB, up to IDENTITY_TABLES. */
  int ntables = (int)(pmm_low_end() >> 22);
  if (ntables < 1)
    ntables = 4; /* before pmm_init: legacy 16 MiB */
  if (ntables > IDENTITY_TABLES)
    ntables = IDENTITY_TABLES;
  for (int t = 0; t < ntables; t++) {
    for (int i = 0; i < PT_ENTRIES; i++) {
      uint32_t frame = ((uint32_t)t * PT_ENTRIES + i) * 4096;
      page_tables[t][i] = frame | PAGE_PRESENT | PAGE_RW;
    }
    page_directory[t] = (uint32_t)page_tables[t] | PAGE_PRESENT | PAGE_RW;
  }

  /* Install the supervisor-only scratch page table (entries start empty). It
   * lives in the shared kernel directory, so every later process dir inherits
   * it and the kernel can use scratch under any CR3. No PAGE_USER: ring 3 can
   * never reach SCRATCH_VADDR. */
  for (int i = 0; i < PT_ENTRIES; i++)
    scratch_pt[i] = 0;
  page_directory[SCRATCH_VADDR >> 22] = (uint32_t)scratch_pt | PAGE_PRESENT | PAGE_RW;

  pool_next = pmm_pool_start(); /* page-table pool: [pool_start, low_end) */

  __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));

  uintptr_t cr0 = read_cr0();
  cr0 |= 0x80010000; /* CR0.PG + CR0.WP                     */
  write_cr0(cr0);
}

uint32_t paging_kernel_phys(void) { return (uint32_t)page_directory; }

/* --- page-frame pool over [pmm_pool_start(), pmm_low_end()) (identity-mapped)
 * - RAM-scaled (see pmm.c); on a 256 MiB box that is the legacy [12 MiB, 16
 * MiB). User program/stack frames come from pmm high memory; the pool backs
 * only page directories + page tables (a few frames per process), so it scales
 * with how many processes can exist. pool_next is primed in paging_init (after
 * pmm). */

void *palloc(void)
{
  uint32_t page;
  if (pool_free) {
    page = pool_free;
    pool_free = *(uint32_t *)page; /* next link stored in the page    */
  } else if (pool_next + 4096 <= pmm_low_end()) {
    page = pool_next;
    pool_next += 4096;
  } else {
    return 0; /* pool exhausted                  */
  }
  for (int i = 0; i < 1024; i++)
    ((uint32_t *)page)[i] = 0; /* hand out zeroed frames          */
  return (void *)page;
}

void pfree(void *page)
{
  *(uint32_t *)page = pool_free;
  pool_free = (uint32_t)page;
}

void *paging_scratch_map(int slot, uint32_t phys)
{
  if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
    return 0;
  uint32_t va = SCRATCH_VADDR + (uint32_t)slot * 4096;
  scratch_pt[(va >> 12) & 0x3FF] = (phys & ~0xFFFu) | PAGE_PRESENT | PAGE_RW;
  __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
  return (void *)va;
}

void paging_scratch_unmap(int slot)
{
  if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
    return;
  uint32_t va = SCRATCH_VADDR + (uint32_t)slot * 4096;
  scratch_pt[(va >> 12) & 0x3FF] = 0;
  __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* --- per-process address spaces ------------------------------------------- */
uint32_t *paging_new_process_dir(void)
{
  uint32_t *dir = palloc();
  if (!dir)
    return 0;

  /* Inherit every shared kernel/heap/pool/scratch/MMIO mapping (all
   * supervisor-only). The whole low 4 MiB stays the shared identity table, so
   * ring 3 can't touch it. The user window PDEs are empty here and filled by
   * paging_map_user_at with this process's PRIVATE page tables. */
  for (int i = 0; i < PD_ENTRIES; i++)
    dir[i] = page_directory[i];
  return dir;
}

void paging_free_process_dir(uint32_t *dir)
{
  /* Retire any guarded thread-stack slots this address space owned, so the
   * per-CR3 slot table can't later match a stale dir pointer (loader.c). */
  extern void user_stack_reset_owner(uint32_t *dir);
  user_stack_reset_owner(dir);
  /* Free the user window: each present PDE in the window range is a private
   * page table; free its pmm-backed frames, then the table itself (pool). */
  for (uint32_t pdi = USER_PDI_LO; pdi < USER_PDI_HI; pdi++) {
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    for (int i = 0; i < PT_ENTRIES; i++)
      if (pt[i] & PAGE_PRESENT)
        pmm_free_frame(pt[i] & ~0xFFFu);
    pfree(pt);
    dir[pdi] = 0;
  }
  /* Reclaim private page tables above the 2 GiB window: the high-vaddr client
   * buffers (WIN_FB / COMPOSITOR_FB / WIN_MAP) that paging_map_user_at installs
   * carry PAGE_USER on their PDE, while every shared kernel/heap/pool/MMIO PDE
   * is supervisor-only — so a PAGE_USER PDE here is always process-private.
   * Free only the page table (pool page); the frames themselves are
   * window/back-buffer memory owned by uwin/fb and freed there. Without this
   * the pool leaked one page table per mapped window buffer at every process
   * exit. */
  for (uint32_t pdi = USER_PDI_HI; pdi < PD_ENTRIES; pdi++) {
    if ((dir[pdi] & PAGE_PRESENT) && (dir[pdi] & PAGE_USER)) {
      pfree((void *)(dir[pdi] & ~0xFFFu));
      dir[pdi] = 0;
    }
  }
  pfree(dir);
}

/* Is `vaddr` a present, user-accessible page in the CURRENT address space?
 * Reads cr3 (so it works for threads, which carry dir=0) and walks the tables,
 * all identity-mapped. Lets the crash handler probe a frame pointer before
 * dereferencing it for a backtrace, so a corrupt one can't double-fault. */
int paging_user_mapped(uint32_t vaddr)
{
  uint32_t cr3 = (uint32_t)read_cr3();
  uint32_t *pd = (uint32_t *)(cr3 & ~0xFFFu);
  uint32_t pde = pd[vaddr >> 22];
  if (!(pde & PAGE_PRESENT))
    return 0;
  uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
  uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
  return (pte & PAGE_PRESENT) && (pte & PAGE_USER);
}

int paging_user_range(uint32_t vaddr, uint32_t len, int write)
{
  uint64_t end;
  if (vaddr < USER_BASE || (uint64_t)vaddr + len > USER_REGION_END)
    return 0;
  if (!len)
    return 1;
  end = (uint64_t)vaddr + len;
  for (uint32_t a = vaddr & ~0xFFFu; (uint64_t)a < end; a += 4096) {
    uint32_t pde = ((uint32_t *)read_cr3())[a >> 22];
    uint32_t *pt;
    uint32_t pte;
    if (!(pde & PAGE_PRESENT))
      return 0;
    pt = (uint32_t *)(pde & ~0xFFFu);
    pte = pt[(a >> 12) & 0x3FF];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER) || (write && !(pte & PAGE_RW)))
      return 0;
  }
  return 1;
}

uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr)
{
  uint32_t pde = dir[vaddr >> 22];
  if (!(pde & PAGE_PRESENT))
    return 0;
  uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
  uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
  return (pte & PAGE_PRESENT) ? (pte & ~0xFFFu) : 0;
}

int paging_handle_page_fault(uint32_t vaddr, uint32_t error)
{
  uint32_t *pd, pde, *pt, pte, old, fresh;
  if (!(error & 2) || vaddr < USER_BASE || vaddr >= USER_REGION_END)
    return 0;
  pd = (uint32_t *)(uintptr_t)read_cr3();
  pde = pd[vaddr >> 22];
  if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER))
    return 0;
  pt = (uint32_t *)(pde & ~0xFFFu);
  pte = pt[(vaddr >> 12) & 0x3FF];
  if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER) || !(pte & PAGE_COW))
    return 0;
  old = pte & ~0xFFFu;
  if (pmm_frame_refs(old) <= 1) {
    pt[(vaddr >> 12) & 0x3FF] = (pte | PAGE_RW) & ~PAGE_COW;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
  }
  fresh = pmm_alloc_frame();
  if (!fresh)
    return 0;
  {
    uint32_t fl = irq_save();
    uint32_t *src = paging_scratch_map(0, old);
    uint32_t *dst = paging_scratch_map(1, fresh);
    for (int i = 0; i < 1024; i++)
      dst[i] = src[i];
    paging_scratch_unmap(1);
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
  pt[(vaddr >> 12) & 0x3FF] = fresh | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
  pmm_free_frame(old);
  return 1;
}

uint32_t *paging_fork_dir(uint32_t *parent)
{
  uint32_t *child = paging_new_process_dir();
  if (!child)
    return 0;

  for (uint32_t pdi = USER_PDI_LO; pdi < USER_PDI_HI; pdi++) {
    if (!(parent[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *ppt = (uint32_t *)(parent[pdi] & ~0xFFFu);
    for (int i = 0; i < PT_ENTRIES; i++) {
      if (!(ppt[i] & PAGE_PRESENT))
        continue;
      uint32_t vaddr = (pdi << 22) | ((uint32_t)i << 12);
      uint32_t phys = ppt[i] & ~0xFFFu;
      if (paging_map_user_at(child, vaddr, phys, 1) < 0) {
        paging_free_process_dir(child);
        return 0;
      }
      {
        uint32_t *cpt = (uint32_t *)(child[vaddr >> 22] & ~0xFFFu);
        ppt[i] = (ppt[i] & ~PAGE_RW) | PAGE_COW;
        cpt[(vaddr >> 12) & 0x3FF] = (cpt[(vaddr >> 12) & 0x3FF] & ~PAGE_RW) | PAGE_COW;
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
        pmm_retain_frame(phys);
      }
    }
  }
  return child;
}

int paging_identity_map(uint32_t phys, uint32_t len)
{
  uint32_t *dir;
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir)); /* current address space  */

  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint32_t pdi = a >> 22;
    uint32_t pti = (a >> 12) & 0x3FF;
    uint32_t *pt;
    if (dir[pdi] & PAGE_PRESENT) {
      pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    } else {
      pt = palloc();
      if (!pt)
        return -1;
      dir[pdi] = (uint32_t)pt | PAGE_PRESENT | PAGE_RW;
    }
    pt[pti] = a | PAGE_PRESENT | PAGE_RW;
    __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
  }
  return 0;
}

/* Map [phys, phys+len) into the kernel master directory (page_directory) as
 * supervisor RW. A window's client buffer lives in >16 MiB physical memory
 * (outside the identity map); the WM compositor runs under page_directory, so
 * this is how it reaches the buffer to blit it. New PDE page tables come from
 * the pool and are kept (a bounded set, since RAM is bounded). Returns 0, or -1
 * if the pool is exhausted. Idempotent for an already-mapped range. */
int paging_kmap(uint32_t phys, uint32_t len)
{
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    uint32_t *pt;
    if (page_directory[pdi] & PAGE_PRESENT) {
      pt = (uint32_t *)(page_directory[pdi] & ~0xFFFu);
    } else {
      pt = palloc();
      if (!pt)
        return -1;
      page_directory[pdi] = (uint32_t)pt | PAGE_PRESENT | PAGE_RW;
    }
    pt[pti] = a | PAGE_PRESENT | PAGE_RW; /* supervisor: kernel only */
    __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
  }
  return 0;
}

/* Drop a range previously mapped by paging_kmap (clears the PTEs; the pooled
 * page tables stay, ready for the next buffer that lands in the same PDE). */
void paging_kunmap(uint32_t phys, uint32_t len)
{
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    if (!(page_directory[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(page_directory[pdi] & ~0xFFFu);
    pt[pti] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
  }
}

/* Map npages of [phys..] at `vaddr` in `dir` as user pages, allocating a
 * private USER page table for each spanned page-directory entry. Unlike
 * paging_map_user_page (low-4-MiB window only), this serves a window's client
 * buffer at a high vaddr with its own page tables. Returns 0, or -1 on pool
 * exhaustion (partial mappings are cleaned up by paging_unmap_user_at). */
int paging_map_user_at(uint32_t *dir, uint32_t vaddr, uint32_t phys, int npages)
{
  for (int p = 0; p < npages; p++) {
    uint32_t a = vaddr + (uint32_t)p * 4096;
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    uint32_t *pt;
    if (dir[pdi] & PAGE_PRESENT) {
      pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    } else {
      pt = palloc();
      if (!pt)
        return -1;
      dir[pdi] = (uint32_t)pt | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    pt[pti] = ((phys + (uint32_t)p * 4096) & ~0xFFFu) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  }
  return 0;
}

/* Tear down a high-vaddr user mapping made by paging_map_user_at: clear the
 * PTEs and free a spanned PDE's page table once it holds no more mappings.
 * `bytes` is the buffer's reserved vaddr span (so every PDE it could touch is
 * reclaimed). */
void paging_unmap_user_at(uint32_t *dir, uint32_t vaddr, uint32_t bytes)
{
  uint32_t start = vaddr & ~0x3FFFFFu; /* round to PDE (4 MiB)    */
  uint32_t stop = (vaddr + bytes + 0x3FFFFFu) & ~0x3FFFFFu;
  for (uint32_t base = start; base < stop; base += 0x400000u) {
    uint32_t pdi = base >> 22;
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    pfree((void *)(dir[pdi] & ~0xFFFu)); /* the private page table  */
    dir[pdi] = 0;
  }
}

/* Heap grow: map fresh, zeroed user pages for [old_top, new_top) (both page-
 * aligned) into the process window `dir`. New frames are zero-filled through
 * the supervisor scratch slot so no prior tenant's bytes leak to ring 3, and
 * each is invlpg'd in case a not-present translation was cached. Returns 0, or
 * -1 on pmm/ pool exhaustion after rolling back the pages this call had mapped.
 * Backs the user-space heap (SYS_SBRK); the frames are reclaimed at process
 * exit by paging_free_process_dir, which walks every present window PTE. */
int paging_user_grow(uint32_t *dir, uint32_t old_top, uint32_t new_top)
{
  for (uint32_t page = old_top; page < new_top; page += 4096) {
    uint32_t phys = pmm_alloc_frame();
    int ok = phys != 0;
    if (ok) {
      uint32_t fl = irq_save();
      uint32_t *kv = paging_scratch_map(0, phys);
      for (int k = 0; k < 1024; k++)
        kv[k] = 0;
      paging_scratch_unmap(0);
      irq_restore(fl);
      if (paging_map_user_at(dir, page, phys, 1) < 0) {
        pmm_free_frame(phys);
        ok = 0;
      }
    }
    if (!ok) {
      paging_user_shrink(dir, old_top, page); /* undo this call */
      return -1;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
  }
  return 0;
}

/* Heap shrink: unmap and free the user data frames for [new_top, old_top) (both
 * page-aligned). Page tables are left in place (they get reused as the heap
 * grows again and are reclaimed wholesale at process exit). */
void paging_user_shrink(uint32_t *dir, uint32_t new_top, uint32_t old_top)
{
  for (uint32_t a = new_top; a < old_top; a += 4096) {
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    if (pt[pti] & PAGE_PRESENT) {
      pmm_free_frame(pt[pti] & ~0xFFFu);
      pt[pti] = 0;
      __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
  }
}

void paging_mark_wc(uint32_t virt, uint32_t len)
{
  uint32_t *dir;
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir));

  uint32_t start = virt & ~0xFFFu;
  uint32_t stop = (virt + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint32_t pdi = a >> 22;
    uint32_t pti = (a >> 12) & 0x3FF;
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    if (pt[pti] & PAGE_PRESENT) {
      pt[pti] |= PAGE_PWT;
      __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
  }
}

void paging_mark_uc(uint32_t virt, uint32_t len)
{
  uint32_t *dir;
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir));

  uint32_t start = virt & ~0xFFFu;
  uint32_t stop = (virt + len + 0xFFFu) & ~0xFFFu;
  for (uint32_t a = start; a < stop && a >= start; a += 4096) {
    uint32_t pdi = a >> 22;
    uint32_t pti = (a >> 12) & 0x3FF;
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    if (pt[pti] & PAGE_PRESENT) {
      pt[pti] |= PAGE_PCD;
      __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
  }
}

#endif /* __x86_64__ */
