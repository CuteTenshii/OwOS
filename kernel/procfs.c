/* procfs.c — a tiny read-only /proc: kernel introspection through the ordinary
 * file interface, one concern per file, generated on read.
 *
 * This replaces the old grab-bag SYS_SYSINFO / SYS_PCIDEV syscalls:
 * programs (neofetch, lspci) just readfile("/proc/<x>") like any other file,
 * and `cat /proc/uptime` works too. Each file is a single fact, never a bundle.
 *
 *   /proc/cpuinfo   model + MHz          /proc/host      SMBIOS product
 *   /proc/meminfo   used / total KiB     /proc/battery   present/pct/state
 *   /proc/uptime    seconds              /proc/display   framebuffer + EDID
 *   /proc/date      wall clock           /proc/storage   one line per disk
 *   /proc/tasks     scheduler task count /proc/owofs     file/byte usage
 *                                        /proc/pci       one line per PCI
 * device
 *
 * Device attributes that belong to a kernel object (firmware type, NIC link/IP,
 * battery, framebuffer/monitor) live in the /sys tree instead — see sysfs.c.
 */

#include "procfs.h"
#include "ahci.h"
#include "ata.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "kstring.h"
#include "log.h"
#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "power.h"
#include "rtc.h"
#include "sched.h"
#include "smp.h"
#include "syscall_path.h"
#include "timer.h"
#include "usb.h"
#include "users.h"
#include "vdagent.h"
#include "vmware.h"
#include "wallclock.h"
#include "xhci.h"
#include <stdint.h>

