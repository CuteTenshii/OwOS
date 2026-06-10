/* kernel.c — OwOS entry point (called from boot.S).
 *
 * Brings up the core subsystems in order, sets up memory (physical frame
 * allocator, paging, kernel heap), enables interrupts, then drops into the
 * shell.
 */

#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "keyboard.h"
#include "multiboot.h"
#include "multiboot2.h"
#include "fs.h"
#include "ahci.h"
#include "nvme.h"
#include "net.h"
#include "heap.h"
#include "loader.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "sched.h"
#include "services.h"
#include "syscall.h"
#include "serial.h"
#include "shell.h"
#include "sb16.h"
#include "timer.h"
#include "vga.h"
#include "fb.h"
#include "wm.h"

/* Two CPU-bound kernel threads that never yield. If scheduling were
 * cooperative the first one would hang the machine; under preemption the
 * shell stays responsive and `tasks` shows their run counts climbing. */
static void spinner(void)
{
	for (;;)
		__asm__ volatile ("");
}

/* GRUB boots us through multiboot2 (its only reliable UEFI handoff). Translate
 * that tag-chain into the multiboot1 `struct multiboot_info` the rest of the
 * kernel consumes, so pmm/fb stay format-agnostic. The synthesized mmap is
 * emitted in multiboot1 entry layout (a 4-byte size=20 field, then the 8/8/4
 * addr/len/type) into a static scratch buffer that pmm walks the usual way.
 * Returns the filled-in info struct. */
static struct multiboot_info synth_mbi;
/* Real UEFI firmware hands GRUB a memory map with *hundreds* of descriptors
 * (ACPI, runtime services, MMIO holes, fragmented conventional RAM); QEMU has
 * only a handful. Cap generously — a too-small buffer silently drops the later
 * regions, and if the bulk RAM block lands past the cutoff, pmm sees ~0 MiB. */
static uint8_t synth_mmap[1024 * 24];   /* up to 1024 regions, 24 bytes each    */

static struct multiboot_info *multiboot2_to_info(uint32_t info_addr)
{
	struct multiboot_info *m = &synth_mbi;
	uint8_t *mz = (uint8_t *)m;
	for (unsigned i = 0; i < sizeof *m; i++)
		mz[i] = 0;

	int mmap_used = 0;
	/* The info block starts with total_size + reserved (8 bytes), then tags. */
	uint8_t *p = (uint8_t *)info_addr + 8;
	for (;;) {
		struct mb2_tag *tag = (struct mb2_tag *)p;
		if (tag->type == MB2_TAG_END)
			break;

		if (tag->type == MB2_TAG_MMAP) {
			struct mb2_tag_mmap *mm = (struct mb2_tag_mmap *)p;
			uint8_t *e   = p + sizeof *mm;
			uint8_t *end = p + mm->size;
			while (e + mm->entry_size <= end) {
				struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
				if ((mmap_used + 1) * 24 <= (int)sizeof synth_mmap) {
					uint8_t *o = synth_mmap + mmap_used * 24;
					*(uint32_t *)(o + 0)  = 20;             /* size field   */
					*(uint64_t *)(o + 4)  = me->base_addr;  /* addr         */
					*(uint64_t *)(o + 12) = me->length;     /* len          */
					*(uint32_t *)(o + 20) = me->type;       /* type         */
					mmap_used++;
				}
				e += mm->entry_size;
			}
		} else if (tag->type == MB2_TAG_FRAMEBUFFER) {
			struct mb2_tag_framebuffer *fbt = (struct mb2_tag_framebuffer *)p;
			m->framebuffer_addr   = fbt->framebuffer_addr;
			m->framebuffer_pitch  = fbt->framebuffer_pitch;
			m->framebuffer_width  = fbt->framebuffer_width;
			m->framebuffer_height = fbt->framebuffer_height;
			m->framebuffer_bpp    = fbt->framebuffer_bpp;
			m->framebuffer_type   = fbt->framebuffer_type;
			m->flags |= MULTIBOOT_FLAG_FB;
		}

		p += (tag->size + 7) & ~7u;    /* tags are 8-byte aligned             */
	}

