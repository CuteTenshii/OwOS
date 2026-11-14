/* paging.c: AArch64 address spaces behind the shared paging API (paging.h). 39-bit VAs in 4 KiB pages: L1 slots
 * 0..3 are the kernel's 1 GiB identity blocks (mmu_init), shared by every space; a process's tables sit above. No
 * ASIDs: switching spaces flushes the local TLB, and a change to a live space is broadcast in the inner shareable
 * domain, which reaches every CPU running it. */
#include "paging.h"
#include "arch.h"
#include "kstring.h"
#include "loader.h"
#include "pmm.h"

#define ENTRIES 512
#define PAGE 4096u
#define PTE_VALID 1ull
#define PTE_TABLE 3ull              /* valid table descriptor, levels 1 and 2 */
#define PTE_PAGE 3ull               /* valid page descriptor, level 3 */
#define PTE_ATTR_NORMAL (1ull << 2) /* MAIR index 1, write-back cacheable */
#define PTE_AP_EL0 (1ull << 6)
#define PTE_AP_RO (1ull << 7)
#define PTE_SH_ISH (3ull << 8)
#define PTE_AF (1ull << 10)
#define PTE_PXN (1ull << 53)
#define PTE_COW (1ull << 55) /* software bit: read-only until a write copies the frame */
#define PTE_ADDR(e) ((e) & 0x0000FFFFFFFFF000ull)
#define KERNEL_SLOTS 4
#define VA_BITS 39

#define USER_PAGE (PTE_PAGE | PTE_ATTR_NORMAL | PTE_SH_ISH | PTE_AF | PTE_AP_EL0 | PTE_PXN)

void *palloc(void)
{
  uintptr_t f = pmm_alloc_frame();
  if (!f)
    return 0;
  memset((void *)f, 0, PAGE);
  return (void *)f;
}

void pfree(void *page) { pmm_free_frame((uintptr_t)page); }

uintptr_t paging_kernel_phys(void) { return (uintptr_t)kernel_l1; }

void paging_load(pgdir_t *dir)
{
  __asm__ volatile("msr ttbr0_el1, %0\n\tisb\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" : : "r"(dir) : "memory");
}

pgdir_t *paging_current(void)
{
  pgdir_t *dir;
  __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(dir));
  return dir;
}

static void tlb_flush_page(uaddr_t va)
{
  __asm__ volatile("dsb ishst\n\ttlbi vae1is, %0\n\tdsb ish\n\tisb" : : "r"(va >> 12) : "memory");
}

