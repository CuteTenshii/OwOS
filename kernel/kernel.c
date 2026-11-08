/* kernel.c: OwOS entry point (called from boot.S); brings up subsystems in order, then runs the shell. */

#include <stdint.h>

#include "ahci.h"
#include "atapi.h"
#include "blk.h"
#include "console.h"
#include "entropy.h"
#include "ext.h"
#include "fat.h"
#include "fb.h"
#include "firmware.h"
#include "fs.h"
#include "gdt.h"
#include "hda.h"
#include "heap.h"
#include "hfs.h"
#include "idt.h"
#include "irq.h"
#include "irqflags.h"
#include "iso9660.h"
#include "keyboard.h"
#include "kstring.h"
#include "loader.h"
#include "log.h"
#include "mouse.h"
#include "multiboot.h"
#include "multiboot2.h"
#include "net.h"
#include "nvme.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "power.h"
#include "sb16.h"
#include "sched.h"
#include "sd.h"
#include "serial.h"
#include "shell.h"
#include "smbios.h"
#include "smp.h"
#include "syscall.h"
#include "timer.h"
#include "users.h"
#include "uwin.h"
#include "virtiogpu.h"
#include "vmsvga.h"
#include "vmtools.h"
#include "wallclock.h"
#include "xhci.h"

/* Boot-stage breadcrumb printed by the panic screen (idt.c). */
const char *g_boot_stage = "early";
static void bootmark(const char *s) { g_boot_stage = s; }

static void shell_feed_locked(char c)
{
  uintptr_t flags = irq_save();
  sched_kernel_enter();
  shell_feed(c);
  sched_kernel_leave();
  irq_restore(flags);
}

static void shell_interrupt_locked(void)
{
  uintptr_t flags = irq_save();
  sched_kernel_enter();
  shell_interrupt();
  sched_kernel_leave();
  irq_restore(flags);
}

/* Install an embedded blob to owofs when absent or when its size differs from the built-in one. */
static void install_blob(const char *path, const unsigned char *start, const unsigned char *end, uint32_t mode)
{
  uint32_t len = (uint32_t)(end - start);
  if (fs_size(path) == (int)len)
    return;        /* already up to date */
  fs_delete(path); /* no-op if absent; drop stale copy */
  fs_create(path, start, len);
  fs_chmod(path, mode);
}

