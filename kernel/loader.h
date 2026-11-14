/* loader.h: load a user ELF program into the user region and run it. */
#ifndef OWOS_LOADER_H
#define OWOS_LOADER_H

#include "paging.h"
#include "uabi.h"
#include "uaccess.h"
#include <stdint.h>

/* Each process gets a large PRIVATE window [USER_BASE, USER_REGION_END) (common/uabi.h), backed on demand by pmm
 * high memory (>16 MiB) through its own page tables. It is clear of the identity map, the kernel heap/pool, every
 * device MMIO mapping and the shm slots, so it collides with nothing. The span is the per-process cap; frames are only
 * allocated for the program image + stack actually used, so physical RAM is the real ceiling. */
#define USER_STACK_TOP (USER_REGION_END - 0x1000u) /* stack grows down from the top */
#define USER_STACK_PAGES                                                                                               \
  256u                      /* 1 MiB of user stack: the recursive-descent                                              \
                                                         JS parser nests one frame-ladder per                          \
                               source nesting level (~900 B each at -O0), so a real minified bundle                    \
                               needs far more than a few KiB. Backed by pmm high memory; RAM is the                    \
                               ceiling. */
#define USER_GUARD_PAGES 1u /* unmapped gap below the stack (overflow trap) */
/* Guarded thread stacks: a fixed region just below the main stack's guard,
 * split into TSTACK_SLOTS slots. user_stack_alloc() maps a slot's upper pages
 * RW and leaves its lowest page unmapped as a guard, so a *thread* stack
 * overflow faults cleanly instead of scribbling the heap (the main stack
 * already gets this via the guard above; malloc'd thread stacks did not, and a
 * fetch-thread overflow corrupted the heap). The heap ceiling drops below this
 * region. */
#define TSTACK_SLOT_PAGES 64u /* 63 usable pages (252 KiB) + 1 guard page  */
#define TSTACK_SLOTS 32u      /* -> 8 MiB reserved below the main stack     */
#define TSTACK_TOP (USER_STACK_TOP - (USER_STACK_PAGES + USER_GUARD_PAGES) * 4096u)
#define TSTACK_LOW (TSTACK_TOP - TSTACK_SLOTS * TSTACK_SLOT_PAGES * 4096u)
/* The heap (SYS_SBRK) grows up from the image end toward, but never into, the
 * fixed user stack + guarded thread-stack region at the top of the window. */
#define USER_HEAP_MAX TSTACK_LOW

/* Map a guarded thread stack in the CALLING process's window (SYS_STACK_ALLOC):
 * `pages` mapped pages + one unmapped guard page below. Returns the stack top
 * (highest address, for thread_create) or 0. user_stack_free releases it. */
uaddr_t user_stack_alloc(int pages);
void user_stack_free(uaddr_t top);
/* Drop a process's thread-stack slot table on exit (its pages are freed with
 * the address space; this just clears the bookkeeping). */
void user_stack_reset_owner(pgdir_t *dir);

/* Above the private window (common/uabi.h): COMPOSITOR_FB_ADDR is the framebuffer back buffer that SYS_FB_MAP hands
 * the compositor, and shared memory (shm.c) maps at the start of an SHM_SLOT_SIZE slot below or above it. The teardown
 * of an address space frees those page tables but never their frames. */

/* Build a fresh address space from an ELF image of the kernel's word size. On success returns 0 and
 * fills *out_dir / *out_entry / *out_brk (the page-aligned image end, where the
 * heap begins); on a bad image returns -1. */
int load_elf(const void *image, uint32_t size, pgdir_t **out_dir, uaddr_t *out_entry, uaddr_t *out_brk);

/* Lay argc/argv onto the top of a process's user stack (System V layout) and
 * return the initial user esp. */
uaddr_t setup_args(pgdir_t *dir, int argc, char *const argv[]);

/* Load an ELF and spawn it as a scheduled user process with the given
 * arguments. Returns its pid, or -1 on failure. */
int exec_program(const void *image, uint32_t size, int argc, char *const argv[]);

/* As exec_program, but the process stays off the CPUs until sched_release(pid). */
int exec_program_held(const void *image, uint32_t size, int argc, char *const argv[]);

#endif /* OWOS_LOADER_H */
