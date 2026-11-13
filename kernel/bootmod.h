/* bootmod.h: files the bootloader loaded next to the kernel (multiboot modules). The ISO's GRUB config loads the
 * kernel image itself as "owos-kernel", so the Installer can copy it onto a disk. */
#ifndef OWOS_BOOTMOD_H
#define OWOS_BOOTMOD_H

#include <stdint.h>

/* Record a module while parsing the boot information; `cmdline` is copied. GRUB passes the module's arguments
 * there, QEMU's loader the file name followed by them. */
void bootmod_add(uint32_t start, uint32_t end, const char *cmdline);

/* Keep the physical allocator off every module. Call right after pmm_init. */
void bootmod_reserve(void);

/* Move modules out of the low region before the kernel heap claims it, and map the rest. Call after paging_init
 * and before heap_init. */
void bootmod_relocate(void);

/* The module whose command line has `name` as one of its words, or NULL; *len gets its size. */
const void *bootmod_find(const char *name, uint32_t *len);

#endif /* OWOS_BOOTMOD_H */