/* Install an embedded blob given its disk path and objcopy symbol base; INSTALL_APP is the /bin form. */
#define INSTALL_FILE(diskpath, symbase, mode)                                                                          \
  do {                                                                                                                 \
    extern const unsigned char symbase##_start[];                                                                      \
    extern const unsigned char symbase##_end[];                                                                        \
    install_blob(diskpath, symbase##_start, symbase##_end, mode);                                                      \
  } while (0)
#define INSTALL_APP(nm) INSTALL_FILE("/bin/" #nm, _binary_app_##nm, 0755)
#define INSTALL_DESKTOP(nm) INSTALL_FILE("/usr/share/applications/" #nm ".desktop", _binary_desktop_##nm, 0644)

/* Create a directory / file only if absent, with an explicit mode (the boot session runs as root). */
static void mkdir_mode(const char *path, uint32_t mode)
{
  if (fs_mkdir(path) == 0)
    fs_chmod(path, mode);
}

/* Add a daemon line to an inittab that predates it (seed_file only writes a missing file). */
static void inittab_ensure(const char *daemon)
{
  char buf[256];
  int n = fs_read("/etc/inittab", buf, sizeof buf - 1);
  uint32_t dl = (uint32_t)strlen(daemon);
  if (n < 0 || (uint32_t)n + dl + 2 > sizeof buf)
    return;
  buf[n] = '\0';
  for (const char *p = buf; *p; p++)
    if ((p == buf || p[-1] == '\n') && memcmp(p, daemon, dl) == 0 && (p[dl] == '\n' || p[dl] == '\0'))
      return;
  if (n > 0 && buf[n - 1] != '\n')
    buf[n++] = '\n';
  memcpy(buf + n, daemon, dl);
  buf[n + (int)dl] = '\n';
  fs_delete("/etc/inittab");
  fs_create("/etc/inittab", buf, (uint32_t)n + dl + 1);
  fs_chmod("/etc/inittab", 0644);
}

static void seed_file(const char *path, const char *data, uint32_t len, uint32_t mode)
{
  char probe;
  if (fs_read(path, &probe, 0) >= 0)
    return;
  fs_create(path, data, len);
  fs_chmod(path, mode);
}

/* CPU-bound thread that never yields (preemption test). */
static void spinner(void)
{
  for (;;)
    __asm__ volatile("");
}

/* Load a /bin program and run it as a detached root process. */
static void spawn_bin_detached(const char *path, char *arg0)
{
  int sz = fs_size(path);
  uint8_t *buf = sz > 0 ? kmalloc((uint32_t)sz) : 0;
  if (!buf)
    return;
  int n = fs_read(path, buf, (uint32_t)sz);
  char *argv[1] = {arg0};
  if (n > 0)
    exec_program(buf, (uint32_t)n, 1, argv);
  kfree(buf);
}

/* Slow post-sti bring-up in a background thread so the shell loop starts at once. */
static void boot_bringup(void)
{
  /* PID 1 runs as root so the daemons it spawns inherit uid 0. */
  spawn_bin_detached("/bin/init", "init");

  bootmark("xhci");
  xhci_boot();

  bootmark("vmtools");
  vmtools_init(); /* no-op off VMware */

  sched_exit(0);
}

/* True if command line `s` has `tok` as a whitespace-delimited token. */
static bool cmdline_has(const char *s, const char *tok)
{
  if (!s)
    return false;
  for (;;) {
    while (*s == ' ' || *s == '\t')
      s++;
    if (*s == '\0')
      return false;
    const char *a = s, *b = tok;
    while (*a && *a != ' ' && *a != '\t' && *a == *b) {
      a++;
      b++;
    }
    if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\t'))
      return true;
    while (*s && *s != ' ' && *s != '\t')
      s++;
  }
}

static struct multiboot_info synth_mbi;
/* UEFI maps can have hundreds of regions; too small a cap drops later ones. */
static uint8_t synth_mmap[1024 * 24];

/* Translate GRUB's multiboot2 tags into a multiboot1 info struct (mmap in mb1 entry layout). */
static struct multiboot_info *multiboot2_to_info(uintptr_t info_addr)
{
  struct multiboot_info *m = &synth_mbi;
  uint8_t *mz = (uint8_t *)m;
  for (unsigned i = 0; i < sizeof *m; i++)
    mz[i] = 0;

  int mmap_used = 0;
  bool uefi = false;
  /* The info block starts with total_size + reserved (8 bytes), then tags. */
  uint8_t *p = (uint8_t *)(uintptr_t)info_addr + 8;
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
      uint8_t *e = p + sizeof *mm;
      uint8_t *end = p + mm->size;
      while (e + mm->entry_size <= end) {
        struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
        if ((mmap_used + 1) * 24 <= (int)sizeof synth_mmap) {
          uint8_t *o = synth_mmap + mmap_used * 24;
          *(uint32_t *)(o + 0) = 20;            /* size field   */
          *(uint64_t *)(o + 4) = me->base_addr; /* addr         */
          *(uint64_t *)(o + 12) = me->length;   /* len          */
          *(uint32_t *)(o + 20) = me->type;     /* type         */
          mmap_used++;
        }
        e += mm->entry_size;
      }
    } else if (tag->type == MB2_TAG_FRAMEBUFFER) {
      struct mb2_tag_framebuffer *fbt = (struct mb2_tag_framebuffer *)p;
      m->framebuffer_addr = fbt->framebuffer_addr;
      m->framebuffer_pitch = fbt->framebuffer_pitch;
      m->framebuffer_width = fbt->framebuffer_width;
      m->framebuffer_height = fbt->framebuffer_height;
      m->framebuffer_bpp = fbt->framebuffer_bpp;
      m->framebuffer_type = fbt->framebuffer_type;
      m->flags |= MULTIBOOT_FLAG_FB;
    } else if (tag->type == MB2_TAG_ACPI_OLD || tag->type == MB2_TAG_ACPI_NEW) {
      /* UEFI RSDP is outside the legacy BIOS area; power.c keeps a copy for shutdown. */
      struct mb2_tag_acpi *a = (struct mb2_tag_acpi *)p;
      acpi_set_rsdp(a->rsdp);
    } else if (tag->type == MB2_TAG_EFI32) {
      /* EFI system table (UEFI only): its config tables lead to SMBIOS. */
      uefi = true;
      smbios_set_efi(((struct mb2_tag_efi32 *)p)->pointer, 0);
    } else if (tag->type == MB2_TAG_EFI64) {
      uefi = true;
      smbios_set_efi(((struct mb2_tag_efi64 *)p)->pointer, 1);
    } else if (tag->type == MB2_TAG_SMBIOS) {
      /* SMBIOS entry point (the only route to DMI on UEFI) starts at `tables`. */
      struct mb2_tag_smbios *sm = (struct mb2_tag_smbios *)p;
      smbios_set_entry((uint32_t)(uintptr_t)sm->tables);
    }

    p += (tag->size + 7) & ~7u;
  }

  firmware_set(uefi ? FIRMWARE_UEFI : FIRMWARE_BIOS);

  if (mmap_used) {
    m->flags |= MULTIBOOT_FLAG_MMAP;
    m->mmap_addr = (uint32_t)(uintptr_t)synth_mmap;
    m->mmap_length = (uint32_t)(mmap_used * 24);
  }
  return m;
}

