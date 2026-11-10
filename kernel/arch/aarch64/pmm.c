/* pmm.c: AArch64 physical frame allocator over the RAM range reported by the DTB. */
#include "arch.h"
#include "kstring.h"

extern char __kernel_end[];

struct free_frame {
  struct free_frame *next;
};

static uintptr_t next_unused, ram_end;
static struct free_frame *free_list;
static uint64_t free_frames, total_frames;

void pmm_init(uint64_t ram_base, uint64_t ram_size)
{
  next_unused = ((uintptr_t)__kernel_end + PMM_FRAME_SIZE - 1) & ~(uintptr_t)(PMM_FRAME_SIZE - 1);
  ram_end = (uintptr_t)(ram_base + ram_size) & ~(uintptr_t)(PMM_FRAME_SIZE - 1);
  total_frames = free_frames = (ram_end - next_unused) / PMM_FRAME_SIZE;
}

uintptr_t pmm_alloc_frame(void)
{
  uintptr_t frame;
  if (free_list) {
    frame = (uintptr_t)free_list;
    free_list = free_list->next;
  } else if (next_unused < ram_end) {
    frame = next_unused;
    next_unused += PMM_FRAME_SIZE;
  } else {
    return 0;
  }
  free_frames--;
  return frame;
}

void pmm_free_frame(uintptr_t frame)
{
  struct free_frame *f = (struct free_frame *)frame;
  f->next = free_list;
  free_list = f;
  free_frames++;
}

uint64_t pmm_free_count(void) { return free_frames; }
uint64_t pmm_total_count(void) { return total_frames; }
