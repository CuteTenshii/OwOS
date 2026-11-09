/* gdt.h — Global Descriptor Table setup.
 *
 * We replace the bootloader's GDT with our own flat layout: every segment
 * spans the full 4 GiB at base 0, so segmentation effectively does nothing
 * and we address memory linearly (paging will do the real work later).
 */
#ifndef OWOS_GDT_H
#define OWOS_GDT_H

#include <stdint.h>

/* Segment selectors = byte offset of the descriptor into the GDT.
 * These must match the order entries are filled in gdt_init(), and the
 * hardcoded values in gdt_flush.S. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE 0x18 /* use with RPL 3 -> 0x1B                      */
#define GDT_USER_DATA 0x20 /* use with RPL 3 -> 0x23                      */
#define GDT_TSS 0x28

void gdt_init(void);

/* Make the calling CPU's user TLS segment start at `base` and load it into %gs (0: the flat user data
 * segment). The scheduler calls this on every switch, as the segment is per CPU. */
void gdt_set_tls(uint32_t base);

/* Set the kernel stack the CPU loads on a ring3 -> ring0 trap (for the calling
 * CPU's own TSS). */
void tss_set_kernel_stack(uintptr_t stack_top);

/* An application processor switches from the trampoline GDT to the kernel GDT,
 * then loads its own per-CPU TSS into TR. */
void gdt_load_ap(void);
void tss_load_ap(void);

#endif /* OWOS_GDT_H */
