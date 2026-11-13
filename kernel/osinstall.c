/* osinstall.c: write a bootable OwOS disk. A GPT holds a BIOS boot partition with GRUB's core image, an ESP with
 * GRUB for UEFI, its config and the kernel, and an empty owofs root that the installed kernel fills on its first
 * boot, as it fills any fresh disk. GRUB's boot sector goes in last, so an interrupted install never boots. */
#include "osinstall.h"
#include "blk.h"
#include "bootmod.h"
#include "entropy.h"
#include "fat.h"
#include "fs.h"
#include "install.h"
#include "kstring.h"
#include "part.h"

#define BIOS_START 2048u
#define BIOS_SECTORS 2048u
#define ESP_START (BIOS_START + BIOS_SECTORS)
#define ESP_SECTORS 262144u
#define ROOT_START (ESP_START + ESP_SECTORS)
#define KERNEL_CHUNK (512u * 1024u)

/* Where grub-install patches GRUB's i386-pc images: the core image's sector in boot.img, the boot-drive check
 * replaced by NOPs for hard disks, and the list of the core image's remaining sectors in its first sector. */
#define BOOT_KERNEL_SECTOR 0x5Cu
#define BOOT_DRIVE_CHECK 0x66u
#define CORE_BLOCKLIST_START 0x1F4u
#define MBR_CODE_END 0x1B8u /* the disk signature and partition table follow */

#define GRUB_BOOT_IMG "/usr/lib/grub/boot.img"
#define GRUB_CORE_IMG "/usr/lib/grub/core.img"
#define GRUB_EFI64 "/usr/lib/grub/bootx64.efi"
#define GRUB_EFI32 "/usr/lib/grub/bootia32.efi"

/* The early config built into GRUB's images finds the ESP by owos.id, then reads this. */
static const char grub_cfg[] = "set timeout=0\nset default=0\ninsmod all_video\n"
                               "set gfxmode=1920x1080x32,1366x768x32,1280x1024x32,1024x768x32,800x600x32,auto\n"
                               "menuentry \"OwOS\" {\n\tmultiboot2 /boot/owos.bin verbose\n\tboot\n}\n";
static const char owos_id[] = "OwOS ESP\n";

enum stage { IDLE, TABLE, ESP_FORMAT, ESP_FILES, KERNEL, ROOT, BOOT_CODE, DONE, FAILED, NSTAGES };
static const char *const stage_name[NSTAGES] = {
    "",
    "Partitioning the disk",
    "Formatting the boot partition",
    "Copying the bootloader",
    "Copying the kernel",
    "Preparing the system partition",
    "Making the disk bootable",
    "Done",
    "Failed",
};

static struct {
  enum stage stage, failed_at;
  blk_read_fn read;
  blk_write_fn write;
  uint32_t sectors;
  const uint8_t *kernel;
  uint32_t kernel_len, kernel_done, units_done;
  void *esp;
} job;

static void put64(uint8_t *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (8u * (uint32_t)i));
}

static uint32_t file_len(const struct payload_file *f) { return (uint32_t)(f->end - f->start); }

static bool images_present(void)
{
  uint32_t len;
  return bootmod_find("owos-kernel", &len) && install_payload_find(GRUB_BOOT_IMG) &&
         install_payload_find(GRUB_CORE_IMG) && install_payload_find(GRUB_EFI64) && install_payload_find(GRUB_EFI32);
}

static enum install_problem disk_problem(const struct blk_devinfo *d)
{
  blk_read_fn read;
  blk_write_fn write;
  int iso;
  if (blk_dev_io(d->dev, &read, &write, &iso) != 0)
    return INSTALL_NO_DEVICE;
  if (iso || !write)
    return INSTALL_READ_ONLY;
  if (d->sectors < INSTALL_MIN_SECTORS)
    return INSTALL_TOO_SMALL;
  /* An ISO9660 primary volume descriptor at byte 32 KiB: a live USB stick or a written ISO. */
  uint8_t sec[512];
  if (read(64, sec) == 0 && sec[0] == 1 && memcmp(sec + 1, "CD001", 5) == 0)
    return INSTALL_LIVE_MEDIUM;
  if (!images_present())
    return INSTALL_NO_IMAGE;
  return INSTALL_OK;
}

int osinstall_disk(int index, struct install_disk *out)
{
  struct blk_devinfo devs[16];
  int n = blk_dev_list(devs, 16);
  if (index < 0 || index >= n)
    return 0;
  memset(out, 0, sizeof *out);
  strlcpy(out->dev, devs[index].dev, sizeof out->dev);
  strlcpy(out->model, devs[index].model ? devs[index].model : "", sizeof out->model);
  out->sectors = devs[index].sectors;
  out->problem = disk_problem(&devs[index]);
  return 1;
}

static bool running(void) { return job.stage != IDLE && job.stage != DONE && job.stage != FAILED; }

enum install_problem osinstall_start(const char *dev)
{
  if (running())
    return INSTALL_BUSY;
  struct blk_devinfo devs[16];
  int n = blk_dev_list(devs, 16);
  for (int i = 0; i < n; i++) {
    if (!streq(devs[i].dev, dev))
      continue;
    enum install_problem p = disk_problem(&devs[i]);
    if (p != INSTALL_OK)
      return p;
    memset(&job, 0, sizeof job);
    blk_dev_io(dev, &job.read, &job.write, 0);
    job.sectors = devs[i].sectors;
    job.kernel = bootmod_find("owos-kernel", &job.kernel_len);
    job.stage = TABLE;
    return INSTALL_OK;
  }
  return INSTALL_NO_DEVICE;
}