static void tlb_flush_all(void) { __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory"); }

/* Instruction fetch doesn't snoop the data the kernel wrote into a frame: clean it to the point of unification and
 * drop the I-cache, so code loaded or copied there runs as written. */
static void sync_code(uintptr_t frame)
{
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uintptr_t line = 4u << ((ctr >> 16) & 0xF); /* DminLine */
  for (uintptr_t a = frame; a < frame + PAGE; a += line)
    __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

/* Install `val` at `pte` for `va`, break-before-make if a valid entry is being replaced. */
static void set_pte(uint64_t *pte, uint64_t val, uaddr_t va)
{
  if (*pte & PTE_VALID) {
    *pte = 0;
    tlb_flush_page(va);
  }
  *pte = val;
  __asm__ volatile("dsb ishst\n\tisb" ::: "memory");
}

static uint64_t *next_table(uint64_t *t, unsigned i, bool alloc)
{
  if (t[i] & PTE_VALID)
    return (uint64_t *)(uintptr_t)PTE_ADDR(t[i]);
  if (!alloc)
    return 0;
  uint64_t *n = palloc();
  if (n)
    t[i] = (uint64_t)(uintptr_t)n | PTE_TABLE;
  return n;
}

/* The level-3 entry for `va` in `root`, allocating tables if `alloc`; 0 for the kernel's slots or past the VA range. */
static uint64_t *pte_of(pgdir_t *root, uaddr_t va, bool alloc)
{
  unsigned l1 = (unsigned)(va >> 30) & (ENTRIES - 1);
  if (va >> VA_BITS || l1 < KERNEL_SLOTS)
    return 0;
  uint64_t *l2 = next_table(root, l1, alloc);
  uint64_t *l3 = l2 ? next_table(l2, (unsigned)(va >> 21) & (ENTRIES - 1), alloc) : 0;
  return l3 ? &l3[(va >> 12) & (ENTRIES - 1)] : 0;
}

static bool user_entry(const uint64_t *pte) { return pte && (*pte & PTE_VALID) && (*pte & PTE_AP_EL0); }

pgdir_t *paging_new_process_dir(void)
{
  uint64_t *root = palloc();
  if (root)
    memcpy(root, kernel_l1, KERNEL_SLOTS * sizeof *root);
  return root;
}

void paging_free_process_dir(pgdir_t *dir)
{
  user_stack_reset_owner(dir); /* its thread-stack slot table goes before the root page is reused */
  for (unsigned i = KERNEL_SLOTS; i < ENTRIES; i++) {
    if (!(dir[i] & PTE_VALID))
      continue;
    uint64_t *l2 = (uint64_t *)(uintptr_t)PTE_ADDR(dir[i]);
    for (unsigned j = 0; j < ENTRIES; j++) {
      if (!(l2[j] & PTE_VALID))
        continue;
      uint64_t *l3 = (uint64_t *)(uintptr_t)PTE_ADDR(l2[j]);
      uaddr_t base = (uaddr_t)i << 30 | (uaddr_t)j << 21;
      /* Frames of the private window are the process's own; shm (shm.c) and the framebuffer have other owners. */
      if (base >= USER_BASE && base < USER_REGION_END)
        for (unsigned k = 0; k < ENTRIES; k++)
          if (l3[k] & PTE_VALID)
            pmm_free_frame(PTE_ADDR(l3[k]));
      pfree(l3);
    }
    pfree(l2);
  }
  pfree(dir);
  tlb_flush_all();
}

bool paging_user_mapped(uaddr_t vaddr) { return user_entry(pte_of(paging_current(), vaddr, false)); }

bool paging_user_range(uaddr_t vaddr, uint32_t len, bool write)
{
  if (vaddr < USER_BASE || vaddr > USER_REGION_END || len > USER_REGION_END - vaddr)
    return false;
  for (uaddr_t a = vaddr & ~(uaddr_t)(PAGE - 1); a < vaddr + len; a += PAGE) {
    uint64_t *pte = pte_of(paging_current(), a, false);
    if (!user_entry(pte) || (write && (*pte & PTE_AP_RO) && !(*pte & PTE_COW)))
      return false;
  }
  return true;
}

uint32_t paging_user_phys(pgdir_t *dir, uaddr_t vaddr)
{
  uint64_t *pte = pte_of(dir, vaddr, false);
  return pte && (*pte & PTE_VALID) ? (uint32_t)PTE_ADDR(*pte) : 0;
}

int paging_map_user_at(pgdir_t *dir, uaddr_t vaddr, uint32_t phys, int npages)
{
  for (int p = 0; p < npages; p++) {
    uaddr_t va = vaddr + (uaddr_t)p * PAGE;
    uint64_t *pte = pte_of(dir, va, true);
    if (!pte)
      return -1;
    uintptr_t frame = (uintptr_t)phys + (uintptr_t)p * PAGE;
    sync_code(frame);
    set_pte(pte, frame | USER_PAGE, va);
  }
  return 0;
}

void paging_unmap_user_at(pgdir_t *dir, uaddr_t vaddr, uint32_t bytes)
{
  /* Whole 2 MiB tables go, frames kept: their owner (shm.c) frees them. */
  uaddr_t start = vaddr & ~(uaddr_t)0x1FFFFF, stop = (vaddr + bytes + 0x1FFFFF) & ~(uaddr_t)0x1FFFFF;
  for (uaddr_t base = start; base < stop; base += 0x200000) {
    unsigned l1 = (unsigned)(base >> 30) & (ENTRIES - 1);
    if (base >> VA_BITS || l1 < KERNEL_SLOTS || !(dir[l1] & PTE_VALID))
      continue;
    uint64_t *l2 = (uint64_t *)(uintptr_t)PTE_ADDR(dir[l1]);
    uint64_t *slot = &l2[(base >> 21) & (ENTRIES - 1)];
    if (!(*slot & PTE_VALID))
      continue;
    void *l3 = (void *)(uintptr_t)PTE_ADDR(*slot);
    *slot = 0;
    tlb_flush_all();
    pfree(l3);
  }
}

bool paging_handle_page_fault(uaddr_t vaddr, uint32_t error)
{
  if (!(error & 2) || vaddr < USER_BASE || vaddr >= USER_REGION_END)
    return false;
  uint64_t *pte = pte_of(paging_current(), vaddr, false);
  /* Another CPU already made the page writable; this one faulted on its stale read-only translation. */
  if (user_entry(pte) && (error & 1) && !(*pte & PTE_AP_RO)) {
    tlb_flush_page(vaddr);
    return true;
  }
  if (!user_entry(pte) || !(*pte & PTE_COW))
    return false;
  uaddr_t page = vaddr & ~(uaddr_t)(PAGE - 1);
  uintptr_t old = PTE_ADDR(*pte);
  if (pmm_frame_refs(old) <= 1) { /* the other sharer is gone: just take the page back */
    set_pte(pte, (*pte & ~(PTE_AP_RO | PTE_COW)), page);
    return true;
  }
  uintptr_t fresh = pmm_alloc_frame();
  if (!fresh)
    return false;
  memcpy((void *)fresh, (const void *)old, PAGE);
  sync_code(fresh);
  set_pte(pte, fresh | USER_PAGE, page); /* flushes the old translation before the shared frame is let go */
  pmm_free_frame(old);
  return true;
}

pgdir_t *paging_fork_dir(pgdir_t *parent)
{
  pgdir_t *child = paging_new_process_dir();
  if (!child)
    return 0;
  for (uaddr_t base = USER_BASE; base < USER_REGION_END; base += 0x200000) {
    unsigned l1 = (unsigned)(base >> 30) & (ENTRIES - 1);
    if (!(parent[l1] & PTE_VALID))
      continue;
    uint64_t *l2 = (uint64_t *)(uintptr_t)PTE_ADDR(parent[l1]);
    uint64_t l2e = l2[(base >> 21) & (ENTRIES - 1)];
    if (!(l2e & PTE_VALID))
      continue;
    uint64_t *l3 = (uint64_t *)(uintptr_t)PTE_ADDR(l2e);
    for (unsigned k = 0; k < ENTRIES; k++) {
      if (!(l3[k] & PTE_VALID))
        continue;
      uaddr_t va = base + (uaddr_t)k * PAGE;
      uint64_t *cpte = pte_of(child, va, true);
      if (!cpte) {
        paging_free_process_dir(child);
        return 0;
      }
      /* Both sides read-only and copy-on-write, sharing the frame. */
      l3[k] |= PTE_AP_RO | PTE_COW;
      *cpte = l3[k];
      pmm_retain_frame(PTE_ADDR(l3[k]));
    }
  }
  tlb_flush_all(); /* the parent's threads on other CPUs must stop writing the now-shared frames */
  return child;
}

int paging_user_grow(pgdir_t *dir, uaddr_t old_top, uaddr_t new_top)
{
  for (uaddr_t page = old_top; page < new_top; page += PAGE) {
    uintptr_t phys = pmm_alloc_frame();
    if (phys)
      memset((void *)phys, 0, PAGE);
    if (!phys || paging_map_user_at(dir, page, (uint32_t)phys, 1) < 0) {
      if (phys)
        pmm_free_frame(phys);
      paging_user_shrink(dir, old_top, page); /* undo this call */
      return -1;
    }
  }
  return 0;
}

void paging_user_shrink(pgdir_t *dir, uaddr_t new_top, uaddr_t old_top)
{
  for (uaddr_t a = new_top; a < old_top; a += PAGE) {
    uint64_t *pte = pte_of(dir, a, false);
    if (pte && (*pte & PTE_VALID)) {
      uintptr_t frame = PTE_ADDR(*pte);
      *pte = 0;
      tlb_flush_page(a); /* a sibling thread elsewhere may still hold the translation */
      pmm_free_frame(frame);
    }
  }
}

/* Physical memory below 4 GiB is identity-mapped, which covers every frame the pmm hands out. */
void *paging_scratch_map(int slot, uint32_t phys)
{
  (void)slot;
  return (void *)(uintptr_t)phys;
}

void paging_scratch_unmap(int slot) { (void)slot; }

int paging_identity_map(uint32_t phys, uint32_t len) { return (uint64_t)phys + len <= (1ull << 32) ? 0 : -1; }

int paging_kmap(uint32_t phys, uint32_t len) { return paging_identity_map(phys, len); }

void paging_kunmap(uint32_t phys, uint32_t len)
{
  (void)phys;
  (void)len;
}