	if (mmap_used) {
		m->flags     |= MULTIBOOT_FLAG_MMAP;
		m->mmap_addr  = (uint32_t)synth_mmap;
		m->mmap_length = (uint32_t)(mmap_used * 24);
	}
	return m;
}

void kernel_main(uint32_t magic, struct multiboot_info *mbi)
{
	serial_init();                 /* first: the console mirrors here         */
	vga_init();

	vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
	vga_write("OwOS\n");
	vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
	vga_write("booted in 32-bit protected mode via multiboot.\n");

	if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
		/* GRUB/UEFI path: ebx points at a multiboot2 tag chain, not an mbi. */
		mbi = multiboot2_to_info((uint32_t)mbi);
	} else if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		vga_set_color(VGA_WHITE, VGA_RED);
		vga_write("bad multiboot magic -- halting.\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	gdt_init();
	idt_init();
	irq_init();

	vga_write("mmdbg: path=");
	vga_write(magic == MULTIBOOT2_BOOTLOADER_MAGIC ? "mb2" : "mb1");
	vga_write(" mmapflag=");
	vga_write_dec((mbi->flags & MULTIBOOT_FLAG_MMAP) ? 1 : 0);
	vga_write(" entries=");
	vga_write_dec(mbi->mmap_length / 24);   /* synth/E820 entries are 24 bytes  */
	vga_putc('\n');

	pmm_init(mbi);                 /* read the memory map, track frames       */
	vga_write("pmm: ");
	vga_write_dec(pmm_total_bytes() / (1024 * 1024));
	vga_write(" MiB RAM detected.\n");

	paging_init();                 /* identity-map low 16 MiB, enable paging  */
	vga_write("paging: enabled (low 16 MiB identity-mapped).\n");

	heap_init();                   /* kmalloc/kfree over [4 MiB, 16 MiB)      */
	vga_write("heap: ready.\n");

	pci_init();                    /* enumerate the PCI bus                   */
	vga_write("pci: ");
	vga_write_dec((uint32_t)pci_count());
	vga_write(" devices found.\n");

	/* Bring up a linear framebuffer. Prefer one the bootloader handed us (GRUB
	 * via Multiboot, on real VMs/hardware where we can't drive the GPU to set a
	 * mode); otherwise program QEMU's DISPI adapter directly (the -kernel path,
	 * which provides no framebuffer). */
	{
		int rc = -1;
		if ((mbi->flags & MULTIBOOT_FLAG_FB) &&
		    mbi->framebuffer_type == MULTIBOOT_FB_TYPE_RGB &&
		    mbi->framebuffer_bpp == 32) {
			rc = fb_adopt((uint32_t)mbi->framebuffer_addr,
			              mbi->framebuffer_width, mbi->framebuffer_height,
			              mbi->framebuffer_pitch, mbi->framebuffer_bpp);
			if (rc == 0) {
				vga_write("fb: adopted bootloader framebuffer ");
				vga_write_dec(mbi->framebuffer_width);  vga_putc('x');
				vga_write_dec(mbi->framebuffer_height); vga_write("x32.\n");
			}
		}
		if (rc != 0)
			rc = fb_init(1024, 768);   /* VBE DISPI fallback (QEMU -kernel) */

		if (rc == 0)
			vga_write("fb: graphics mode up.\n");
		else
			vga_write("fb: no framebuffer available; staying in text mode.\n");
	}

	net_init();                    /* bring up the RTL8139 NIC, if present     */

	fs_init();                     /* mount (or format) the ATA disk          */

	ahci_init();                   /* bring up a SATA disk via AHCI, if present */
	nvme_init();                   /* bring up an NVMe namespace, if present    */

	syscall_init();                /* int 0x80 gate (DPL 3)                   */

	/* Install the embedded ring-3 programs onto the disk if absent. */
	{
		extern const unsigned char _binary_user_hello_elf_start[];
		extern const unsigned char _binary_user_hello_elf_end[];
		extern const unsigned char _binary_user_worker_elf_start[];
		extern const unsigned char _binary_user_worker_elf_end[];
		extern const unsigned char _binary_user_args_elf_start[];
		extern const unsigned char _binary_user_args_elf_end[];
		extern const unsigned char _binary_user_pipe_elf_start[];
		extern const unsigned char _binary_user_pipe_elf_end[];
		extern const unsigned char _binary_user_produce_elf_start[];
		extern const unsigned char _binary_user_produce_elf_end[];
		extern const unsigned char _binary_user_upper_elf_start[];
		extern const unsigned char _binary_user_upper_elf_end[];
		extern const unsigned char _binary_user_edit_elf_start[];
		extern const unsigned char _binary_user_edit_elf_end[];
		extern const unsigned char _binary_user_curl_elf_start[];
		extern const unsigned char _binary_user_curl_elf_end[];
		char probe;
		/* A Unix-like layout: programs in /bin, a home for the user. mkdir is
		 * a no-op if the directory already exists from a previous boot. */
		fs_mkdir("/bin");
		fs_mkdir("/home");
		fs_mkdir("/home/root");
		if (fs_read("/bin/hello", &probe, 0) < 0)
			fs_create("/bin/hello", _binary_user_hello_elf_start,
			          (uint32_t)(_binary_user_hello_elf_end -
			                     _binary_user_hello_elf_start));
		if (fs_read("/bin/worker", &probe, 0) < 0)
			fs_create("/bin/worker", _binary_user_worker_elf_start,
			          (uint32_t)(_binary_user_worker_elf_end -
			                     _binary_user_worker_elf_start));
		if (fs_read("/bin/args", &probe, 0) < 0)
			fs_create("/bin/args", _binary_user_args_elf_start,
			          (uint32_t)(_binary_user_args_elf_end -
			                     _binary_user_args_elf_start));
		if (fs_read("/bin/pipe", &probe, 0) < 0)
			fs_create("/bin/pipe", _binary_user_pipe_elf_start,
			          (uint32_t)(_binary_user_pipe_elf_end -
			                     _binary_user_pipe_elf_start));
		if (fs_read("/bin/produce", &probe, 0) < 0)
			fs_create("/bin/produce", _binary_user_produce_elf_start,
			          (uint32_t)(_binary_user_produce_elf_end -
			                     _binary_user_produce_elf_start));
		if (fs_read("/bin/upper", &probe, 0) < 0)
			fs_create("/bin/upper", _binary_user_upper_elf_start,
			          (uint32_t)(_binary_user_upper_elf_end -
			                     _binary_user_upper_elf_start));
		if (fs_read("/bin/edit", &probe, 0) < 0)
			fs_create("/bin/edit", _binary_user_edit_elf_start,
			          (uint32_t)(_binary_user_edit_elf_end -
			                     _binary_user_edit_elf_start));
		if (fs_read("/bin/curl", &probe, 0) < 0)
			fs_create("/bin/curl", _binary_user_curl_elf_start,
			          (uint32_t)(_binary_user_curl_elf_end -
			                     _binary_user_curl_elf_start));
	}
	vga_write("usermode: ready ('run hello').\n");

	timer_init();                  /* IRQ0: keep time                         */
	keyboard_init();               /* IRQ1: buffer keystrokes                 */
	sb16_init();                   /* IRQ5: PCM audio (no-op if absent)       */

	sched_init();                  /* current context becomes task "main"     */
	sched_create_thread("spin-a", spinner);
	sched_create_thread("spin-b", spinner);
	vga_write("scheduler: 2 worker threads spawned.\n");

	services_init();               /* supervisor thread + auto-start daemons   */

	wm_init();                     /* graphics desktop + mouse, if fb is up    */

	__asm__ volatile ("sti");      /* interrupts on; preemption begins        */

	shell_init();

	for (;;) {
		if (keyboard_take_interrupt())     /* Ctrl+C with no command running  */
			shell_interrupt();
		char c;
		while (keyboard_poll(&c))          /* PS/2 keyboard (the QEMU window)  */
			shell_feed(c);
		while (serial_poll(&c))            /* host terminal (-serial stdio)    */
			shell_feed(c);
		__asm__ volatile ("hlt");  /* sleep until the next interrupt          */
	}
}