static bool write_table(void)
{
  uint8_t random[64];
  entropy_get(random, sizeof random);
  struct part_spec parts[] = {
      {PART_TYPE_BIOS_BOOT, BIOS_START, BIOS_SECTORS, "BIOS boot"},
      {PART_TYPE_ESP, ESP_START, ESP_SECTORS, "EFI system"},
      {PART_TYPE_OWOS_ROOT, ROOT_START, job.sectors - PART_GPT_TAIL - ROOT_START, "OwOS"},
  };
  return part_write_gpt(job.write, job.sectors, parts, 3, random) == 0;
}

static bool format_esp(void)
{
  struct fat_format_opts o = {.write = job.write,
                              .base_lba = ESP_START,
                              .sectors = ESP_SECTORS,
                              .kind = 32,
                              .bytes_per_sec = 512,
                              .label = "OWOS"};
  return fat_format(&o) == 0;
}

static bool put_file(const char *path, const void *data, uint32_t len)
{
  return fat_ops()->create(job.esp, path, data, len) == 0;
}

static bool copy_bootloader(void)
{
  const struct fs_ops *fat = fat_ops();
  job.esp = fat->probe(job.read, job.write);
  if (!job.esp)
    return false;
  static const char *const dirs[] = {"/EFI", "/EFI/BOOT", "/boot", "/boot/grub"};
  for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    if (fat->mkdir(job.esp, dirs[i]) != 0)
      return false;
  const struct payload_file *e64 = install_payload_find(GRUB_EFI64), *e32 = install_payload_find(GRUB_EFI32);
  return put_file("/EFI/BOOT/BOOTX64.EFI", e64->start, file_len(e64)) &&
         put_file("/EFI/BOOT/BOOTIA32.EFI", e32->start, file_len(e32)) &&
         put_file("/boot/grub/grub.cfg", grub_cfg, sizeof grub_cfg - 1) &&
         put_file("/boot/grub/owos.id", owos_id, sizeof owos_id - 1);
}

/* One chunk of the kernel image; true once all of it is on the ESP. */
static bool copy_kernel_chunk(bool *finished)
{
  uint32_t n = job.kernel_len - job.kernel_done;
  if (n > KERNEL_CHUNK)
    n = KERNEL_CHUNK;
  const uint8_t *p = job.kernel + job.kernel_done;
  bool ok = job.kernel_done == 0 ? put_file("/boot/owos.bin", p, n)
                                 : fat_ops()->write_at(job.esp, "/boot/owos.bin", job.kernel_done, p, n) == (int)n;
  job.kernel_done += n;
  *finished = job.kernel_done == job.kernel_len;
  return ok;
}

static bool write_boot_code(void)
{
  const struct payload_file *core = install_payload_find(GRUB_CORE_IMG), *boot = install_payload_find(GRUB_BOOT_IMG);
  uint32_t len = file_len(core), n = (len + 511) / 512;
  if (n > BIOS_SECTORS || file_len(boot) < 512)
    return false;
  uint8_t sec[512];
  for (uint32_t i = 0; i < n; i++) {
    uint32_t take = len - i * 512 < 512 ? len - i * 512 : 512;
    memset(sec, 0, sizeof sec);
    memcpy(sec, core->start + i * 512, take);
    if (i == 0)
      put64(sec + CORE_BLOCKLIST_START, BIOS_START + 1);
    if (job.write(BIOS_START + i, sec) != 0)
      return false;
  }
  if (job.read(0, sec) != 0)
    return false;
  memcpy(sec, boot->start, MBR_CODE_END);
  put64(sec + BOOT_KERNEL_SECTOR, BIOS_START);
  sec[BOOT_DRIVE_CHECK] = sec[BOOT_DRIVE_CHECK + 1] = 0x90;
  return job.write(0, sec) == 0;
}

int osinstall_step(struct install_progress *out)
{
  uint32_t kernel_units = (job.kernel_len + KERNEL_CHUNK - 1) / KERNEL_CHUNK;
  if (running()) {
    bool ok = true, next = true;
    switch (job.stage) {
    case TABLE:
      ok = write_table();
      break;
    case ESP_FORMAT:
      ok = format_esp();
      break;
    case ESP_FILES:
      ok = copy_bootloader();
      break;
    case KERNEL:
      ok = copy_kernel_chunk(&next);
      break;
    case ROOT:
      ok = fs_format_at(job.write, ROOT_START, job.sectors - PART_GPT_TAIL - ROOT_START) == 0;
      break;
    case BOOT_CODE:
      ok = write_boot_code();
      break;
    default:
      break;
    }
    job.units_done++;
    if (!ok) {
      job.failed_at = job.stage;
      job.stage = FAILED;
    } else if (next)
      job.stage = (enum stage)(job.stage + 1);
    if ((job.stage == DONE || job.stage == FAILED) && job.esp) {
      fat_ops()->close(job.esp);
      job.esp = 0;
    }
  }
  memset(out, 0, sizeof *out);
  out->total = 5 + kernel_units;
  out->done = job.stage == DONE ? out->total : job.units_done;
  enum stage shown = job.stage == FAILED ? job.failed_at : job.stage;
  strlcpy(out->stage, shown < NSTAGES ? stage_name[shown] : "", sizeof out->stage);
  return job.stage == DONE ? 1 : job.stage == FAILED || job.stage == IDLE ? -1 : 0;
}
