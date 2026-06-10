/* pmm.c — track physical RAM one 4 KiB frame at a time with a bitmap. */

#include "pmm.h"
#include "multiboot.h"
#include "vga.h"

#define FRAME_SIZE     PMM_FRAME_SIZE
#define MAX_FRAMES     (1u << 20)      /* support up to 4 GiB                 */
#define BITMAP_DWORDS  (MAX_FRAMES / 32)

/* The low 16 MiB is identity-mapped and owned by the kernel + heap; the PMM
 * hands out only frames above it. Must match the identity map (paging.c) and
 * the heap window (heap.c). */
#define RESERVED_BYTES (16u * 1024 * 1024)
#define RESERVED_FRAMES (RESERVED_BYTES / FRAME_SIZE)

static uint32_t bitmap[BITMAP_DWORDS];
static uint32_t total_frames;
static uint32_t used_frames;

static int  frame_test(uint32_t f)  { return (bitmap[f / 32] >> (f % 32)) & 1u; }
static void frame_set(uint32_t f)   { bitmap[f / 32] |=  (1u << (f % 32)); }
static void frame_clear(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }

void pmm_init(const struct multiboot_info *mbi)
{
	/* Without a memory map we can't know what's safe; refuse to guess. */
	if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {
		vga_write("pmm: no multiboot memory map -- cannot init.\n");
		return;
	}

	/* First pass: find the highest usable address to size the bitmap. */
	uint64_t highest = 0;
	uint32_t addr = mbi->mmap_addr;
	uint32_t end  = mbi->mmap_addr + mbi->mmap_length;
	while (addr < end) {
		const struct multiboot_mmap_entry *e =
			(const struct multiboot_mmap_entry *)addr;
		if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
			uint64_t top = e->addr + e->len;
			if (top > highest)
				highest = top;
		}
		addr += e->size + 4;
	}

	total_frames = highest / FRAME_SIZE;
	if (total_frames > MAX_FRAMES)
		total_frames = MAX_FRAMES;

	/* Start with everything marked used, then free the available regions. */
	for (uint32_t i = 0; i < BITMAP_DWORDS; i++)
		bitmap[i] = 0xFFFFFFFF;
	used_frames = total_frames;

	addr = mbi->mmap_addr;
	while (addr < end) {
		const struct multiboot_mmap_entry *e =
			(const struct multiboot_mmap_entry *)addr;
		if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
			uint32_t f0 = (e->addr + FRAME_SIZE - 1) / FRAME_SIZE; /* round up   */
			uint32_t f1 = (e->addr + e->len) / FRAME_SIZE;         /* round down */
			for (uint32_t f = f0; f < f1 && f < total_frames; f++) {
				if (frame_test(f)) {
					frame_clear(f);
					used_frames--;
				}
			}
		}
		addr += e->size + 4;
	}

	/* Reserve the low 16 MiB (kernel image + identity-mapped heap). */
	for (uint32_t f = 0; f < RESERVED_FRAMES && f < total_frames; f++) {
		if (!frame_test(f)) {
			frame_set(f);
			used_frames++;
		}
	}
}

uint32_t pmm_alloc_frame(void)
{
	for (uint32_t f = 0; f < total_frames; f++) {
		if (!frame_test(f)) {
			frame_set(f);
			used_frames++;
			return f * FRAME_SIZE;
		}
	}
	return 0;   /* out of physical memory */
}

uint32_t pmm_alloc_contig(uint32_t bytes)
{
	uint32_t need = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
	if (need == 0)
		return 0;

	for (uint32_t f = 0; f + need <= total_frames; ) {
		uint32_t run = 0;
		while (run < need && !frame_test(f + run))
			run++;
		if (run == need) {                 /* found a long-enough free run     */
			for (uint32_t i = 0; i < need; i++) {
				frame_set(f + i);
				used_frames++;
			}
			return f * FRAME_SIZE;
		}
		f += run + 1;                      /* skip past the blocking used frame */
	}
	return 0;
}

void pmm_free_frame(uint32_t phys)
{
	uint32_t f = phys / FRAME_SIZE;
	if (f < total_frames && frame_test(f)) {
		frame_clear(f);
		used_frames--;
	}
}

uint32_t pmm_total_bytes(void) { return total_frames * FRAME_SIZE; }
uint32_t pmm_used_bytes(void)  { return used_frames * FRAME_SIZE; }
uint32_t pmm_free_bytes(void)  { return (total_frames - used_frames) * FRAME_SIZE; }
