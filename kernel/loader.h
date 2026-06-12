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
#define USER_REGION_END 0x44000000u    /* +64 MiB: end of the per-process window */
#define USER_STACK_TOP  0x43FFF000u    /* user stack grows down from the top    */

/* Build a fresh address space from an ELF32 image. On success returns 0 and
 * fills *out_dir / *out_entry; on a bad image returns -1. */
int load_elf(const void *image, uint32_t size, uint32_t **out_dir, uint32_t *out_entry);

/* Lay argc/argv onto the top of a process's user stack (System V layout) and
 * return the initial user esp. */
uint32_t setup_args(uint32_t *dir, int argc, char *const argv[]);

/* Load an ELF and spawn it as a scheduled ring-3 process with the given
 * arguments. Returns its pid, or -1 on failure. */
int exec_program(const void *image, uint32_t size, int argc, char *const argv[]);

#endif /* OWOS_LOADER_H */
