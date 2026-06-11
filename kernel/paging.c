/* paging.c — set up a page directory that identity-maps the low 16 MiB,
 * then turn paging on.
 *
 * Identity mapping (virtual == physical) means the kernel, stack, VGA buffer,
 * and these page tables all keep working the instant CR0.PG flips. We map a
 * full 16 MiB so the kernel heap (4-16 MiB) is addressable too.
 */

#include "paging.h"
#include <stdint.h>

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_PWT     0x8   /* write-through bit; selects PAT entry 1 here      */
#define PAGE_PCD     0x10  /* cache-disable bit; UC- mapping for MMIO regs     */

#define PD_ENTRIES   1024
#define PT_ENTRIES   1024
#define IDENTITY_TABLES 4              /* 4 tables * 4 MiB = 16 MiB           */

/* Page-aligned so their physical addresses are valid PDE/PTE pointers. Living
 * in .bss (below 4 MiB) they are themselves identity-mapped. */
static uint32_t page_directory[PD_ENTRIES]              __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));

void paging_init(void)
{
	for (int i = 0; i < PD_ENTRIES; i++)
		page_directory[i] = 0;         /* not present                         */

	for (int t = 0; t < IDENTITY_TABLES; t++) {
		for (int i = 0; i < PT_ENTRIES; i++) {
			uint32_t frame = ((uint32_t)t * PT_ENTRIES + i) * 4096;
			page_tables[t][i] = frame | PAGE_PRESENT | PAGE_RW;
		}
		page_directory[t] = (uint32_t)page_tables[t] | PAGE_PRESENT | PAGE_RW;
	}

	__asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

	uint32_t cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;                 /* CR0.PG                              */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

uint32_t paging_kernel_phys(void)
{
	return (uint32_t)page_directory;
}

/* --- page-frame pool over [12 MiB, 16 MiB) (identity-mapped) --------------- */
#define POOL_START 0x00C00000u
#define POOL_END   0x01000000u
#define USER_WINDOW_START 0x00280000u   /* 2.5 MiB: start of the per-process window
                                          * (kernel image + BSS now reaches ~2.1 MiB,
                                          *  so the window starts above it) */

static uint32_t pool_next = POOL_START;
static uint32_t pool_free;              /* freelist head (0 = empty)           */

void *palloc(void)
{
	uint32_t page;
	if (pool_free) {
		page = pool_free;
		pool_free = *(uint32_t *)page;     /* next link stored in the page    */
	} else if (pool_next + 4096 <= POOL_END) {
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

/* --- per-process address spaces ------------------------------------------- */
uint32_t *paging_new_process_dir(void)
{
	uint32_t *dir = palloc();
	if (!dir)
		return 0;

	/* Share every kernel/heap/pool mapping... */
	for (int i = 0; i < PD_ENTRIES; i++)
		dir[i] = page_directory[i];

	/* ...but give this process a private page table for the low 4 MiB, so its
	 * user window (2-4 MiB) is isolated while the kernel half (0-2 MiB) stays
	 * identity-mapped and supervisor-only. */
	uint32_t *t0 = palloc();
	if (!t0) {
		pfree(dir);
		return 0;
	}
	for (int i = 0; i < PT_ENTRIES; i++) {
		uint32_t vaddr = (uint32_t)i * 4096;
		if (vaddr < USER_WINDOW_START)
			t0[i] = vaddr | PAGE_PRESENT | PAGE_RW;       /* kernel, supervisor */
		/* else: user window, left not-present until mapped */
	}
	dir[0] = (uint32_t)t0 | PAGE_PRESENT | PAGE_RW | PAGE_USER;
	return dir;
}

void paging_map_user_page(uint32_t *dir, uint32_t vaddr, uint32_t frame)
{
	uint32_t *t0 = (uint32_t *)(dir[0] & ~0xFFFu);     /* the private table    */
	t0[(vaddr >> 12) & 0x3FF] =
		(frame & ~0xFFFu) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
}

/* Clear a single user-window PTE *without* freeing the frame. Used to detach a
 * shared window buffer (heap-backed, not a pool frame) from a process before
 * its dir is torn down, so paging_free_process_dir's pfree never sees a page
 * that didn't come from the pool. */
void paging_unmap_user_page(uint32_t *dir, uint32_t vaddr)
{
	uint32_t *t0 = (uint32_t *)(dir[0] & ~0xFFFu);
	t0[(vaddr >> 12) & 0x3FF] = 0;
}

void paging_free_process_dir(uint32_t *dir)
{
	uint32_t *t0 = (uint32_t *)(dir[0] & ~0xFFFu);
	for (int i = USER_WINDOW_START / 4096; i < PT_ENTRIES; i++)
		if (t0[i] & PAGE_PRESENT)
			pfree((void *)(t0[i] & ~0xFFFu));   /* free private user frames    */
	pfree(t0);
	pfree(dir);
}

/* Is `vaddr` a present, user-accessible page in the CURRENT address space?
 * Reads cr3 (so it works for threads, which carry dir=0) and walks the tables,
 * all identity-mapped. Lets the crash handler probe a frame pointer before
 * dereferencing it for a backtrace, so a corrupt one can't double-fault. */
int paging_user_mapped(uint32_t vaddr)
{
	uint32_t cr3;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
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
	uint32_t *t0 = (uint32_t *)(dir[0] & ~0xFFFu);   /* low-4-MiB table        */
	return t0[(vaddr >> 12) & 0x3FF] & ~0xFFFu;
}

uint32_t *paging_fork_dir(uint32_t *parent)
{
	uint32_t *child = paging_new_process_dir();
	if (!child)
		return 0;

	uint32_t *pt = (uint32_t *)(parent[0] & ~0xFFFu);
	uint32_t *ct = (uint32_t *)(child[0] & ~0xFFFu);
	for (int i = USER_WINDOW_START / 4096; i < PT_ENTRIES; i++) {
		if (!(pt[i] & PAGE_PRESENT))
			continue;
		uint32_t *nf = palloc();
		if (!nf) {
			paging_free_process_dir(child);
			return 0;
		}
		uint32_t *pf = (uint32_t *)(pt[i] & ~0xFFFu);   /* parent frame        */
		for (int k = 0; k < 1024; k++)
			nf[k] = pf[k];                              /* copy 4 KiB          */
		ct[i] = (uint32_t)nf | PAGE_PRESENT | PAGE_RW | PAGE_USER;
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
