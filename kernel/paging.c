/* paging.c: kernel address space bring-up for i386 (2-level) and x86_64 (4-level), identity-mapped. */

#include "paging.h"
#include "creg.h"
#include "irqflags.h"
#include "kstring.h"
#include "loader.h" /* USER_BASE / USER_REGION_END: the per-process window */
#include "pmm.h"
#include <stdint.h>

/* IF-off bracket around scratch-slot use so a fork copy can't be preempted by another scratch user. */

#define PAGE_PRESENT 0x1
#define PAGE_RW 0x2
#define PAGE_USER 0x4
#define PAGE_PWT 0x8   /* write-through bit; selects PAT entry 1 here */
#define PAGE_PCD 0x10  /* cache-disable bit; UC- mapping for MMIO regs */
#define PAGE_COW 0x200 /* software bit: user page is shared read-only */

/* Scratch window: one PT in the shared kernel mapping to transiently map any pmm frame. */
#define SCRATCH_VADDR 0x10000000u

static volatile uint32_t page_pool_lock;
static volatile uint32_t scratch_lock;

static void page_pool_acquire(void)
{
  while (__sync_lock_test_and_set(&page_pool_lock, 1))
    __asm__ volatile("pause");
}

static void page_pool_release(void) { __sync_lock_release(&page_pool_lock); }

static void scratch_acquire(void)
{
  while (__sync_lock_test_and_set(&scratch_lock, 1))
    __asm__ volatile("pause");
}

static void scratch_release(void) { __sync_lock_release(&scratch_lock); }

#ifdef __x86_64__
/* 64-bit layout: PDPT[0]=kern_pd0 (shared), [1]=private user window, [3]=high_pd (WIN_FB + MMIO). */

#define PAGE_PS 0x80                  /* page-size bit: PD entry = 2 MiB page */
#define PG_ADDR 0x000FFFFFFFFFF000ULL /* phys-address field of an entry */
#define LVL4(va) ((int)(((va) >> 39) & 0x1FF))
#define LVL3(va) ((int)(((va) >> 30) & 0x1FF))
#define LVL2(va) ((int)(((va) >> 21) & 0x1FF))
#define LVL1(va) ((int)(((va) >> 12) & 0x1FF))

/* Page-aligned, in low .bss (identity-mapped, so physaddr == virtaddr). */
static uint64_t master_pml4[512] __attribute__((aligned(4096)));
static uint64_t master_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kern_pd0[512] __attribute__((aligned(4096))); /* [0,1GiB) */
static uint64_t high_pd[512] __attribute__((aligned(4096)));  /* [3,4GiB) */
static uint64_t scratch_pt[512] __attribute__((aligned(4096)));

static uint32_t pool_next; /* next unused page (0 until primed) */
static uint32_t pool_free; /* freelist head (0 = empty) */

static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)va) : "memory"); }

void *palloc(void)
{
  uintptr_t flags = irq_save();
  page_pool_acquire();
  uint32_t page;
  if (pool_free) {
    page = pool_free;
    pool_free = *(uint32_t *)(uintptr_t)page;
  } else if (pool_next + 4096 <= pmm_low_end()) {
    page = pool_next;
    pool_next += 4096;
  } else {
    page_pool_release();
    irq_restore(flags);
    return 0; /* pool exhausted */
  }
  uint64_t *p = (uint64_t *)(uintptr_t)page;
  page_pool_release();
  irq_restore(flags);
  memset(p, 0, 4096); /* hand out zeroed frames */
  return p;
}

void pfree(void *page)
{
  uintptr_t flags = irq_save();
  page_pool_acquire();
  *(uint32_t *)page = pool_free;
  pool_free = (uint32_t)(uintptr_t)page;
  page_pool_release();
  irq_restore(flags);
}

