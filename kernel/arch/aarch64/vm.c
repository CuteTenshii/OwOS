/* vm.c: per-process stage-1 translation tables (4 KiB pages under the kernel's 1 GiB identity blocks). */
#include "arch.h"
#include "kstring.h"

#define PTE_TABLE 3ULL
#define PTE_PAGE 3ULL
#define PTE_ATTR_N (1ULL << 2)
#define PTE_AP_EL0 (1ULL << 6)
#define PTE_SH_ISH (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_ADDR(e) ((e) & 0x0000fffffffff000ULL)
#define ENTRIES 512

static uint64_t *new_table(void)
{
  uintptr_t f = pmm_alloc_frame();
  if (!f)
    return 0;
  memset((void *)f, 0, PMM_FRAME_SIZE);
  return (uint64_t *)f;
}

uint64_t *vm_create(void)
{
  uint64_t *root = new_table();
  if (root)
    memcpy(root, kernel_l1, 4 * sizeof(uint64_t));
  return root;
}

static uint64_t *next_level(uint64_t *table, unsigned idx)
{
  if (!(table[idx] & 1)) {
    uint64_t *t = new_table();
    if (!t)
      return 0;
    table[idx] = (uint64_t)(uintptr_t)t | PTE_TABLE;
  }
  return (uint64_t *)PTE_ADDR(table[idx]);
}

int vm_map(uint64_t *root, uintptr_t va, uintptr_t pa, int prot)
{
  if (va < USER_BASE || va >= USER_END)
    return -1;
  uint64_t *l2 = next_level(root, (va >> 30) & (ENTRIES - 1));
  uint64_t *l3 = l2 ? next_level(l2, (va >> 21) & (ENTRIES - 1)) : 0;
  if (!l3)
    return -1;
  uint64_t e = pa | PTE_PAGE | PTE_ATTR_N | PTE_SH_ISH | PTE_AF | PTE_AP_EL0 | PTE_PXN;
  if (prot == VM_USER_RX)
    e |= 1ULL << 7; /* AP[2]: read-only */
  else
    e |= PTE_UXN;
  l3[(va >> 12) & (ENTRIES - 1)] = e;
  return 0;
}

void vm_switch(uint64_t *root)
{
  SYSREG_WRITE(ttbr0_el1, root);
  __asm__ volatile("isb\n\ttlbi vmalle1\n\tdsb ish\n\tisb");
}

void vm_destroy(uint64_t *root)
{
  for (unsigned i = 4; i < ENTRIES; i++) {
    if (!(root[i] & 1))
      continue;
    uint64_t *l2 = (uint64_t *)PTE_ADDR(root[i]);
    for (unsigned j = 0; j < ENTRIES; j++) {
      if (!(l2[j] & 1))
        continue;
      uint64_t *l3 = (uint64_t *)PTE_ADDR(l2[j]);
      for (unsigned k = 0; k < ENTRIES; k++)
        if (l3[k] & 1)
          pmm_free_frame(PTE_ADDR(l3[k]));
      pmm_free_frame((uintptr_t)l3);
    }
    pmm_free_frame((uintptr_t)l2);
  }
  pmm_free_frame((uintptr_t)root);
}
