/* uvm.h: user address spaces: the user window, per-process page tables and image loaders. */
#ifndef OWOS_ARCH_AARCH64_UVM_H
#define OWOS_ARCH_AARCH64_UVM_H

#include "pmm.h"
#include <stdint.h>

#define USER_BASE 0x100000000UL
#define USER_END 0x4000000000UL /* 256 GiB; page tables are built on demand */
#define USER_STACK_PAGES 64
#define USER_STACK_BOTTOM (USER_END - USER_STACK_PAGES * 0x1000UL)

static inline int user_range(uint64_t ptr, uint64_t len)
{
  return ptr >= USER_BASE && ptr < USER_END && len <= USER_END - ptr;
}

#define VM_USER_RX 1
#define VM_USER_RW 2
#define VM_USER_RO 3

/* The kernel reaches physical frames through its identity map; host tests override this. */
#ifndef PHYS_TO_PTR
#define PHYS_TO_PTR(phys) ((void *)(uintptr_t)(phys))
#endif

extern uint64_t kernel_l1[512];

/* Per-process translation tables: the kernel identity map plus a user window at USER_BASE. */
uint64_t *vm_create(void);
int vm_map(uint64_t *root, uintptr_t va, uintptr_t pa, int prot);
void vm_switch(uint64_t *root);
void vm_destroy(uint64_t *root);
void vm_sync_code(phys_t frame); /* make freshly written code in a frame visible to instruction fetch */

/* A ready-to-run user address space: page tables with the program and stack mapped. */
struct user_image {
  uint64_t *root;
  uint64_t entry, sp;
};

/* Build an image from a flat code blob mapped at USER_BASE, or from an ELF64 AArch64 executable.
 * Both return 0 on success and leave nothing allocated on failure. */
int user_image_code(struct user_image *img, const void *code, uint32_t len);
int elf_load(struct user_image *img, const void *file, uint64_t size);

#endif