void paging_init(void)
{
  memset(master_pml4, 0, sizeof master_pml4);
  memset(master_pdpt, 0, sizeof master_pdpt);
  memset(kern_pd0, 0, sizeof kern_pd0);
  memset(high_pd, 0, sizeof high_pd);
  memset(scratch_pt, 0, sizeof scratch_pt);

  /* Identity-map [0, pmm_low_end()) with 2 MiB pages (one PD entry each). */
  int n2m = (int)(pmm_low_end() >> 21);
  if (n2m < 8)
    n2m = 8; /* before pmm: legacy 16 MiB */
  if (n2m > 512)
    n2m = 512;
  for (int i = 0; i < n2m; i++)
    kern_pd0[i] = ((uint64_t)i << 21) | PAGE_PRESENT | PAGE_RW | PAGE_PS;

  /* Supervisor-only scratch PT in the shared kernel PD, inherited by every address space. */
  kern_pd0[LVL2(SCRATCH_VADDR)] = ((uint64_t)(uintptr_t)scratch_pt) | PAGE_PRESENT | PAGE_RW;

  master_pdpt[0] = ((uint64_t)(uintptr_t)kern_pd0) | PAGE_PRESENT | PAGE_RW;
  /* high_pd mixes user and kernel pages, so its parent permits user; kernel pages stay supervisor-only. */
  master_pdpt[3] = ((uint64_t)(uintptr_t)high_pd) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  /* PML4[0] spans kernel and user window, so it permits user; lower kernel tables stay supervisor-only. */
  master_pml4[0] = ((uint64_t)(uintptr_t)master_pdpt) | PAGE_PRESENT | PAGE_RW | PAGE_USER;

  pool_next = pmm_pool_start(); /* page-table pool: [pool_start, low_end) */

  /* Long mode is already on (boot.S); just install our tables. */
  __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)master_pml4) : "memory");
  {
    uintptr_t cr0 = read_cr0();
    write_cr0(cr0 | 0x10000); /* CR0.WP: supervisor writes honor RO PTEs */
  }
}

uintptr_t paging_kernel_phys(void) { return (uintptr_t)master_pml4; }

/* Walk to the PTE for `va`, allocating tables; *large set and 0 returned if a 2 MiB page maps it. */
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
      if (t[i] & PAGE_PS) { /* already mapped by a large page */
        *large = 1;
        return 0;
      }
      /* A supervisor page table here is a shared kernel one (MMIO): user PTEs must not land in it. */
      if (l == 2 && (iflags & PAGE_USER) && !(t[i] & PAGE_USER))
        return 0;
      t[i] |= (iflags & PAGE_USER); /* widen a shared branch to user */
    }
    t = (uint64_t *)(uintptr_t)(t[i] & PG_ADDR);
  }
  return &t[LVL1(va)];
}

/* Read-only walk: PTE for `va`, or 0 if absent or covered by a large page. */
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

/* per-process address spaces */
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

  /* Share kernel PDPT slots by pointer; the user window at [1] starts empty. */
  memcpy(pdpt, master_pdpt, sizeof master_pdpt);
  /* The high PD is private: copy shared MMIO PT pointers; the process adds its own WIN_FB tables. */
  memcpy(hpd, high_pd, sizeof high_pd);
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

  /* Free only WIN_FB page tables (PAGE_USER); MMIO tables are shared, WIN_FB frames are freed by uwin. */
  uint64_t *hpd = (uint64_t *)(uintptr_t)(pdpt[3] & PG_ADDR);
  for (int i = 0; i < 512; i++)
    if ((hpd[i] & PAGE_PRESENT) && (hpd[i] & PAGE_USER))
      pfree((void *)(uintptr_t)(hpd[i] & PG_ADDR));
  pfree(hpd);
  pfree(pdpt);
  pfree(pml4);
}

bool paging_user_mapped(uint32_t vaddr)
{
  uint64_t *pte = get_pte(current_pml4(), vaddr);
  return pte && (*pte & PAGE_PRESENT) && (*pte & PAGE_USER);
}

