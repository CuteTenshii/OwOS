/* loader.h — load a flat user binary into the user region and run it. */
#ifndef OWOS_LOADER_H
#define OWOS_LOADER_H

#include <stdint.h>

/* Each process gets a large PRIVATE virtual window at 1 GiB, backed on demand
 * by pmm high memory (>16 MiB) through its own page tables. 1 GiB is clear of
 * the identity map (low 16 MiB), the kernel heap/pool, every device MMIO
 * mapping, and the per-window framebuffer at WIN_FB_VADDR (3 GiB), so it
 * collides with nothing. The span is the per-process cap; frames are only
 * allocated for the program image + stack actually used. */
#define USER_BASE 0x40000000u /* 1 GiB: where user code is loaded      */
#define USER_REGION_END                                                                                                \
  0x80000000u                                      /* 2 GiB: a ~1 GiB per-process heap window.                         \
                                                                         Backed on demand, so the span is free         \
                                                                         until touched; ends well below                \
                                                      WIN_FB_VADDR (3 GiB). Physical RAM (make run -m) is the real     \
                                                      ceiling now, not this boundary. */
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
 * already gets this via the guard above; malloc'd thread stacks did not — a
 * fetch-thread overflow corrupted the heap). The heap ceiling drops below this
 * region. */
#define TSTACK_SLOT_PAGES 64u /* 63 usable pages (252 KiB) + 1 guard page  */
#define TSTACK_SLOTS 32u      /* -> 8 MiB reserved below the main stack     */
#define TSTACK_TOP (USER_STACK_TOP - (USER_STACK_PAGES + USER_GUARD_PAGES) * 4096u)
#define TSTACK_LOW (TSTACK_TOP - TSTACK_SLOTS * TSTACK_SLOT_PAGES * 4096u)
/* The heap (SYS_SBRK) grows up from the image end toward — but never into — the
 * fixed user stack + guarded thread-stack region at the top of the window. */
#define USER_HEAP_MAX TSTACK_LOW

/* Map a guarded thread stack in the CALLING process's window (SYS_STACK_ALLOC):
 * `pages` mapped pages + one unmapped guard page below. Returns the stack top
 * (highest address, for thread_create) or 0. user_stack_free releases it. */
uint32_t user_stack_alloc(int pages);
void user_stack_free(uint32_t top);
/* Drop a process's thread-stack slot table on exit (its pages are freed with
 * the address space; this just clears the bookkeeping). */
void user_stack_reset_owner(uint32_t *dir);

/* Fixed high VAs for the user-space compositor (Phase 1 of moving the WM to
 * ring 3). Only /bin/compositor (root) maps these; they sit ABOVE WIN_FB_VADDR
 * (0xC0000000, an app's own window buffer) so nothing collides with the
 * ordinary per-process windows the compositor also brokers. COMPOSITOR_FB_VADDR
 * the shared cached-RAM back buffer (SYS_FB_MAP); the compositor draws here,
 * then SYS_FB_PRESENT flushes it. WIN_MAP_BASE         base of one fixed slot
 * per window id (SYS_WIN_MAP), so a window's client buffer maps at WIN_MAP_BASE
 * + id*slot and the compositor blits it zero-copy. A slot holds a full- screen
 * buffer; WIN_MAP_MAX slots span up to the 4 GiB top. */
#define COMPOSITOR_FB_VADDR 0xD0000000u /* 3.25 GiB: shared framebuffer back buffer */
#define WIN_MAP_BASE 0xE0000000u        /* 3.5 GiB: window-buffer map slots */
#define WIN_MAP_SLOT_BYTES 0x00800000u  /* 8 MiB per slot (>= 1920x1080x4)          */
#define WIN_MAP_MAX 64u                 /* slots -> 0xE0000000..0x100000000 (4 GiB) */

/* Shared memory (shm.c) maps only at the start of an 8 MiB slot inside [SHM_LOW_BASE, SHM_LOW_END) or
 * [SHM_HIGH_BASE, 4 GiB); a slot spans whole page tables on both arches, so unmapping one never touches a
 * neighbour, and the teardown of an address space frees those tables but never their frames. */
#define SHM_SLOT_BYTES 0x00800000u
#define SHM_LOW_BASE 0xC0000000u
#define SHM_LOW_END COMPOSITOR_FB_VADDR
#define SHM_HIGH_BASE 0xE0000000u

/* Build a fresh address space from an ELF32 image. On success returns 0 and
 * fills *out_dir / *out_entry / *out_brk (the page-aligned image end, where the
 * heap begins); on a bad image returns -1. */
int load_elf(const void *image, uint32_t size, uint32_t **out_dir, uint32_t *out_entry, uint32_t *out_brk);

/* Lay argc/argv onto the top of a process's user stack (System V layout) and
 * return the initial user esp. */
uint32_t setup_args(uint32_t *dir, int argc, char *const argv[]);

/* Load an ELF and spawn it as a scheduled user process with the given
 * arguments. Returns its pid, or -1 on failure. */
int exec_program(const void *image, uint32_t size, int argc, char *const argv[]);

/* As exec_program, but the process stays off the CPUs until sched_release(pid). */
int exec_program_held(const void *image, uint32_t size, int argc, char *const argv[]);

#endif /* OWOS_LOADER_H */
