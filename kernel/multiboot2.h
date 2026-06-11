/* multiboot2.h — the subset of the Multiboot2 boot-info format we read.
 *
 * GRUB hands a multiboot2 kernel a completely different structure from
 * multiboot1: a total_size header followed by a chain of 8-byte-aligned tags,
 * terminated by a type-0 tag. We only consume the memory-map (type 6) and
 * framebuffer (type 8) tags, and translate them into the multiboot1
 * `struct multiboot_info` the rest of the kernel already understands (see
 * multiboot2_to_info in kernel.c), so pmm/fb need no per-format code.
 */
#ifndef OWOS_MULTIBOOT2_H
#define OWOS_MULTIBOOT2_H

#include <stdint.h>

/* Value the bootloader leaves in EAX for a multiboot2 boot. */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

#define MB2_TAG_END         0
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_EFI32       11   /* EFI 32-bit system table ptr (UEFI only)    */
#define MB2_TAG_EFI64       12   /* EFI 64-bit system table ptr (UEFI only)    */
#define MB2_TAG_ACPI_OLD    14   /* a copy of the ACPI 1.0 RSDP (RSDT path)    */
#define MB2_TAG_ACPI_NEW    15   /* a copy of the ACPI 2.0+ RSDP (has XSDT too) */

#define MB2_MEMORY_AVAILABLE 1   /* same code as multiboot1                    */

/* Every tag starts with this; `size` includes the header and is NOT padded,
 * so the next tag is at (this + ((size + 7) & ~7)). */
struct mb2_tag {
	uint32_t type;
	uint32_t size;
} __attribute__((packed));

/* type 6: memory map. A run of `entry_size`-byte entries follows the header. */
struct mb2_tag_mmap {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
} __attribute__((packed));

struct mb2_mmap_entry {
	uint64_t base_addr;
	uint64_t length;
	uint32_t type;
	uint32_t reserved;
} __attribute__((packed));

/* type 8: the framebuffer the loader set up. */
struct mb2_tag_framebuffer {
	uint32_t type;
	uint32_t size;
	uint64_t framebuffer_addr;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint8_t  framebuffer_bpp;
	uint8_t  framebuffer_type;
	uint16_t reserved;
	/* color_info follows; we don't read it (we only accept 32-bpp direct RGB) */
} __attribute__((packed));

/* type 14/15: a verbatim copy of the firmware's RSDP. On UEFI this is the ONLY
 * way to reach ACPI — the RSDP is not in the legacy BIOS memory area, so the
 * power.c scan finds nothing without it. `rsdp` is the raw RSDP bytes (20 for
 * v1, 36 for v2); we only need the first 20 (signature..rsdt_addr). */
struct mb2_tag_acpi {
	uint32_t type;
	uint32_t size;
	uint8_t  rsdp[];
} __attribute__((packed));

#endif /* OWOS_MULTIBOOT2_H */
