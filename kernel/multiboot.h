/* multiboot.h — the subset of the Multiboot v1 info structures we read.
 *
 * The bootloader fills this out and passes a pointer in EBX. We only care
 * about the memory map, which tells us which physical ranges are usable RAM.
 */
#ifndef OWOS_MULTIBOOT_H
#define OWOS_MULTIBOOT_H

#include <stdint.h>

/* Value the bootloader leaves in EAX, confirming a multiboot-compliant boot. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_FLAG_MEM (1 << 0)  /* mem_lower/mem_upper valid          */
#define MULTIBOOT_FLAG_CMD (1 << 2)  /* cmdline (a physical char* ) valid  */
#define MULTIBOOT_FLAG_MMAP (1 << 6) /* mmap_length/mmap_addr valid        */
#define MULTIBOOT_FLAG_FB (1 << 12)  /* framebuffer_* fields valid          */

#define MULTIBOOT_MEMORY_AVAILABLE 1 /* mmap entry type: usable RAM        */

#define MULTIBOOT_FB_TYPE_RGB 1 /* framebuffer_type: direct RGB pixels */

struct multiboot_info {
  uint32_t flags;
  uint32_t mem_lower;
  uint32_t mem_upper;
  uint32_t boot_device;
  uint32_t cmdline;
  uint32_t mods_count;
  uint32_t mods_addr;
  uint32_t syms[4];
  uint32_t mmap_length; /* total size of the mmap buffer, in bytes    */
  uint32_t mmap_addr;   /* physical address of the first entry        */
  uint32_t drives_length;
  uint32_t drives_addr;
  uint32_t config_table;
  uint32_t boot_loader_name;
  uint32_t apm_table;
  uint32_t vbe_control_info;
  uint32_t vbe_mode_info;
  uint16_t vbe_mode;
  uint16_t vbe_interface_seg;
  uint16_t vbe_interface_off;
  uint16_t vbe_interface_len;
  /* Framebuffer the bootloader set up for us (valid only with
   * MULTIBOOT_FLAG_FB). On real hardware / GRUB this is how we inherit a linear
   * framebuffer without driving the GPU ourselves. */
  uint64_t framebuffer_addr;   /* physical base of the linear framebuffer    */
  uint32_t framebuffer_pitch;  /* bytes per scanline (may exceed width*bpp/8) */
  uint32_t framebuffer_width;  /* visible width in pixels                    */
  uint32_t framebuffer_height; /* visible height in pixels                   */
  uint8_t framebuffer_bpp;     /* bits per pixel                             */
  uint8_t framebuffer_type;    /* 1 = direct RGB (see MULTIBOOT_FB_TYPE_RGB)  */
  uint8_t color_info[6];       /* RGB field positions/sizes for type 1        */
} __attribute__((packed));

/* One memory-map entry. `size` is the entry length NOT counting the size
 * field itself, so the next entry is at (this + size + 4). */
struct multiboot_mmap_entry {
  uint32_t size;
  uint64_t addr;
  uint64_t len;
  uint32_t type;
} __attribute__((packed));

#endif /* OWOS_MULTIBOOT_H */
