/* heap.c — a first-fit free-list allocator over a fixed virtual window.
 *
 * The heap runs from just past the kernel image up to 12 MiB (12-16 MiB is the
 * page-frame pool); the identity map already covers the low 16 MiB, so kmalloc
 * never has to map pages itself yet. Blocks are a singly linked list of headers;
 * kmalloc splits a big-enough free block, kfree marks one free and coalesces it
 * with the following block.
 *
 * The start is keyed off the linker's kernel_end, NOT a fixed 4 MiB: the kernel
 * embeds every user-program ELF as a blob in .data, so the image keeps growing
 * as programs are added. Once that pushed .bss past 4 MiB, a fixed heap base
 * overlapped the image and the heap stomped low globals (fb.c's present_hook
 * among them), so the WM's first fb_present jumped through a clobbered function
 * pointer and triple-faulted. Anchoring to kernel_end makes that impossible.
 */

#include "heap.h"
#include <stdint.h>

/* Defined by linker.ld as the first byte past the kernel image (end of .bss). */
extern char kernel_end[];

#define KHEAP_END   0x00C00000u        /* 12 MiB (12-16 MiB is the frame pool) */
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

typedef struct block {
	size_t        size;            /* payload bytes, not counting the header  */
	int           free;
	struct block *next;
} block_t;

static block_t *head;

void heap_init(void)
{
	uint32_t start = ALIGN_UP((uint32_t)(uintptr_t)kernel_end, 4096u);
	head = (block_t *)start;
	head->size = (KHEAP_END - start) - sizeof(block_t);
	head->free = 1;
	head->next = 0;
}

void *kmalloc(size_t size)
{
	if (size == 0)
		return 0;
	size = ALIGN_UP(size, 8);

	for (block_t *b = head; b; b = b->next) {
		if (!b->free || b->size < size)
			continue;

		/* Split if the leftover can hold a header plus a little payload. */
		if (b->size >= size + sizeof(block_t) + 8) {
			block_t *rest = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
			rest->size = b->size - size - sizeof(block_t);
			rest->free = 1;
			rest->next = b->next;
			b->size = size;
			b->next = rest;
		}
		b->free = 0;
		return (uint8_t *)b + sizeof(block_t);
	}
	return 0;   /* no block large enough */
}

void kfree(void *ptr)
{
	if (!ptr)
		return;
	block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));

	/* Robustness: ignore a pointer outside the heap window, or a double free
	 * (either would corrupt the list / underflow the used count). */
	if ((uintptr_t)b < (uintptr_t)head || (uintptr_t)b >= KHEAP_END)
		return;
	if (b->free)
		return;
	b->free = 1;

	/* Merge forward with adjacent free blocks. */
	while (b->next && b->next->free) {
		b->size += sizeof(block_t) + b->next->size;
		b->next = b->next->next;
	}

	/* Merge backward: absorb b into a free predecessor. The list is kept in
	 * address order, so one walk from head finds the block whose next is b.
	 * Without this, freeing blocks in any order but low-to-high left a trail of
	 * small free blocks that never recombined — the heap fragmented over uptime. */
	if (b != head) {
		block_t *p = head;
		while (p && p->next != b)
			p = p->next;
		if (p && p->free) {
			p->size += sizeof(block_t) + b->size;
			p->next  = b->next;
		}
	}
}

size_t heap_used_bytes(void)
{
	size_t used = 0;
	for (block_t *b = head; b; b = b->next)
		if (!b->free)
			used += b->size + sizeof(block_t);
	return used;
}

size_t heap_free_bytes(void)
{
	size_t freed = 0;
	for (block_t *b = head; b; b = b->next)
		if (b->free)
			freed += b->size;
	return freed;
}
