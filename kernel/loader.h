/* loader.h — load a flat user binary into the user region and run it. */
#ifndef OWOS_LOADER_H
#define OWOS_LOADER_H

#include <stdint.h>

/* Each process gets a large PRIVATE virtual window at 1 GiB, backed on demand by
 * pmm high memory (>16 MiB) through its own page tables. 1 GiB is clear of the
 * identity map (low 16 MiB), the kernel heap/pool, every device MMIO mapping,
 * and the per-window framebuffer at WIN_FB_VADDR (3 GiB), so it collides with
 * nothing. The span is the per-process cap; frames are only allocated for the
 * program image + stack actually used. */
#define USER_BASE       0x40000000u    /* 1 GiB: where user code is loaded      */
#define USER_REGION_END 0x80000000u    /* 2 GiB: a ~1 GiB per-process heap window.
                                          Backed on demand, so the span is free
                                          until touched; ends well below WIN_FB_VADDR
                                          (3 GiB). Physical RAM (make run -m) is the
                                          real ceiling now, not this boundary. */
#define USER_STACK_TOP  (USER_REGION_END - 0x1000u)  /* stack grows down from the top */
#define USER_STACK_PAGES 4u            /* 16 KiB of user stack                  */
#define USER_GUARD_PAGES 1u            /* unmapped gap below the stack (overflow trap) */
/* The heap (SYS_SBRK) grows up from the image end toward — but never into — the
 * fixed user stack at the top of the window. One page below the stack is left
 * permanently unmapped as a guard: a stack overflow (or a heap grown to the very
 * top) faults on it and the process is killed cleanly, instead of the stack
 * silently writing into the top heap page. The heap ceiling stops below it. */
#define USER_HEAP_MAX   (USER_STACK_TOP - (USER_STACK_PAGES + USER_GUARD_PAGES) * 4096u)

/* Build a fresh address space from an ELF32 image. On success returns 0 and
 * fills *out_dir / *out_entry / *out_brk (the page-aligned image end, where the
 * heap begins); on a bad image returns -1. */
int load_elf(const void *image, uint32_t size, uint32_t **out_dir,
             uint32_t *out_entry, uint32_t *out_brk);

/* Lay argc/argv onto the top of a process's user stack (System V layout) and
 * return the initial user esp. */
uint32_t setup_args(uint32_t *dir, int argc, char *const argv[]);

/* Load an ELF and spawn it as a scheduled user process with the given
 * arguments. Returns its pid, or -1 on failure. */
int exec_program(const void *image, uint32_t size, int argc, char *const argv[]);

#endif /* OWOS_LOADER_H */
