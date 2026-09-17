/* heap.c — a first-fit free-list allocator over a fixed virtual window.
 *
 * The heap runs from just past the kernel image up to 12 MiB (12-16 MiB is the
 * page-frame pool); the identity map already covers the low 16 MiB, so kmalloc
 * never has to map pages itself yet. Blocks are a singly linked list of
 * headers; kmalloc splits a big-enough free block, kfree marks one free and
 * coalesces it with the following block.
 *
 * The start is keyed off the linker's kernel_end, NOT a fixed 4 MiB: the kernel
 * embeds every user-program ELF as a blob in .data, so the image keeps growing
 * as programs are added. Once that pushed .bss past 4 MiB, a fixed heap base
 * overlapped the image and the heap stomped low globals (fb.c's present_hook
 * among them), so the WM's first fb_present jumped through a clobbered function
 * pointer and triple-faulted. Anchoring to kernel_end makes that impossible.
 */

#include "heap.h"
#include "console.h"
#include "pmm.h"
#include "spinlock.h"
#include <stdint.h>

/* Defined by linker.ld as the first byte past the kernel image (end of .bss).
 */
extern char kernel_end[];

/* Heap end = pmm_pool_start() (the page-table pool starts there). Both ends are
 * RAM-scaled at boot (see pmm.c): on a 256 MiB box this is the legacy 12 MiB,
 * on bigger machines it's far higher, so kmalloc — and the per-task 32 KiB
 * kernel stacks it backs — scale with RAM. heap_init runs after pmm_init, so
 * the value is ready. */
#define KHEAP_END (pmm_pool_start())
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define BLOCK_MAGIC 0x48454150u

typedef struct block {
  size_t size; /* payload bytes, not counting the header  */
  uint32_t magic;
  int free;
  struct block *prev;
  struct block *next;
} block_t;

static block_t *head;
static block_t *alloc_cursor;
static spinlock_t heap_lock;

void heap_init(void)
{
  uint32_t start = ALIGN_UP((uint32_t)(uintptr_t)kernel_end, 4096u);
  /* The kernel image grows as user-program blobs are embedded; if it ever
   * reaches the page-table pool there is no room for a heap. Halt loudly rather
   * than let the unsigned subtraction below wrap to a ~4 GiB first block that
   * would hand out kernel/pool memory as kmalloc payloads. */
  if (start >= KHEAP_END || (KHEAP_END - start) <= sizeof(block_t) + 8) {
    con_write("heap: no room between kernel image and page-table pool -- halting.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  head = (block_t *)(uintptr_t)start;
  head->size = (KHEAP_END - start) - sizeof(block_t);
  head->magic = BLOCK_MAGIC;
  head->free = 1;
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

      /* Split if the leftover can hold a header plus a little payload. */
      if (b->size >= size + sizeof(block_t) + 8) {
        block_t *rest = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
        rest->size = b->size - size - sizeof(block_t);
        rest->magic = BLOCK_MAGIC;
        rest->free = 1;
        rest->prev = b;
        rest->next = b->next;
        if (rest->next)
          rest->next->prev = rest;
        b->size = size;
        b->next = rest;
      }
      b->free = 0;
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

  /* Robustness: ignore a pointer outside the heap window, or a double free
   * (either would corrupt the list / underflow the used count). */
  if (!head || ((uintptr_t)ptr & 7) != 0 || (uintptr_t)b < (uintptr_t)head || (uintptr_t)b >= KHEAP_END ||
      b->magic != BLOCK_MAGIC) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  if (b->free) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  b->free = 1;

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