/* bounded text builder */
struct pb {
  char *p;
  char *end;
};
static void ps(struct pb *b, const char *s)
{
  while (s && *s && b->p < b->end)
    *b->p++ = *s++;
}
static void pc(struct pb *b, char c)
{
  if (b->p < b->end)
    *b->p++ = c;
}
static void pu(struct pb *b, uint32_t v)
{
  char t[12];
  int n = 0;
  if (!v) {
    pc(b, '0');
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    pc(b, t[--n]);
}
static void p2(struct pb *b, uint32_t v)
{
  pc(b, '0' + (char)(v / 10 % 10));
  pc(b, '0' + (char)(v % 10));
}

/* /proc/cpuinfo */
static void gen_cpuinfo(struct pb *b)
{
  static uint32_t mhz; /* cpu_mhz() busy-waits ~200 ms     */
  char brand[52];
  cpu_brand(brand);
  if (!mhz)
    mhz = cpu_mhz();
  ps(b, "model ");
  ps(b, brand[0] ? brand : "unknown");
  pc(b, '\n');
  ps(b, "mhz ");
  pu(b, mhz);
  pc(b, '\n');
  ps(b, "cpus ");
  pu(b, (uint32_t)smp_cpu_count());
  pc(b, '\n');
}

static void gen_meminfo(struct pb *b)
{
  ps(b, "used ");
  pu(b, pmm_ram_used_kib());
  pc(b, '\n');
  ps(b, "total ");
  pu(b, pmm_installed_kib());
  pc(b, '\n');
}

static void gen_uptime(struct pb *b)
{
  pu(b, timer_seconds());
  pc(b, '\n');
}
static void gen_tasks(struct pb *b)
{
  pu(b, (uint32_t)sched_count());
  pc(b, '\n');
}

static void gen_date(struct pb *b)
{
  struct timeval64 tv;
  struct rtc_time t;
  wallclock_get(&tv);
  epoch_to_rtc((uint32_t)tv.tv_sec, &t);
  pu(b, t.year);
  pc(b, '-');
  p2(b, t.month);
  pc(b, '-');
  p2(b, t.day);
  pc(b, ' ');
  p2(b, t.hour);
  pc(b, ':');
  p2(b, t.min);
  pc(b, ':');
  p2(b, t.sec);
  pc(b, '\n');
}

/* /proc/user: "uid gid name home" of the calling task, readable without /etc/passwd */
static void gen_user(struct pb *b)
{
  char name[USER_NAME_MAX], home[USER_HOME_MAX];
  uint32_t uid = sched_uid();
  users_name_of(uid, name, sizeof name);
  users_home_of(uid, home, sizeof home);
  pu(b, uid);
  pc(b, ' ');
  pu(b, sched_gid());
  pc(b, ' ');
  ps(b, name);
  pc(b, ' ');
  ps(b, home);
  pc(b, '\n');
}

static void gen_owofs(struct pb *b)
{
  ps(b, "files ");
  pu(b, (uint32_t)fs_file_count());
  pc(b, '\n');
  ps(b, "bytes ");
  pu(b, fs_used_bytes());
  pc(b, '\n');
}

/* /proc/storage: one line per present disk -> "<type> <sectors> <model>" */
static void gen_storage(struct pb *b)
{
  if (ata_present()) {
    ps(b, "IDE/ATA ");
    pu(b, ata_sectors());
    pc(b, ' ');
    ps(b, ata_model());
    pc(b, '\n');
  }
  if (ahci_present()) {
    ps(b, "SATA/AHCI ");
    pu(b, ahci_sectors());
    pc(b, ' ');
    ps(b, ahci_model());
    pc(b, '\n');
  }
  if (nvme_present()) {
    ps(b, "NVMe ");
    pu(b, nvme_sectors());
    pc(b, ' ');
    ps(b, nvme_model());
    pc(b, '\n');
  }
}

/* /proc/pci: "bus:slot.fn vendor:device class name" per device (lspci reads it;
 * neofetch filters class 03xx for the GPU list and counts the lines). */
static void hex2(struct pb *b, unsigned v)
{
  static const char h[] = "0123456789abcdef";
  pc(b, h[(v >> 4) & 0xF]);
  pc(b, h[v & 0xF]);
}
static void hex4(struct pb *b, unsigned v)
{
  hex2(b, v >> 8);
  hex2(b, v);
}
static void gen_pci(struct pb *b)
{
  struct pci_dev_info d;
  for (int i = 0; pci_dev_info(i, &d) > 0; i++) {
    hex2(b, d.bus);
    pc(b, ':');
    hex2(b, d.slot);
    pc(b, '.');
    pc(b, "0123456789abcdef"[d.func & 0xF]);
    pc(b, ' ');
    hex4(b, d.vendor);
    pc(b, ':');
    hex4(b, d.device);
    pc(b, ' ');
    hex2(b, d.class_code);
    hex2(b, d.subclass);
    pc(b, ' ');
    ps(b, d.name);
    pc(b, '\n');
  }
}

static void gen_journal(struct pb *b) { b->p += journal_copy(b->p, (int)(b->end - b->p)); }

/* usb only knows how to print to the console, so capture that into the /proc
 * buffer. con_capture is global but procfs_read is called synchronously by the
 * reader, so the window is tiny. */
static void gen_usb(struct pb *b)
{
  con_capture_begin(b->p, (uint32_t)(b->end - b->p));
  usb_list(-1);
  b->p += con_capture_end();
}

/* The entry names, in listing order. Single source of truth for read + list. */
static const char *const entries[] = {
    "cpuinfo", "meminfo", "uptime", "tasks", "date", "owofs", "storage", "pci", "journal", "usb", "user",
};
#define NENTRIES ((int)(sizeof entries / sizeof entries[0]))

/* List the idx-th /proc entry name into `name`; 1 if it exists, 0 past the end.
 */
int procfs_list(int idx, char *name, int max)
{
  if (idx < 0 || idx >= NENTRIES)
    return 0;
  const char *s = entries[idx];
  int i = 0;
  for (; s[i] && i < max - 1; i++)
    name[i] = s[i];
  name[i] = '\0';
  return 1;
}

/* Byte length a /proc file would produce (generate into scratch + measure), so
 * stat/ls report a real size and the files behave like ordinary ones. */
int procfs_size(const char *sub)
{
  static char scratch[8192];
  const char *leaf = sub[0] == '/' ? sub + 1 : sub;
  if (streq(leaf, "hostclip") || streq(leaf, "login") || streq(leaf, "sudo"))
    return 0; /* control nodes: a read would consume host state */
  int n = procfs_read(sub, scratch, (int)sizeof scratch);
  return n < 0 ? 0 : n;
}

/* 1 if `sub` ("" or "/name") names the /proc dir, 0 a known file, -1 unknown.
 */
int procfs_kind(const char *sub)
{
  if (sub[0] == '/')
    sub++;
  if (!sub[0])
    return 1; /* /proc itself                      */
  if (streq(sub, "hostclip") || streq(sub, "login") || streq(sub, "sudo"))
    return 0; /* hidden VMware-clipboard device node */
  for (int i = 0; i < NENTRIES; i++)
    if (streq(sub, entries[i]))
      return 0;
  return -1;
}

/* Path is the part after "/proc" (e.g. "/cpuinfo"). Returns bytes written, or
 * -1 for an unknown entry. */
int procfs_read(const char *sub, char *buf, int max)
{
  /* Some generators wait on the timer (cpu_mhz calibration) or ACPI; the
   * int 0x80 gate left IF=0, so enable interrupts or they'd spin forever. */
  __asm__ volatile("sti");
  if (sub[0] == '/')
    sub++;

  /* /proc/hostclip — the VMware or QEMU host clipboard as a device channel (not a
   * listed file). A read returns the newest host selection text, 0 bytes when
   * nothing is new, or -1 off VMware/QEMU so /bin/vmclipd knows to bow out. The
   * backdoor GET is stateful (a success consumes the host's "new selection"
   * flag), so it is meant to be polled by vmclipd alone — hence unlisted, to
   * keep `ls /proc` off it. */
  if (streq(sub, "hostclip")) {
    if (vdagent_present())
      return vdagent_clip_recv(buf, max);
    if (!vmware_present())
      return -1;
    int n = vmware_clip_recv(buf, max);
    return n > 0 ? n : 0;
  }

  struct pb b = {buf, buf + max};

  if (streq(sub, "cpuinfo"))
    gen_cpuinfo(&b);
  else if (streq(sub, "meminfo"))
    gen_meminfo(&b);
  else if (streq(sub, "uptime"))
    gen_uptime(&b);
  else if (streq(sub, "tasks"))
    gen_tasks(&b);
  else if (streq(sub, "date"))
    gen_date(&b);
  else if (streq(sub, "owofs"))
    gen_owofs(&b);
  else if (streq(sub, "storage"))
    gen_storage(&b);
  else if (streq(sub, "pci"))
    gen_pci(&b);
  else if (streq(sub, "journal"))
    gen_journal(&b);
  else if (streq(sub, "usb"))
    gen_usb(&b);
  else if (streq(sub, "user"))
    gen_user(&b);
  else
    return -1;

  return (int)(b.p - buf);
}

/* The write side of /proc: control nodes through the ordinary file interface,
 * so the shell triggers kernel actions with writefile() and never needs a
 * per-action syscall or a "run a kernel command" bridge. Returns len on success
 * (so writefile reports the bytes written), -1 for an unknown/!writable node.
 */
int procfs_write(const char *sub, const char *buf, int len)
{
  __asm__ volatile("sti"); /* dhcp/ntp wait on the timer/IRQs   */
  if (sub[0] == '/')
    sub++;

  /* /proc/hostclip write pushes guest text up to the VMware host clipboard. Use
   * the raw payload (a clip is arbitrary text, may exceed the trimmed arg[]).
   */
  if (streq(sub, "hostclip"))
    return (vdagent_present() ? vdagent_clip_send(buf, len) : vmware_clip_send(buf, len)) < 0 ? -1 : len;

  /* Trim a trailing newline and bound the payload into a NUL-terminated copy.
   */
  char arg[256];
  int n = len;
  if (n > (int)sizeof arg - 1)
    n = (int)sizeof arg - 1;
  for (int i = 0; i < n; i++)
    arg[i] = buf[i];
  arg[n] = '\0';
  while (n > 0 && (arg[n - 1] == '\n' || arg[n - 1] == '\r'))
    arg[--n] = '\0';

  /* Switch the session: payload is "name" or "name password" (one space). The
   * kernel hashes the plaintext and checks it against /etc/passwd, so this is
   * the one trusted step user space can't fake. Returns len on success; -1 if
   * the account is unknown or the password is wrong (writefile -> error). */
  if (streq(sub, "login")) {
    char *pass = arg;
    while (*pass && *pass != ' ')
      pass++;
    if (*pass == ' ')
      *pass++ = '\0'; /* split name from password       */
    else
      pass = (char *)""; /* no password supplied           */
    return users_login(arg, pass) == 0 ? len : -1;
  }

  /* sudo, for the calling uid. First byte: 'q' may this user sudo; '?' is a grant running; 'k' end it; 'p' + the
   * user's own password starts one. Returns len on yes/success, -1 on no. */
  if (streq(sub, "sudo") && n > 0) {
    uint32_t uid = sched_uid();
    switch (arg[0]) {
    case 'q':
      return users_sudo_allowed(uid) ? len : -1;
    case '?':
      return users_sudo_valid(uid) ? len : -1;
    case 'k':
      users_sudo_revoke(uid);
      return len;
    case 'p':
      return users_sudo_grant(uid, arg + 1) == 0 ? len : -1;
    }
  }
  return -1;
}

/* The part of `path` after a leading "/proc", or NULL if it isn't under /proc.
 * "/proc" -> "", "/proc/cpuinfo" -> "/cpuinfo". */
const char *procfs_subpath(const char *path)
{
  if (!path)
    return 0;
  while (*path == '/')
    path++;
  const char *q = "proc";
  while (*q && *path == *q) {
    path++;
    q++;
  }
  if (*q)
    return 0; /* didn't match "proc"               */
  if (*path && *path != '/')
    return 0;  /* "procfoo" is not /proc            */
  return path; /* "" or "/cpuinfo"                  */
}