bool paging_user_range(uint32_t vaddr, uint32_t len, bool write)
{
  uint64_t end;
  if (vaddr < USER_BASE || (uint64_t)vaddr + len > USER_REGION_END)
    return false;
  if (!len)
    return true;
  end = (uint64_t)vaddr + len;
  for (uint64_t a = vaddr & ~0xFFFULL; a < end; a += 4096) {
    uint64_t *pte = get_pte(current_pml4(), a);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER) || (write && !(*pte & PAGE_RW) && !(*pte & PAGE_COW)))
      return false;
  }
  return true;
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
  uint64_t start = vaddr & ~0x1FFFFFull; /* round to PD (2 MiB) */
  uint64_t stop = ((uint64_t)vaddr + bytes + 0x1FFFFFull) & ~0x1FFFFFull;
  for (uint64_t base = start; base < stop; base += 0x200000ull) {
    uint64_t *pde = get_pde(pml4, base);
    if (!pde || !(*pde & PAGE_PRESENT) || (*pde & PAGE_PS) || !(*pde & PAGE_USER))
      continue;                                 /* absent, or a shared kernel table this call never allocated */
    pfree((void *)(uintptr_t)(*pde & PG_ADDR)); /* the private page table */
    *pde = 0;
  }
}

bool paging_handle_page_fault(uint32_t vaddr, uint32_t error)
{
  uint64_t *pte;
  uint32_t old, fresh;
  if (!(error & 2) || vaddr < USER_BASE || vaddr >= USER_REGION_END)
    return false;
  pte = get_pte(current_pml4(), vaddr);
  if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER) || !(*pte & PAGE_COW))
    return false;
  old = (uint32_t)(*pte & PG_ADDR);
  if (pmm_frame_refs(old) <= 1) {
    *pte = (*pte | PAGE_RW) & ~PAGE_COW;
    invlpg(vaddr);
    return true;
  }
  fresh = pmm_alloc_frame();
  if (!fresh)
    return false;
  {
    uint32_t fl = irq_save();
    uint32_t *src = paging_scratch_map(0, old);
    uint32_t *dst = paging_scratch_map(1, fresh);
    memcpy(dst, src, 4096);
    paging_scratch_unmap(1);
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
  *pte = (fresh & PG_ADDR) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  invlpg(vaddr);
  pmm_free_frame(old);
  return true;
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
        uint64_t va = ((uint64_t)1 << 30) /* PDPT slot 1 = 1 GiB */
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
      memset(kv, 0, 4096);
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
  if (slot == 0)
    scratch_acquire();
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
  if (slot == 0)
    scratch_release();
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
/* 32-bit (i386): two-level paging. */

/* PDE range of the user window; a present PDE here is always process-private. */
#define USER_PDI_LO (USER_BASE >> 22)
#define USER_PDI_HI (USER_REGION_END >> 22)

#define PD_ENTRIES 1024
#define PT_ENTRIES 1024
/* Max identity tables: 48 * 4 MiB = 192 MiB, must stay below SCRATCH_VADDR. */
#define IDENTITY_TABLES 48

/* Page-aligned so their physical addresses are valid PDE/PTE pointers. */
static uint32_t page_directory[PD_ENTRIES] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));

static uint32_t scratch_pt[PT_ENTRIES] __attribute__((aligned(4096)));

/* Page-table pool cursor (primed in paging_init from pmm_pool_start). */
static uint32_t pool_next; /* next unused page (0 until primed) */
static uint32_t pool_free; /* freelist head (0 = empty) */

void paging_init(void)
{
  memset(page_directory, 0, sizeof page_directory); /* not present */

  /* Identity-map [0, pmm_low_end()), one table per 4 MiB, up to IDENTITY_TABLES. */
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

  /* Supervisor-only scratch PT in the shared kernel directory, usable under any CR3. */
  memset(scratch_pt, 0, sizeof scratch_pt);
  page_directory[SCRATCH_VADDR >> 22] = (uint32_t)scratch_pt | PAGE_PRESENT | PAGE_RW;

  pool_next = pmm_pool_start(); /* page-table pool: [pool_start, low_end) */

  __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));

  uintptr_t cr0 = read_cr0();
  cr0 |= 0x80010000; /* CR0.PG + CR0.WP */
  write_cr0(cr0);
}

