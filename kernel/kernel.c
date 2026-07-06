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
#include "firmware.h"
#include "smbios.h"
#include "fs.h"
#include "users.h"
#include "ahci.h"
#include "nvme.h"
#include "sd.h"
#include "atapi.h"
#include "blk.h"
#include "hfs.h"
#include "ext.h"
#include "iso9660.h"
#include "fat.h"
#include "net.h"
#include "heap.h"
#include "entropy.h"
#include "loader.h"
#include "paging.h"
#include "pci.h"
#include "smp.h"
#include "pmm.h"
#include "power.h"
#include "sched.h"
#include "syscall.h"
#include "serial.h"
#include "shell.h"
#include "log.h"
#include "sb16.h"
#include "hda.h"
#include "timer.h"
#include "console.h"
#include "fb.h"
#include "mouse.h"
#include "vmsvga.h"
#include "vmtools.h"
#include "uwin.h"
#include "xhci.h"

/* Boot-stage breadcrumb shown on the panic screen. On a laptop there is no COM1
 * to capture the serial log and the panic clears the boot log off screen, so the
 * panic must carry "how far did boot get" itself (idt.c prints it as boot=...).
 * Updated by bootmark(). */
const char *g_boot_stage = "early";
static void bootmark(const char *s)
{
	g_boot_stage = s;
}

/* Install an embedded program/asset blob to owofs, (re)creating it when absent
 * OR when the on-disk size differs from the built-in blob. The size check makes
 * a rebuilt binary actually replace the persisted copy in disk.img — without it
 * an "install only if absent" boots the stale program after every rebuild. */
static void install_blob(const char *path, const unsigned char *start,
                         const unsigned char *end)
{
	uint32_t len = (uint32_t)(end - start);
	if (fs_size(path) == (int)len) return;     /* already up to date */
	fs_delete(path);                           /* no-op if absent; drop stale copy */
	fs_create(path, start, len);
}

/* Install any embedded blob to owofs from just its disk path + objcopy symbol
 * base: the start/end externs and the install call all derive from the base, so
 * adding a shipped file (program, font, asset) is one line and never a separate
 * extern pair. INSTALL_APP wraps the /bin program convention (build.mk renames
 * each program blob to _binary_app_<nm>_{start,end}). */
#define INSTALL_FILE(diskpath, symbase) do {                                  \
		extern const unsigned char symbase##_start[];                        \
		extern const unsigned char symbase##_end[];                          \
		install_blob(diskpath, symbase##_start, symbase##_end);              \
	} while (0)
#define INSTALL_APP(nm) INSTALL_FILE("/bin/" #nm, _binary_app_##nm)

/* Two CPU-bound kernel threads that never yield. If scheduling were
 * cooperative the first one would hang the machine; under preemption the
 * shell stays responsive and `tasks` shows their run counts climbing. */
static void spinner(void)
{
	for (;;)
		__asm__ volatile ("");
}

/* Load a /bin program and run it as a detached root process (the WM reaps it).
 * Used to launch PID 1 (/bin/init) at boot. */
static void spawn_bin_detached(const char *path, char *arg0)
{
	int sz = fs_size(path);
	uint8_t *buf = sz > 0 ? kmalloc((uint32_t)sz) : 0;
	if (!buf)
		return;
	int n = fs_read(path, buf, (uint32_t)sz);
	char *argv[1] = { arg0 };
	if (n > 0)
		exec_program(buf, (uint32_t)n, 1, argv);
	kfree(buf);
}

/* Slow post-`sti` device bring-up, run as a background thread so the shell input
 * loop in kernel_main starts immediately. On real hardware DHCP retransmits and
 * xHCI port resets (both timer_sleep-based) take seconds; doing them inline left
 * keystrokes piling up in the keyboard ring — captured at `sti`, but not drained
 * until the loop ran — and flushing all at once when boot finished. A kernel
 * thread runs with IF=1, so the "must run after sti" timer_sleep paths are happy. */
