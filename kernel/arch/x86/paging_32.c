/* paging_32.c: i386 kernel address space bring-up (2-level, identity-mapped) and per-process spaces. */

#include "paging_impl.h"

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
  pmm_set_contig_hole(USER_BASE, USER_REGION_END);

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
    if (!(dir[pdi] & PAGE_PRESENT) || !(dir[pdi] & PAGE_USER))
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
  /* Another CPU already made the page writable; this CPU faulted on its stale read-only translation. */
  if ((error & 1) && (pte & PAGE_PRESENT) && (pte & PAGE_USER) && (pte & PAGE_RW)) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return true;
  }
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
  free_frames_flushed(&old, 1); /* a sibling thread elsewhere may still read the shared frame */
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
  smp_tlb_shootdown(); /* the parent's threads on other CPUs must stop writing the now-shared frames */
  return child;
}

/* Kernel identity mappings must stay out of the per-process user window: there they would be inherited into every
 * address space and freed or extended as if private. */
static bool in_user_window(uint32_t start, uint32_t stop) { return start < USER_REGION_END && stop > USER_BASE; }

int paging_identity_map(uint32_t phys, uint32_t len)
{
  uint32_t *dir;
  __asm__ volatile("mov %%cr3, %0" : "=r"(dir)); /* current address space */

  uint32_t start = phys & ~0xFFFu;
  uint32_t stop = (phys + len + 0xFFFu) & ~0xFFFu;
  if (in_user_window(start, stop))
    return -1;
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
  if (in_user_window(start, stop))
    return -1;
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
  smp_tlb_shootdown(); /* the caller frees these frames next */
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
  void *tables[UNMAP_BATCH];
  int n = 0;
  for (uint32_t base = start; base < stop; base += 0x400000u) {
    uint32_t pdi = base >> 22;
    if (!(dir[pdi] & PAGE_PRESENT) || !(dir[pdi] & PAGE_USER))
      continue; /* absent, or a shared kernel table this call never allocated */
    tables[n++] = (void *)(dir[pdi] & ~0xFFFu);
    dir[pdi] = 0;
    if (n == UNMAP_BATCH) {
      free_tables_flushed(dir, tables, n);
      n = 0;
    }
  }
  free_tables_flushed(dir, tables, n);
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
  uint32_t frames[UNMAP_BATCH];
  int n = 0;
  for (uint32_t a = new_top; a < old_top; a += 4096) {
    uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
    if (!(dir[pdi] & PAGE_PRESENT))
      continue;
    uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
    if (pt[pti] & PAGE_PRESENT) {
      frames[n++] = pt[pti] & ~0xFFFu;
      pt[pti] = 0;
      __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
      if (n == UNMAP_BATCH) {
        free_frames_flushed(frames, n);
        n = 0;
      }
    }
  }
  free_frames_flushed(frames, n);
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
