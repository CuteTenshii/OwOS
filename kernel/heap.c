/* heap.c: first-fit free-list allocator from kernel_end up to the page-frame pool. */

#include "heap.h"
#include "console.h"
#include "pmm.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stdint.h>

/* First byte past the kernel image (linker.ld). */
extern char kernel_end[];

/* Heap end is pmm_pool_start(); set after pmm_init. */
#define KHEAP_END (pmm_pool_start())
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define BLOCK_MAGIC 0x48454150u

typedef struct block {
  size_t size; /* payload bytes, not counting the header  */
  uint32_t magic;
  bool free;
  struct block *prev;
  struct block *next;
} __attribute__((aligned(8))) block_t;

_Static_assert(sizeof(block_t) % 8 == 0, "payloads must stay 8-byte aligned, kfree rejects the rest");

static block_t *head;
static block_t *alloc_cursor;
static spinlock_t heap_lock;

void heap_init(void)
{
  uint32_t start = ALIGN_UP((uint32_t)(uintptr_t)kernel_end, 4096u);
  /* No room for a heap: halt rather than wrap the size subtraction below. */
  if (start >= KHEAP_END || (KHEAP_END - start) <= sizeof(block_t) + 8) {
    con_write("heap: no room between kernel image and page-table pool -- halting.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  head = (block_t *)(uintptr_t)start;
  head->size = (KHEAP_END - start) - sizeof(block_t);
  head->magic = BLOCK_MAGIC;
  head->free = true;
  head->prev = 0;
  head->next = 0;
  alloc_cursor = head;
}

void *kmalloc(size_t size)
{
  uintptr_t flags = spin_lock_irqsave(&heap_lock);
  if (size == 0) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return 0;
  }
  size = ALIGN_UP(size, 8);

  block_t *start = alloc_cursor ? alloc_cursor : head;
  for (int pass = 0; pass < 2; pass++) {
    for (block_t *b = start; b; b = b->next) {
      if (!b->free || b->size < size)
        continue;

      /* Split if the leftover fits a header plus a little payload. */
      if (b->size >= size + sizeof(block_t) + 8) {
        block_t *rest = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
        rest->size = b->size - size - sizeof(block_t);
        rest->magic = BLOCK_MAGIC;
        rest->free = true;
        rest->prev = b;
        rest->next = b->next;
        if (rest->next)
          rest->next->prev = rest;
        b->size = size;
        b->next = rest;
      }
      b->free = false;
      alloc_cursor = b->next ? b->next : head;
      void *result = (uint8_t *)b + sizeof(block_t);
      spin_unlock_irqrestore(&heap_lock, flags);
      return result;
    }
    if (start == head)
      break;
    start = head;
  }
  spin_unlock_irqrestore(&heap_lock, flags);
  return 0; /* no block large enough */
}

void kfree(void *ptr)
{
  uintptr_t flags;
  if (!ptr)
    return;
  flags = spin_lock_irqsave(&heap_lock);
  block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));

  /* Ignore pointers outside the heap and double frees. */
  if (!head || ((uintptr_t)ptr & 7) != 0 || (uintptr_t)b < (uintptr_t)head || (uintptr_t)b >= KHEAP_END ||
      b->magic != BLOCK_MAGIC) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  if (b->free) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  b->free = true;

  /* Merge forward with adjacent free blocks. */
  while (b->next && b->next->free) {
    if (alloc_cursor == b->next)
      alloc_cursor = b;
    b->size += sizeof(block_t) + b->next->size;
    b->next = b->next->next;
    if (b->next)
      b->next->prev = b;
  }

  /* Merge backward without walking from the head. */
  if (b->prev && b->prev->free) {
    block_t *p = b->prev;
    if (alloc_cursor == b)
      alloc_cursor = p;
    p->size += sizeof(block_t) + b->size;
    p->next = b->next;
    if (p->next)
      p->next->prev = p;
  }
  spin_unlock_irqrestore(&heap_lock, flags);
}

size_t heap_used_bytes(void)
{
  uintptr_t flags = spin_lock_irqsave(&heap_lock);
  size_t used = 0;
  for (block_t *b = head; b; b = b->next)
    if (!b->free)
      used += b->size + sizeof(block_t);
  spin_unlock_irqrestore(&heap_lock, flags);
  return used;
}

size_t heap_free_bytes(void)
{
  uintptr_t flags = spin_lock_irqsave(&heap_lock);
  size_t freed = 0;
  for (block_t *b = head; b; b = b->next)
    if (b->free)
      freed += b->size;
  spin_unlock_irqrestore(&heap_lock, flags);
  return freed;
}