static void boot_bringup(void)
{
	/* Launch PID 1: /bin/init, detached from this post-sti boot thread, as root
	 * (init inherits uid 0, so the daemons it spawns get the privileges dhcpcd's
	 * raw socket / sshd's spawn-as-user need). init reads /etc/inittab and brings
	 * up the user daemons (dhcpcd/mdnsd/sshd) -- the kernel no longer does init's
	 * job. Non-blocking here (init detaches its own children). */
	spawn_bin_detached("/bin/init", "init");

	bootmark("xhci");
	xhci_boot();                   /* USB host up + devices configured.            */

	bootmark("vmtools");
	vmtools_init();                /* VMware guest agent: clock sync + guestinfo +   */
	                               /* heartbeat + clipboard sync over the backdoor.  */
	                               /* No-op off VMware.                              */

	sched_exit(0);                 /* one-shot: done, leave a zombie to reap        */
}

/* GRUB boots us through multiboot2 (its only reliable UEFI handoff). Translate
 * that tag-chain into the multiboot1 `struct multiboot_info` the rest of the
 * kernel consumes, so pmm/fb stay format-agnostic. The synthesized mmap is
 * emitted in multiboot1 entry layout (a 4-byte size=20 field, then the 8/8/4
 * addr/len/type) into a static scratch buffer that pmm walks the usual way.
 * Returns the filled-in info struct. */
/* True if the NUL-terminated command line `s` contains the whitespace-delimited
 * token `tok` (so "verbose" matches "ro verbose quiet" but not "verbosely").
 * Used to read boot flags like `verbose` off the kernel command line. */
