/* paging.c — set up a page directory that identity-maps the low 16 MiB,
 * then turn paging on.
 *
 * Identity mapping (virtual == physical) means the kernel, stack, VGA buffer,
 * and these page tables all keep working the instant CR0.PG flips. We map a
 * full 16 MiB so the kernel heap (4-16 MiB) is addressable too.
 */

#include "paging.h"
#include "creg.h"
#include "irqflags.h"
#include "pmm.h"
#include "loader.h"     /* USER_BASE / USER_REGION_END: the per-process window  */
#include <stdint.h>

/* Page-directory index range covering the user window [USER_BASE, USER_REGION_END).
 * Each spanned PDE holds a private user page table; nothing in the shared kernel
 * directory maps here, so a present PDE in this range is always process-private. */
#define USER_PDI_LO (USER_BASE >> 22)
#define USER_PDI_HI (USER_REGION_END >> 22)

/* Bracket a scratch-mapping interval with IF off (restoring the prior flag), so
 * a fork copying through the shared scratch slots can't be preempted by another
 * scratch user. (irq_save/irq_restore live in irqflags.h.) */

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_PWT     0x8   /* write-through bit; selects PAT entry 1 here      */
#define PAGE_PCD     0x10  /* cache-disable bit; UC- mapping for MMIO regs     */

#define PD_ENTRIES   1024
#define PT_ENTRIES   1024
/* Max identity tables: 48 * 4 MiB = 192 MiB, the cap on the RAM-scaled low region
 * (pmm_low_end()). paging_init maps only as many as that region needs; the rest
 * sit unused in .bss (192 KiB). The region must stay below SCRATCH_VADDR. */
#define IDENTITY_TABLES 48

/* Page-aligned so their physical addresses are valid PDE/PTE pointers. Living
 * in .bss (below ~4 MiB) they are themselves identity-mapped. */
static uint32_t page_directory[PD_ENTRIES]              __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));

/* Scratch mapping window: one page table, installed into the shared kernel
 * directory at boot, covering SCRATCH_VADDR.. so the kernel can transiently map
 * any pmm frame to write it. SCRATCH_VADDR (256 MiB) sits above the 16 MiB
 * identity map and below WIN_FB_VADDR/MMIO, so it collides with nothing. The
 * page table itself lives in low .bss (identity-mapped, so its physaddr==addr). */
#define SCRATCH_VADDR 0x10000000u
static uint32_t scratch_pt[PT_ENTRIES] __attribute__((aligned(4096)));

/* Page-table pool cursor (primed in paging_init from pmm_pool_start). */
static uint32_t pool_next;              /* next unused page (0 until primed)    */
static uint32_t pool_free;              /* freelist head (0 = empty)           */

void paging_init(void)
{
	for (int i = 0; i < PD_ENTRIES; i++)
		page_directory[i] = 0;         /* not present                         */

	/* Identity-map [0, pmm_low_end()) — the RAM-scaled kernel/heap/pool region.
	 * One table per 4 MiB, up to IDENTITY_TABLES. */
	int ntables = (int)(pmm_low_end() >> 22);
	if (ntables < 1) ntables = 4;             /* before pmm_init: legacy 16 MiB */
	if (ntables > IDENTITY_TABLES) ntables = IDENTITY_TABLES;
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

	pool_next = pmm_pool_start();       /* page-table pool: [pool_start, low_end) */

	__asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

	uintptr_t cr0 = read_cr0();
	cr0 |= 0x80000000;                 /* CR0.PG                              */
	write_cr0(cr0);
}

uint32_t paging_kernel_phys(void)
{
	return (uint32_t)page_directory;
}

/* --- page-frame pool over [pmm_pool_start(), pmm_low_end()) (identity-mapped) -
 * RAM-scaled (see pmm.c); on a 256 MiB box that is the legacy [12 MiB, 16 MiB).
 * User program/stack frames come from pmm high memory; the pool backs only page
 * directories + page tables (a few frames per process), so it scales with how
 * many processes can exist. pool_next is primed in paging_init (after pmm). */

void *palloc(void)
{
	uint32_t page;
	if (pool_free) {
		page = pool_free;
		pool_free = *(uint32_t *)page;     /* next link stored in the page    */
	} else if (pool_next + 4096 <= pmm_low_end()) {
		page = pool_next;
		pool_next += 4096;
	} else {
		return 0;                          /* pool exhausted                  */
	}
	for (int i = 0; i < 1024; i++)
		((uint32_t *)page)[i] = 0;         /* hand out zeroed frames          */
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
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
	return (void *)va;
}

void paging_scratch_unmap(int slot)
{
	if (slot < 0 || slot >= PAGING_SCRATCH_SLOTS)
		return;
	uint32_t va = SCRATCH_VADDR + (uint32_t)slot * 4096;
	scratch_pt[(va >> 12) & 0x3FF] = 0;
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
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

uint32_t paging_user_phys(uint32_t *dir, uint32_t vaddr)
{
	uint32_t pde = dir[vaddr >> 22];
	if (!(pde & PAGE_PRESENT))
		return 0;
	uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
	uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
	return (pte & PAGE_PRESENT) ? (pte & ~0xFFFu) : 0;
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
			uint32_t cf = pmm_alloc_frame();
			if (!cf) {
				paging_free_process_dir(child);
				return 0;
			}
			/* Both frames live above the identity map; copy the 4 KiB through
			 * the supervisor scratch slots, IF off so they aren't stolen. */
			uint32_t fl = irq_save();
			uint32_t *pv = paging_scratch_map(0, ppt[i] & ~0xFFFu);
			uint32_t *cv = paging_scratch_map(1, cf);
			for (int k = 0; k < 1024; k++)
				cv[k] = pv[k];
			paging_scratch_unmap(1);
			paging_scratch_unmap(0);
			irq_restore(fl);
			if (paging_map_user_at(child, vaddr, cf, 1) < 0) {
				pmm_free_frame(cf);
				paging_free_process_dir(child);
				return 0;
			}
		}
	}
	return child;
}

