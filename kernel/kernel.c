/* kernel.c: OwOS entry point (called from boot.S); brings up subsystems in order, then runs the shell. */

#include <stdint.h>

#include "ahci.h"
#include "atapi.h"
#include "backlight.h"
#include "blk.h"
#include "bootmod.h"
#include "console.h"
#include "entropy.h"
#include "es1371.h"
#include "ext.h"
#include "fat.h"
#include "fb.h"
#include "firmware.h"
#include "fpu.h"
#include "fs.h"
#include "gdt.h"
#include "hda.h"
#include "heap.h"
#include "hfs.h"
#include "idt.h"
#include "install.h"
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
#include "scsi.h"
#include "sd.h"
#include "serial.h"
#include "shell.h"
#include "smbios.h"
#include "smp.h"
#include "syscall.h"
#include "syscall_graphics.h"
#include "timer.h"
#include "ufs.h"
#include "usb.h"
#include "userland.h"
#include "users.h"
#include "virtiogpu.h"
#include "virtioserial.h"
#include "vmsvga.h"
#include "vmtools.h"
#include "wallclock.h"

/* Boot-stage breadcrumb printed by the panic screen (idt.c). */
const char *g_boot_stage = "early";
static void bootmark(const char *s) { g_boot_stage = s; }

/* Slow post-sti bring-up in a background thread so the shell loop starts at once. */
static void boot_bringup(void)
{
  userland_start();

  bootmark("usb");
  usb_boot();

  bootmark("vmtools");
  vmtools_init(); /* no-op off VMware */

  bootmark("vserial");
  vserial_init(); /* no-op without virtio-serial */

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

    if (tag->type == MB2_TAG_MODULE) {
      struct mb2_tag_module *mod = (struct mb2_tag_module *)p;
      bootmod_add(mod->mod_start, mod->mod_end, mod->string);
    } else if (tag->type == MB2_TAG_CMDLINE) {
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
  } else if (magic == MULTIBOOT_BOOTLOADER_MAGIC && (mbi->flags & MULTIBOOT_FLAG_MODS)) {
    const struct multiboot_mod_list *ml = (const struct multiboot_mod_list *)(uintptr_t)mbi->mods_addr;
    for (uint32_t i = 0; i < mbi->mods_count; i++)
      bootmod_add(ml[i].mod_start, ml[i].mod_end, (const char *)(uintptr_t)ml[i].cmdline);
  } else if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
    con_set_color(CON_WHITE, CON_RED);
    con_write("bad multiboot magic -- halting.\n");
    for (;;)
      __asm__ volatile("cli; hlt");
  }

  gdt_init();
  idt_init();
  irq_init();
  fpu_cpu_init();

  con_printf("mmdbg: path=%s mmapflag=%u entries=%u firmware=%s\n",
             magic == MULTIBOOT2_BOOTLOADER_MAGIC ? "mb2" : "mb1", (mbi->flags & MULTIBOOT_FLAG_MMAP) ? 1 : 0,
             mbi->mmap_length / 24, /* synth/E820 entries are 24 bytes  */
             firmware_name());

  pmm_init(mbi);
  bootmod_reserve();
  con_printf("pmm: %u MiB RAM detected", pmm_installed_kib() / 1024);
  if (pmm_installed_kib() > pmm_total_kib()) {
    /* 32-bit, no PAE: only the low 4 GiB is usable. */
    con_printf(" (%u MiB usable)", pmm_total_kib() / 1024);
  }
  con_write(".\n");

  paging_init();
  con_write("paging: enabled (low 16 MiB identity-mapped).\n");
  bootmod_relocate();

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

  if (backlight_init())
    con_printf("backlight: Intel PWM, period %u, level %u/100.\n", backlight_max(), (uint32_t)backlight_percent());

  net_init();

  ahci_init();
  nvme_init();
  sd_init();
  scsi_init();
  ufs_init();
  atapi_init();

  blk_init();
  /* Live owofs adds per-file cluster rounding and its directory on top of the payload itself. */
  fs_init(install_payload_bytes() / 512u * 9u / 8u + 8u * 2048u);
  /* Removable volumes are not auto-mounted (mount command or Files app). */

  syscall_init();

  /* init's service table, one /bin daemon per line; compositor first. */
  userland_prepare("compositor\ndhcpcd\nmdnsd\nsshd\nntpd\nmediad\n");
  con_write("usermode: ready ('curl <url>').\n");

  timer_init();
  wallclock_init();
  keyboard_init();
  sb16_init();
  hda_init();
  es1371_init();

  sched_init();
  net_start();

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
    if (!graphics_fb_owned()) {
      if (keyboard_take_interrupt())
        console_interrupt();
      while (keyboard_poll_text(&c, &mods))
        console_feed(c);
    }
    while (serial_poll(&c))
      console_feed(c);
    __asm__ volatile("hlt");
  }
}