static int cmdline_has(const char *s, const char *tok)
{
	if (!s)
		return 0;
	for (;;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '\0')
			return 0;
		const char *a = s, *b = tok;
		while (*a && *a != ' ' && *a != '\t' && *a == *b) {
			a++;
			b++;
		}
		if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\t'))
			return 1;
		while (*s && *s != ' ' && *s != '\t')   /* skip to the next token       */
			s++;
	}
}

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
	int uefi = 0;
	/* The info block starts with total_size + reserved (8 bytes), then tags. */
	uint8_t *p = (uint8_t *)info_addr + 8;
	for (;;) {
		struct mb2_tag *tag = (struct mb2_tag *)p;
		if (tag->type == MB2_TAG_END)
			break;

		if (tag->type == MB2_TAG_CMDLINE) {
			/* GRUB's command line. A `verbose` token here shows the boot log
			 * on screen, same as the mb1 path below. */
			if (cmdline_has(((struct mb2_tag_string *)p)->string, "verbose"))
				log_set_verbose(1);
		} else if (tag->type == MB2_TAG_MMAP) {
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
		} else if (tag->type == MB2_TAG_ACPI_OLD ||
		           tag->type == MB2_TAG_ACPI_NEW) {
			/* The firmware's RSDP. On UEFI it's not in the legacy BIOS area,
			 * so hand it to power.c now (we're still pre-paging and the whole
			 * 4 GiB is flat) — it stashes a copy so shutdown can reach ACPI. */
			struct mb2_tag_acpi *a = (struct mb2_tag_acpi *)p;
			acpi_set_rsdp(a->rsdp);
		} else if (tag->type == MB2_TAG_EFI32) {
			/* GRUB only hands us an EFI system-table tag on a UEFI boot;
			 * legacy BIOS / CSM carries none. Its configuration-table array
			 * is how we reach SMBIOS under UEFI (smbios.c). */
			uefi = 1;
			smbios_set_efi(((struct mb2_tag_efi32 *)p)->pointer, 0);
		} else if (tag->type == MB2_TAG_EFI64) {
			uefi = 1;
			smbios_set_efi(((struct mb2_tag_efi64 *)p)->pointer, 1);
		} else if (tag->type == MB2_TAG_SMBIOS) {
			/* The SMBIOS entry point. On UEFI the "_SM_"/"_SM3_" anchor is
			 * not in the legacy f-segment, so this tag is the only route to
			 * DMI (machine vendor/product for the splash's Host line). The
			 * entry point starts at the tag's `tables` field. */
			struct mb2_tag_smbios *sm = (struct mb2_tag_smbios *)p;
			smbios_set_entry((uint32_t)sm->tables);
		}

		p += (tag->size + 7) & ~7u;    /* tags are 8-byte aligned             */
	}

	firmware_set(uefi ? FIRMWARE_UEFI : FIRMWARE_BIOS);

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
	con_init();

	/* Boot is quiet on screen by default; a `verbose` token on the kernel
	 * command line shows the bring-up log live. Either way the journal records
	 * it (and the serial port streams it). Read it before the first log line so
	 * the whole boot honours it. mb1 carries the cmdline in the info struct;
	 * mb2's is read in multiboot2_to_info() below. */
	if (magic == MULTIBOOT_BOOTLOADER_MAGIC &&
	    (mbi->flags & MULTIBOOT_FLAG_CMD) &&
	    cmdline_has((const char *)(uintptr_t)mbi->cmdline, "verbose"))
		log_set_verbose(1);

	con_set_color(CON_LIGHT_CYAN, CON_BLACK);
	con_write("OwOS\n");
	con_set_color(CON_LIGHT_GREY, CON_BLACK);
	con_write("booted in 32-bit protected mode via multiboot.\n");

	if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
		/* GRUB/UEFI path: ebx points at a multiboot2 tag chain, not an mbi. */
		mbi = multiboot2_to_info((uint32_t)mbi);
	} else if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		con_set_color(CON_WHITE, CON_RED);
		con_write("bad multiboot magic -- halting.\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	gdt_init();
	idt_init();
	irq_init();

	con_printf("mmdbg: path=%s mmapflag=%u entries=%u firmware=%s\n",
	           magic == MULTIBOOT2_BOOTLOADER_MAGIC ? "mb2" : "mb1",
	           (mbi->flags & MULTIBOOT_FLAG_MMAP) ? 1 : 0,
	           mbi->mmap_length / 24,   /* synth/E820 entries are 24 bytes  */
	           firmware_name());

	pmm_init(mbi);                 /* read the memory map, track frames       */
	con_printf("pmm: %u MiB RAM detected", pmm_installed_kib() / 1024);
	if (pmm_installed_kib() > pmm_total_kib()) {
		/* 32-bit, no PAE: we can only manage the low 4 GiB of it. */
		con_printf(" (%u MiB usable)", pmm_total_kib() / 1024);
	}
	con_write(".\n");

	paging_init();                 /* identity-map low 16 MiB, enable paging  */
	con_write("paging: enabled (low 16 MiB identity-mapped).\n");

	heap_init();                   /* kmalloc/kfree over [4 MiB, 16 MiB)      */
	con_write("heap: ready.\n");

	entropy_init();                /* seed the RNG pool before any consumer    */

	smbios_init();                 /* read DMI vendor/product for the splash  */

	pci_init();                    /* enumerate the PCI bus                   */
	con_printf("pci: %u devices found.\n", (uint32_t)pci_count());

	smp_init();                    /* discover CPUs via ACPI MADT (no AP bring-up yet) */

	/* Bring up a linear framebuffer. Prefer one the bootloader handed us (GRUB
	 * via Multiboot, on real VMs/hardware where we can't drive the GPU to set a
	 * mode); otherwise program QEMU's DISPI adapter directly (the -kernel path,
	 * which provides no framebuffer). */
	{
		int rc = -1, adopted = 0, svga = 0;
		/* Prefer the VMware SVGA II adapter when its PCI device is present:
		 * driving it directly (mode control + a command FIFO) beats adopting the
		 * dumb VBE framebuffer GRUB hands us on a VMware guest, where the legacy
		 * framebuffer needn't keep auto-refreshing once SVGA mode is on. Returns
		 * -1 on any non-VMware machine, so the adopt and DISPI paths below still
		 * serve real GPUs (UEFI GOP) and QEMU's default VGA. */
		rc = vmsvga_init(1024, 768);
		svga = (rc == 0);
		if (rc != 0 &&
		    (mbi->flags & MULTIBOOT_FLAG_FB) &&
		    mbi->framebuffer_type == MULTIBOOT_FB_TYPE_RGB &&
		    mbi->framebuffer_bpp == 32) {
			rc = fb_adopt((uint32_t)mbi->framebuffer_addr,
			              mbi->framebuffer_width, mbi->framebuffer_height,
			              mbi->framebuffer_pitch, mbi->framebuffer_bpp);
			adopted = (rc == 0);
		}
		if (rc != 0)
			rc = fb_init(1024, 768);   /* VBE DISPI fallback (QEMU -kernel) */


		/* On a verbose boot, catch the framebuffer console up with everything
		 * logged before graphics mode existed (banner, pmm, paging, ...), which
		 * until now only reached the 0xB8000 text the graphics mode hides. After
		 * this the status lines below stream onto it live. */
		if (rc == 0 && log_verbose())
			journal_to_fb();

		if (adopted) {
			con_printf("fb: adopted bootloader framebuffer %ux%ux32.\n",
			           mbi->framebuffer_width, mbi->framebuffer_height);
		}
		if (svga)
			con_write("fb: VMware SVGA II adopted.\n");
		if (rc == 0)
			con_write("fb: graphics mode up.\n");
		else
			con_write("fb: no framebuffer available; staying in text mode.\n");
	}

	net_init();                    /* bring up the RTL8139 NIC, if present     */

	ahci_init();                   /* bring up a SATA disk via AHCI, if present */
	nvme_init();                   /* bring up an NVMe namespace, if present    */
	sd_init();                     /* bring up an SD/MMC card via SDHCI, if any */
	atapi_init();                  /* find a CD/DVD drive on the IDE bus, if any */

	blk_init();                    /* pick owofs's backing disk: IDE>AHCI>NVMe */
	fs_init();                     /* mount it (auto-format only the IDE disk) */
	/* No auto-mount of removable volumes at boot — they mount only when asked,
	 * via `mount <dev> <point>` or the Files app's Connect button (fs_mount, which
	 * probes the device and adds it to fs.c's mount table). Several can be mounted
	 * at once, each at its own /media/<name>. */

	syscall_init();                /* int 0x80 gate (DPL 3)                   */

	/* Install the embedded programs onto the disk if absent. */
	{
		char probe;
		/* A Unix-like layout: programs in /bin, a home for the user. mkdir is
		 * a no-op if the directory already exists from a previous boot. */
		fs_mkdir("/bin");
		fs_mkdir("/home");
		fs_mkdir("/home/root");
		fs_mkdir("/home/root/Downloads");   /* the browser saves here */

		/* The account database. /etc/passwd seeds the root account with no
		 * password (name:pwhash:uid:gid:home); /etc/group seeds the root group.
		 * Both are plain owofs files managed thereafter by the shell's
		 * useradd/groupadd/passwd commands. Only created if absent, so a previous
		 * boot's accounts (and any passwords set) persist. */
		fs_mkdir("/etc");
		/* The shared UI font: gfx loads it at runtime so no app bakes in a
		 * bitmap atlas. One on-disk copy, installed if a prior boot lacks it. */
		fs_mkdir("/etc/fonts");
		INSTALL_FILE("/etc/fonts/LiberationSans-Regular.ttf", _binary_fonts_LiberationSans_Regular_ttf);
		INSTALL_FILE("/etc/fonts/LiberationMono-Regular.ttf", _binary_fonts_LiberationMono_Regular_ttf);
		if (fs_read("/etc/passwd", &probe, 0) < 0)
			fs_create("/etc/passwd", "root::0:0:/home/root\n", 21);
		if (fs_read("/etc/group", &probe, 0) < 0)
			fs_create("/etc/group", "root:0:\n", 8);
		/* mDNS/DNS-SD advertisements for /bin/mdnsd: one "type port [txt...]" per
		 * line. Seeds ssh + smb so they're announced on the .local link again;
		 * install-if-absent so a user's edits persist. */
		if (fs_read("/etc/mdns.conf", &probe, 0) < 0)
			fs_create("/etc/mdns.conf", "_ssh._tcp 22\n_smb._tcp 445\n", 26);
		/* init's service table: one /bin daemon per line (blank/# ignored). PID 1
		 * (/bin/init) spawns each at boot -- the kernel no longer autostarts any
		 * daemon itself. The compositor is first so the desktop comes up before the
		 * network daemons. Install-if-absent so a user's edits persist. */
		if (fs_read("/etc/inittab", &probe, 0) < 0)
			fs_create("/etc/inittab", "compositor\ndhcpcd\nmdnsd\nsshd\n", 29);
		users_init();
		INSTALL_APP(edit);     INSTALL_APP(curl);     INSTALL_APP(editor);
		INSTALL_APP(about);    INSTALL_APP(clock);    INSTALL_APP(calc);
		INSTALL_APP(files);
		INSTALL_APP(browser);  INSTALL_APP(imgview);  INSTALL_APP(term);
		INSTALL_APP(compositor);   /* the user-space desktop; launched first by init */
		INSTALL_APP(neofetch); INSTALL_APP(lspci);    INSTALL_APP(uname);    INSTALL_APP(ping);
		INSTALL_APP(dig);      INSTALL_APP(stun);     INSTALL_APP(torrent);  INSTALL_APP(player);
		INSTALL_APP(settings); INSTALL_APP(ssh);      INSTALL_APP(sshd);
		INSTALL_APP(clipd);      INSTALL_APP(vmclipd);
		INSTALL_APP(cliphist); INSTALL_APP(notifyd);  INSTALL_APP(webp);
		INSTALL_APP(json);     INSTALL_APP(base64);   INSTALL_APP(alac);     INSTALL_APP(aac);      INSTALL_APP(mpeg);     INSTALL_APP(h264);     INSTALL_APP(smbd);
		INSTALL_APP(dhcpcd);   INSTALL_APP(ntpd);     INSTALL_APP(mdnsd);
		INSTALL_APP(cast);
		INSTALL_APP(init);
		INSTALL_FILE("/bin/notify-send", _binary_app_notifysend);
	}
	con_write("usermode: ready ('run curl <url>').\n");

	timer_init();                  /* IRQ0: keep time                         */
	keyboard_init();               /* IRQ1: buffer keystrokes                 */
	sb16_init();                   /* IRQ5: PCM audio (no-op if absent)       */
	hda_init();                    /* HD Audio / Realtek codec (no-op if absent) */

	sched_init();                  /* current context becomes task "main"     */
	sched_create_thread("spin-a", spinner);
	sched_create_thread("spin-b", spinner);
	con_write("scheduler: 2 worker threads spawned.\n");

	/* PS/2 mouse: force the i8042 IRQ/clock bits so the pointer works before the
	 * user-space compositor comes up. The compositor sets the pointer bounds to
	 * the screen size via SYS_INPUT_SET_BOUNDS; seed them from the framebuffer. */
	if (fb_active())
		mouse_init((int)fb_width() - 1, (int)fb_height() - 1);

	bootmark("sti");
	__asm__ volatile ("sti");      /* interrupts on; preemption begins        */

	smp_start_aps();               /* wake the other cores (they park for now) */

	/* DHCP + xHCI + /media mount are slow on real hardware (timer_sleep-based
	 * retries/resets). Run them in a background thread so the shell input loop
	 * below starts draining keystrokes immediately instead of letting them pile
	 * up in the keyboard ring for several seconds and flush all at once. */
	sched_create_thread("bootup", boot_bringup);

	bootmark("shell");
	shell_init();

	for (;;) {
		char c; unsigned char mods;
		/* Physical keyboard (the QEMU/HW window). While the user-space compositor
		 * is alive it owns the keyboard: it drains it via SYS_INPUT_POLL and routes
		 * keys to the focused window (Ctrl+C reaches the window the same way). The
		 * kernel must NOT also drain it here, or input would split/race. When no
		 * compositor is running (not started yet, or it died), the physical
		 * keyboard falls through to the recovery console — the failsafe that keeps
		 * the machine usable. The COM1 serial path below is independent of this and
		 * always drives the shell (it's the debug console + test harness). */
		if (!uwin_compositor_active()) {
			if (keyboard_take_interrupt())     /* Ctrl+C -> cancel the shell line */
				shell_interrupt();
			while (keyboard_poll(&c, &mods))
				shell_feed(c);
		}
		/* else: the compositor owns the keyboard — it drains both the keys AND the
		 * Ctrl+C latch itself via SYS_INPUT_POLL (which surfaces the interrupt as a
		 * KEY_INTR event to the focused window). The kernel must not consume either
		 * here, or it would race the compositor for them. */
		while (serial_poll(&c))            /* host terminal (-serial stdio)    */
			shell_feed(c);                 /* the debug console always drives shell */
		__asm__ volatile ("hlt");  /* sleep until the next interrupt          */
	}
}
