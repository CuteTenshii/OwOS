/* heap.c — a first-fit free-list allocator over a fixed virtual window.
 *
 * The heap occupies [4 MiB, 16 MiB), which the identity map already covers,
 * so kmalloc never has to map pages itself yet. Blocks are a singly linked
 * list of headers; kmalloc splits a big-enough free block, kfree marks one
 * free and coalesces it with the following block.
 */

#include "heap.h"
#include <stdint.h>

#define KHEAP_START 0x00400000u        /* 4 MiB                              */
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
	head = (block_t *)KHEAP_START;
	head->size = (KHEAP_END - KHEAP_START) - sizeof(block_t);
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
	b->free = 1;

	/* Merge forward with adjacent free blocks. */
	while (b->next && b->next->free) {
		b->size += sizeof(block_t) + b->next->size;
		b->next = b->next->next;
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
