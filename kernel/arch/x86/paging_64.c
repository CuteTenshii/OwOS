/* paging_64.c: x86_64 kernel address space bring-up (4-level, identity-mapped) and per-process spaces. */

#include "paging_impl.h"

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
  pmm_set_contig_hole(USER_BASE, USER_REGION_END);

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
  extern void user_stack_reset_owner(uint32_t *dir);
  user_stack_reset_owner(dir); /* free its thread-stack slot table before the PML4 page is reused */
  uint64_t *pml4 = (uint64_t *)(void *)dir;
  uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & PG_ADDR);

  /* User window [1,2GiB): free every present frame, then its page tables. */
  if ((pdpt[1] & PAGE_PRESENT) && (pdpt[1] & PAGE_USER)) {
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

/* Kernel identity mappings must stay out of the per-process user window: there they would be inherited into every
 * address space and freed or extended as if private. */
static bool in_user_window(uint32_t start, uint32_t stop) { return start < USER_REGION_END && stop > USER_BASE; }

int paging_identity_map(uint32_t phys, uint32_t len)
{
  uint64_t *pml4 = current_pml4();
  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  if (in_user_window(start, stop))
    return -1;
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
  if (in_user_window(start, stop))
    return -1;
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