void kernel_main(uint32_t magic, struct multiboot_info *mbi)
{
  serial_init();
  con_init();

  /* A `verbose` token on the cmdline shows the boot log live (mb2's is read in multiboot2_to_info). */
  if (magic == MULTIBOOT_BOOTLOADER_MAGIC && (mbi->flags & MULTIBOOT_FLAG_CMD) &&
      cmdline_has((const char *)(uintptr_t)mbi->cmdline, "verbose"))
    log_set_verbose(1);

  con_set_color(CON_LIGHT_CYAN, CON_BLACK);
  con_write("OwOS\n");
  con_set_color(CON_LIGHT_GREY, CON_BLACK);
  con_write("booted in 32-bit protected mode via multiboot.\n");

  if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
    mbi = multiboot2_to_info((uintptr_t)mbi);
  } else if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
    con_set_color(CON_WHITE, CON_RED);
    con_write("bad multiboot magic -- halting.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }

  gdt_init();
  idt_init();
  irq_init();

  con_printf("mmdbg: path=%s mmapflag=%u entries=%u firmware=%s\n",
             magic == MULTIBOOT2_BOOTLOADER_MAGIC ? "mb2" : "mb1", (mbi->flags & MULTIBOOT_FLAG_MMAP) ? 1 : 0,
             mbi->mmap_length / 24, /* synth/E820 entries are 24 bytes  */
             firmware_name());

  pmm_init(mbi);
  con_printf("pmm: %u MiB RAM detected", pmm_installed_kib() / 1024);
  if (pmm_installed_kib() > pmm_total_kib()) {
    /* 32-bit, no PAE: only the low 4 GiB is usable. */
    con_printf(" (%u MiB usable)", pmm_total_kib() / 1024);
  }
  con_write(".\n");

  paging_init();
  con_write("paging: enabled (low 16 MiB identity-mapped).\n");

  heap_init();
  con_write("heap: ready.\n");

  entropy_init();

  smbios_init();

  pci_init();
  con_printf("pci: %u devices found.\n", (uint32_t)pci_count());

  smp_init();

  /* Framebuffer: VMware SVGA, virtio-gpu, else the bootloader's, else QEMU DISPI. */
  {
    int rc = -1, adopted = 0, svga = 0, vgpu = 0;
    rc = vmsvga_init(1024, 768);
    svga = (rc == 0);
    if (rc != 0) {
      rc = virtiogpu_init(1024, 768);
      vgpu = (rc == 0);
    }
    if (rc != 0 && (mbi->flags & MULTIBOOT_FLAG_FB) && mbi->framebuffer_type == MULTIBOOT_FB_TYPE_RGB &&
        mbi->framebuffer_bpp == 32) {
      rc = fb_adopt((uint32_t)mbi->framebuffer_addr, mbi->framebuffer_width, mbi->framebuffer_height,
                    mbi->framebuffer_pitch, mbi->framebuffer_bpp);
      adopted = (rc == 0);
    }
    if (rc != 0)
      rc = fb_init(1024, 768);

    /* Replay the pre-graphics boot log onto the framebuffer. */
    if (rc == 0 && log_verbose())
      journal_to_fb();

    if (adopted) {
      con_printf("fb: adopted bootloader framebuffer %ux%ux32.\n", mbi->framebuffer_width, mbi->framebuffer_height);
    }
    if (svga)
      con_write("fb: VMware SVGA II adopted.\n");
    if (vgpu)
      con_write("fb: virtio-gpu adopted.\n");
    if (rc == 0)
      con_write("fb: graphics mode up.\n");
    else
      con_write("fb: no framebuffer available; staying in text mode.\n");
  }

  net_init();

  ahci_init();
  nvme_init();
  sd_init();
  atapi_init();

  blk_init();
  fs_init();
  /* Removable volumes are not auto-mounted (mount command or Files app). */

  syscall_init();

  /* Install the embedded programs onto the disk if absent. */
  {
    mkdir_mode("/bin", 0755);
    mkdir_mode("/tmp", 01777);
    mkdir_mode("/home", 0755);
    mkdir_mode("/home/root", 0700);
    mkdir_mode("/home/root/Downloads", 0755);

    /* Seed passwd (name:pwhash:uid:gid:home) and group if absent. */
    mkdir_mode("/etc", 0755);
    mkdir_mode("/etc/fonts", 0755);
    INSTALL_FILE("/etc/fonts/LiberationSans-Regular.ttf", _binary_fonts_LiberationSans_Regular_ttf, 0644);
    INSTALL_FILE("/etc/fonts/LiberationMono-Regular.ttf", _binary_fonts_LiberationMono_Regular_ttf, 0644);
    mkdir_mode("/usr", 0755);
    mkdir_mode("/usr/share", 0755);
    mkdir_mode("/usr/share/applications", 0755);
    INSTALL_DESKTOP(term);
    INSTALL_DESKTOP(files);
    INSTALL_DESKTOP(editor);
    INSTALL_DESKTOP(browser);
    INSTALL_DESKTOP(player);
    INSTALL_DESKTOP(clock);
    INSTALL_DESKTOP(calc);
    INSTALL_DESKTOP(notifyd);
    INSTALL_DESKTOP(settings);
    INSTALL_DESKTOP(about);
    seed_file("/etc/passwd", "root::0:0:/home/root\n", 21, 0600);
    seed_file("/etc/group", "root:0:\n", 8, 0644);
    /* mdnsd advertisements, one "type port [txt...]" per line. */
    seed_file("/etc/mdns.conf", "_ssh._tcp 22\n_smb._tcp 445\n", 26, 0644);
    /* init's service table, one /bin daemon per line; compositor first. */
    seed_file("/etc/inittab", "compositor\ndhcpcd\nmdnsd\nsshd\nntpd\nmediad\n", 41, 0644);
    inittab_ensure("ntpd");
    inittab_ensure("mediad");
    users_init();
    INSTALL_APP(edit);
    INSTALL_APP(curl);
    INSTALL_APP(editor);
    INSTALL_APP(about);
    INSTALL_APP(clock);
    INSTALL_APP(calc);
    INSTALL_APP(files);
    INSTALL_APP(browser);
    INSTALL_APP(imgview);
    INSTALL_APP(term);
    INSTALL_APP(compositor);
    INSTALL_APP(neofetch);
    INSTALL_APP(lspci);
    INSTALL_APP(uname);
    INSTALL_APP(ping);
    INSTALL_APP(dig);
    INSTALL_APP(stun);
    INSTALL_APP(torrent);
    INSTALL_APP(player);
    INSTALL_APP(settings);
    INSTALL_APP(ssh);
    INSTALL_APP(sshd);
    INSTALL_APP(clipd);
    INSTALL_APP(vmclipd);
    INSTALL_APP(cliphist);
    INSTALL_APP(notifyd);
    INSTALL_APP(webp);
    INSTALL_APP(json);
    INSTALL_APP(base64);
    INSTALL_APP(alac);
    INSTALL_APP(aac);
    INSTALL_APP(mpeg);
    INSTALL_APP(h264);
    INSTALL_APP(smbd);
    INSTALL_APP(dhcpcd);
    INSTALL_APP(ntpd);
    INSTALL_APP(mdnsd);
    INSTALL_APP(mediad);
    INSTALL_APP(mediactl);
    INSTALL_APP(cast);
    INSTALL_APP(chmod);
    INSTALL_APP(unrar);
    INSTALL_APP(chown);
    INSTALL_APP(init);
    INSTALL_APP(sh);
    INSTALL_APP(sudo);
    INSTALL_FILE("/bin/notify-send", _binary_app_notifysend, 0755);
  }
  con_write("usermode: ready ('run curl <url>').\n");

  timer_init();
  wallclock_init();
  keyboard_init();
  sb16_init();
  hda_init();

  sched_init();
  net_start();
  sched_create_thread("spin-a", spinner);
  sched_create_thread("spin-b", spinner);
  con_write("scheduler: 2 worker threads spawned.\n");

  /* PS/2 mouse: seed pointer bounds from the framebuffer until the compositor sets them. */
  if (fb_active())
    mouse_init((int)fb_width() - 1, (int)fb_height() - 1);

  bootmark("sti");
  __asm__ volatile("sti");

  smp_start_aps();

  sched_create_thread("bootup", boot_bringup);

  bootmark("shell");
  shell_init();

  for (;;) {
    char c;
    unsigned char mods;
    /* The kernel drains the keyboard only while no compositor owns it (recovery console). */
    if (!uwin_compositor_active()) {
      if (keyboard_take_interrupt())
        shell_interrupt_locked();
      while (keyboard_poll_text(&c, &mods))
        shell_feed_locked(c);
    }
    while (serial_poll(&c))
      shell_feed_locked(c);
    __asm__ volatile("hlt");
  }
}
