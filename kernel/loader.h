/* loader.h — load a flat user binary into the user region and run it. */
#ifndef OWOS_LOADER_H
#define OWOS_LOADER_H

#include <stdint.h>

#define USER_BASE       0x00280000u    /* 2.5 MiB: where user code is loaded  */
#define USER_REGION_END 0x00400000u    /* 4 MiB: end of the user window       */
#define USER_STACK_TOP  0x003FF000u    /* user stack grows down from here     */

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