uintptr_t paging_kernel_phys(void) { return (uintptr_t)page_directory; }

/* Frame pool over [pmm_pool_start(), pmm_low_end()): backs page directories and tables only. */

void *palloc(void)
{
  uintptr_t flags = irq_save();
  page_pool_acquire();
  uint32_t page;
  if (pool_free) {
    page = pool_free;
    pool_free = *(uint32_t *)page; /* next link stored in the page */
  } else if (pool_next + 4096 <= pmm_low_end()) {
    page = pool_next;
    pool_next += 4096;
  } else {
    page_pool_release();
    irq_restore(flags);
    return 0; /* pool exhausted */
  }
  page_pool_release();
  irq_restore(flags);
  memset((void *)(uintptr_t)page, 0, 4096); /* hand out zeroed frames */
  return (void *)page;
}

void pfree(void *page)
{
  uintptr_t flags = irq_save();
  page_pool_acquire();
  *(uint32_t *)page = pool_free;
  pool_free = (uint32_t)page;
  page_pool_release();
  irq_restore(flags);
}

void *paging_scratch_map(int slot, uint32_t phys)
{
  if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
    return 0;
  if (slot == 0)
    scratch_acquire();
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
  if (slot == 0)
    scratch_release();
}

/* per-process address spaces */
uint32_t *paging_new_process_dir(void)
{
  uint32_t *dir = palloc();
  if (!dir)
    return 0;

  /* Inherit shared supervisor-only mappings; user window PDEs start empty. */
  memcpy(dir, page_directory, sizeof page_directory);
  return dir;
}

void paging_free_process_dir(uint32_t *dir)
{
  /* Retire guarded thread-stack slots so the per-CR3 slot table can't match a stale dir (loader.c). */
  extern void user_stack_reset_owner(uint32_t *dir);
  user_stack_reset_owner(dir);
  /* Free the user window: each present PDE is a private table; free its frames, then the table. */
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
  /* Free private page tables of high-vaddr client buffers (PAGE_USER PDEs); the frames belong to uwin/fb. */
  for (uint32_t pdi = USER_PDI_HI; pdi < PD_ENTRIES; pdi++) {
    if ((dir[pdi] & PAGE_PRESENT) && (dir[pdi] & PAGE_USER)) {
      pfree((void *)(dir[pdi] & ~0xFFFu));
      dir[pdi] = 0;
    }
  }
  pfree(dir);
}

/* Is `vaddr` a present user page in the current address space? Safe for crash-handler backtraces. */
bool paging_user_mapped(uint32_t vaddr)
{
  uint32_t cr3 = (uint32_t)read_cr3();
  uint32_t *pd = (uint32_t *)(cr3 & ~0xFFFu);
  uint32_t pde = pd[vaddr >> 22];
  if (!(pde & PAGE_PRESENT))
    return false;
  uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
  uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
  return (pte & PAGE_PRESENT) && (pte & PAGE_USER);
}

bool paging_user_range(uint32_t vaddr, uint32_t len, bool write)
{
  uint64_t end;
  if (vaddr < USER_BASE || (uint64_t)vaddr + len > USER_REGION_END)
    return false;
  if (!len)
    return true;
  end = (uint64_t)vaddr + len;
  for (uint32_t a = vaddr & ~0xFFFu; (uint64_t)a < end; a += 4096) {
    uint32_t pde = ((uint32_t *)read_cr3())[a >> 22];
    uint32_t *pt;
    uint32_t pte;
    if (!(pde & PAGE_PRESENT))
      return false;
    pt = (uint32_t *)(pde & ~0xFFFu);
    pte = pt[(a >> 12) & 0x3FF];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER) || (write && !(pte & PAGE_RW) && !(pte & PAGE_COW)))
      return false;
  }
  return true;
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

