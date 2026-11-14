/* userland.c: see userland.h. */
#include "userland.h"
#include "fs.h"
#include "heap.h"
#include "install.h"
#include "irqflags.h"
#include "kstring.h"
#include "loader.h"
#include "sched.h"
#include "shell.h"
#include "users.h"

/* Create a directory only if absent, with an explicit mode (the boot session runs as root). */
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

/* Whether the service table lists `daemon` on a line of its own. */
static bool inittab_lists(const char *inittab, const char *daemon)
{
  uint32_t dl = (uint32_t)strlen(daemon);
  for (const char *p = inittab; *p; p++)
    if ((p == inittab || p[-1] == '\n') && memcmp(p, daemon, dl) == 0 && (p[dl] == '\n' || p[dl] == '\0'))
      return true;
  return false;
}

void userland_prepare(const char *inittab)
{
  mkdir_mode("/bin", 0755);
  mkdir_mode("/home", 0755);
  mkdir_mode("/home/root", 0700);
  mkdir_mode("/home/root/Downloads", 0755);
  mkdir_mode("/var", 0755);
  mkdir_mode("/var/lib", 0755);
  mkdir_mode("/etc", 0755);
  install_payload();

  /* Seed passwd (name:pwhash:uid:gid:home) and group if absent. */
  seed_file("/etc/passwd", "root::0:0:/home/root\n", 21, 0600);
  seed_file("/etc/group", "root:0:\n", 8, 0644);
  /* mdnsd advertisements, one "type port [txt...]" per line. */
  seed_file("/etc/mdns.conf", "_ssh._tcp 22\n_smb._tcp 445\n", 26, 0644);
  seed_file("/etc/inittab", inittab, (uint32_t)strlen(inittab), 0644);
  /* Daemons added after a system was installed reach its existing inittab too. */
  if (inittab_lists(inittab, "ntpd"))
    inittab_ensure("ntpd");
  if (inittab_lists(inittab, "mediad"))
    inittab_ensure("mediad");
  users_init();
}

void userland_start(void)
{
  /* PID 1 runs as root so the daemons it spawns inherit uid 0. */
  int sz = fs_size("/bin/init");
  uint8_t *buf = sz > 0 ? kmalloc((uint32_t)sz) : 0;
  if (!buf)
    return;
  int n = fs_read("/bin/init", buf, (uint32_t)sz);
  char *argv[1] = {"init"};
  if (n > 0)
    exec_program(buf, (uint32_t)n, 1, argv);
  kfree(buf);
}

void console_feed(char c)
{
  uintptr_t flags = irq_save();
  sched_kernel_enter();
  shell_feed(c);
  sched_kernel_leave();
  irq_restore(flags);
}

void console_interrupt(void)
{
  uintptr_t flags = irq_save();
  sched_kernel_enter();
  shell_interrupt();
  sched_kernel_leave();
  irq_restore(flags);
}