int paging_identity_map(uint32_t phys, uint32_t len)
{
	uint32_t *dir;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(dir));   /* current address space  */

	uint32_t start = phys & ~0xFFFu;
	uint32_t stop  = (phys + len + 0xFFFu) & ~0xFFFu;
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
		__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
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
	uint32_t stop  = (phys + len + 0xFFFu) & ~0xFFFu;
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
		pt[pti] = a | PAGE_PRESENT | PAGE_RW;          /* supervisor: kernel only */
		__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
	}
	return 0;
}

/* Drop a range previously mapped by paging_kmap (clears the PTEs; the pooled
 * page tables stay, ready for the next buffer that lands in the same PDE). */
void paging_kunmap(uint32_t phys, uint32_t len)
{
	uint32_t start = phys & ~0xFFFu;
	uint32_t stop  = (phys + len + 0xFFFu) & ~0xFFFu;
	for (uint32_t a = start; a < stop && a >= start; a += 4096) {
		uint32_t pdi = a >> 22, pti = (a >> 12) & 0x3FF;
		if (!(page_directory[pdi] & PAGE_PRESENT))
			continue;
		uint32_t *pt = (uint32_t *)(page_directory[pdi] & ~0xFFFu);
		pt[pti] = 0;
		__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
	}
}

/* Map npages of [phys..] at `vaddr` in `dir` as user pages, allocating a private
 * USER page table for each spanned page-directory entry. Unlike
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
		pt[pti] = ((phys + (uint32_t)p * 4096) & ~0xFFFu)
		        | PAGE_PRESENT | PAGE_RW | PAGE_USER;
	}
	return 0;
}

/* Tear down a high-vaddr user mapping made by paging_map_user_at: clear the PTEs
 * and free a spanned PDE's page table once it holds no more mappings. `bytes` is
 * the buffer's reserved vaddr span (so every PDE it could touch is reclaimed). */
void paging_unmap_user_at(uint32_t *dir, uint32_t vaddr, uint32_t bytes)
{
	uint32_t start = vaddr & ~0x3FFFFFu;               /* round to PDE (4 MiB)    */
	uint32_t stop  = (vaddr + bytes + 0x3FFFFFu) & ~0x3FFFFFu;
	for (uint32_t base = start; base < stop; base += 0x400000u) {
		uint32_t pdi = base >> 22;
		if (!(dir[pdi] & PAGE_PRESENT))
			continue;
		pfree((void *)(dir[pdi] & ~0xFFFu));           /* the private page table  */
		dir[pdi] = 0;
	}
}

/* Heap grow: map fresh, zeroed user pages for [old_top, new_top) (both page-
 * aligned) into the process window `dir`. New frames are zero-filled through the
 * supervisor scratch slot so no prior tenant's bytes leak to ring 3, and each is
 * invlpg'd in case a not-present translation was cached. Returns 0, or -1 on pmm/
 * pool exhaustion after rolling back the pages this call had mapped. Backs the
 * user-space heap (SYS_SBRK); the frames are reclaimed at process exit by
 * paging_free_process_dir, which walks every present window PTE. */
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
			paging_user_shrink(dir, old_top, page);   /* undo this call */
			return -1;
		}
		__asm__ volatile ("invlpg (%0)" : : "r"(page) : "memory");
	}
	return 0;
}

/* Heap shrink: unmap and free the user data frames for [new_top, old_top) (both
 * page-aligned). Page tables are left in place (they get reused as the heap grows
 * again and are reclaimed wholesale at process exit). */
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
			__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
		}
	}
}

void paging_mark_wc(uint32_t virt, uint32_t len)
{
	uint32_t *dir;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(dir));

	uint32_t start = virt & ~0xFFFu;
	uint32_t stop  = (virt + len + 0xFFFu) & ~0xFFFu;
	for (uint32_t a = start; a < stop && a >= start; a += 4096) {
		uint32_t pdi = a >> 22;
		uint32_t pti = (a >> 12) & 0x3FF;
		if (!(dir[pdi] & PAGE_PRESENT))
			continue;
		uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
		if (pt[pti] & PAGE_PRESENT) {
			pt[pti] |= PAGE_PWT;
			__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
		}
	}
}

void paging_mark_uc(uint32_t virt, uint32_t len)
{
	uint32_t *dir;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(dir));

	uint32_t start = virt & ~0xFFFu;
	uint32_t stop  = (virt + len + 0xFFFu) & ~0xFFFu;
	for (uint32_t a = start; a < stop && a >= start; a += 4096) {
		uint32_t pdi = a >> 22;
		uint32_t pti = (a >> 12) & 0x3FF;
		if (!(dir[pdi] & PAGE_PRESENT))
			continue;
		uint32_t *pt = (uint32_t *)(dir[pdi] & ~0xFFFu);
		if (pt[pti] & PAGE_PRESENT) {
			pt[pti] |= PAGE_PCD;
			__asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory");
		}
	}
}