bool paging_handle_page_fault(uint32_t vaddr, uint32_t error)
{
  uint32_t *pd, pde, *pt, pte, old, fresh;
  if (!(error & 2) || vaddr < USER_BASE || vaddr >= USER_REGION_END)
    return false;
  pd = (uint32_t *)(uintptr_t)read_cr3();
  pde = pd[vaddr >> 22];
  if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER))
    return false;
  pt = (uint32_t *)(pde & ~0xFFFu);
  pte = pt[(vaddr >> 12) & 0x3FF];
  if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER) || !(pte & PAGE_COW))
    return false;
  old = pte & ~0xFFFu;
  if (pmm_frame_refs(old) <= 1) {
    pt[(vaddr >> 12) & 0x3FF] = (pte | PAGE_RW) & ~PAGE_COW;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return true;
  }
  fresh = pmm_alloc_frame();
  if (!fresh)
    return false;
  {
    uint32_t fl = irq_save();
    uint32_t *src = paging_scratch_map(0, old);
    uint32_t *dst = paging_scratch_map(1, fresh);
    memcpy(dst, src, 4096);
    paging_scratch_unmap(1);
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
  pt[(vaddr >> 12) & 0x3FF] = fresh | PAGE_PRESENT | PAGE_RW | PAGE_USER;
  __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
  pmm_free_frame(old);
  return true;
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
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir)); /* current address space */

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

/* Map [phys, phys+len) into the kernel master directory as supervisor RW; 0, or -1 if pool exhausted. */
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

/* Drop a range mapped by paging_kmap; pooled page tables are kept. */
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

/* Map npages of [phys..] at `vaddr` in `dir` as user pages with private PTs; 0, or -1 on exhaustion. */
int paging_map_user_at(uint32_t *dir, uint32_t vaddr, uint32_t phys, int npages)
{
  for (int p = 0; p < npages; p++) {
    uint32_t a = vaddr + (uint32_t)p * 4096;
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    uint32_t *pt;
    if (dir[pdi] & PAGE_PRESENT) {
      if (!(dir[pdi] & PAGE_USER)) /* shared kernel table (MMIO): never put user PTEs in it */
        return -1;
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

/* Undo paging_map_user_at: clear PTEs, free PDE page tables left empty; `bytes` is the vaddr span. */
void paging_unmap_user_at(uint32_t *dir, uint32_t vaddr, uint32_t bytes)
{
  uint32_t start = vaddr & ~0x3FFFFFu; /* round to PDE (4 MiB) */
  uint32_t stop = (vaddr + bytes + 0x3FFFFFu) & ~0x3FFFFFu;
  for (uint32_t base = start; base < stop; base += 0x400000u) {
    uint32_t pdi = base >> 22;
    if (!(dir[pdi] & PAGE_PRESENT) || !(dir[pdi] & PAGE_USER))
      continue;                          /* absent, or a shared kernel table this call never allocated */
    pfree((void *)(dir[pdi] & ~0xFFFu)); /* the private page table */
    dir[pdi] = 0;
  }
}

/* Heap grow: map zeroed user pages for [old_top, new_top); rolls back and returns -1 on exhaustion. */
int paging_user_grow(uint32_t *dir, uint32_t old_top, uint32_t new_top)
{
  for (uint32_t page = old_top; page < new_top; page += 4096) {
    uint32_t phys = pmm_alloc_frame();
    int ok = phys != 0;
    if (ok) {
      uint32_t fl = irq_save();
      uint32_t *kv = paging_scratch_map(0, phys);
      memset(kv, 0, 4096);
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

/* Heap shrink: unmap and free user frames for [new_top, old_top); page tables stay. */
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
