/* syscall.c: dispatch user system calls. ABI: eax = number, ebx/ecx/edx = args, result in eax. */

#include "syscall.h"
#include "ahci.h"
#include "ata.h"
#include "battery.h"
#include "blk.h"
#include "console.h"
#include "cpu.h"
#include "crash.h"
#include "edid.h"
#include "entropy.h"
#include "ext.h"
#include "fat.h"
#include "fb.h"
#include "file.h"
#include "firmware.h"
#include "fs.h"
#include "gdt.h"
#include "hda.h"
#include "heap.h"
#include "hfs.h"
#include "idt.h"
#include "ipc.h"
#include "iso9660.h"
#include "keyboard.h"
#include "kstring.h"
#include "loader.h"
#include "mouse.h"
#include "net.h"
#include "nvme.h"
#include "paging.h"
#include "pci.h"
#include "perm.h"
#include "pipe.h"
#include "pmm.h"
#include "power.h"
#include "procfs.h"
#include "rtc.h"
#include "sb16.h"
#include "sched.h"
#include "shell.h"
#include "smbios.h"
#include "syscall_audio.h"
#include "syscall_entropy.h"
#include "syscall_env.h"
#include "syscall_exec.h"
#include "syscall_fs.h"
#include "syscall_graphics.h"
#include "syscall_io.h"
#include "syscall_net.h"
#include "syscall_nr.h"
#include "syscall_path.h"
#include "syscall_process.h"
#include "syscall_signal.h"
#include "syscall_window.h"
#include "sysfs.h"
#include "timer.h"
#include "uaccess.h"
#include "uwin.h"
#include <stdint.h>

/* Build stamp for SYS_UNAME; the Makefile passes it, this is the fallback. */
#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

/* Net transports are sockets: SYS_NET_CONNECT's `proto` selects TCP/TLS/UDP/raw-ICMP. */

/* Make a user-supplied name an absolute owofs path; relative names join the caller's cwd. */
int path_join(const char *base, const char *name, char *out, int outsz)
{
  if (outsz < 2)
    return -1;
  const char *parts[2] = {name[0] == '/' ? "" : base, name};
  int n = 0; /* out[0..n) holds "/a/b"; empty means the root */
  for (int k = 0; k < 2; k++) {
    for (const char *p = parts[k]; *p;) {
      while (*p == '/')
        p++;
      if (!*p)
        break;
      const char *e = p;
      while (*e && *e != '/')
        e++;
      int len = (int)(e - p);
      if (len == 2 && p[0] == '.' && p[1] == '.') {
        while (n > 0 && out[n - 1] != '/')
          n--;
        if (n > 0)
          n--;
      } else if (len != 1 || p[0] != '.') {
        if (n + 1 + len >= outsz) {
          out[0] = '\0';
          return -1;
        }
        out[n++] = '/';
        for (int i = 0; i < len; i++)
          out[n++] = p[i];
      }
      p = e;
    }
  }
  if (n == 0)
    out[n++] = '/';
  out[n] = '\0';
  return 0;
}

int join_cwd(const char *name, char *out, int outsz) { return path_join(sched_cwd(), name, out, outsz); }

/* Read a file by `name` (cwd-relative), falling back to /bin/<name> like the shell's PATH lookup. */
int read_with_path(const char *name, void *buf, uint32_t max)
{
  char abs[SYS_PATH_MAX];
  if (join_cwd(name, abs, sizeof abs) != 0)
    return -1;
  int rc = fs_access(abs, FS_R);
  if (rc == -PERM_EACCES)
    return rc;
  int n = rc == 0 ? fs_read(abs, buf, max) : -1;
  if (n < 0 && name[0] != '/') {
    char p[SYS_PATH_MAX];
    if (path_join("/bin", name, p, sizeof p) != 0)
      return -1;
    rc = fs_access(p, FS_R);
    if (rc == -PERM_EACCES)
      return rc;
    n = rc == 0 ? fs_read(p, buf, max) : -1;
  }
  return n;
}

/* Resolve `name` (cwd-relative, then /bin) to an existing absolute owofs path in `out`; 0 / -1. */
int resolve_path(const char *name, char *out, int outcap)
{
  uint32_t sz;
  if (join_cwd(name, out, outcap) != 0)
    return -1;
  if (fs_stat(out, 0, &sz, 0) == 0)
    return 0;
  if (name[0] == '/' || path_join("/bin", name, out, outcap) != 0)
    return -1;
  return fs_stat(out, 0, &sz, 0) == 0 ? 0 : -1;
}

/* pread: read up to `len` bytes at `off`; short count clamped to file size, 0 = EOF. */
int read_at_with_path(const char *name, uint32_t off, void *buf, uint32_t len)
{
  char abs[SYS_PATH_MAX];
  uint32_t size = 0;
  if (resolve_path(name, abs, sizeof abs) != 0)
    return -1;
  int rc = fs_access(abs, FS_R);
  if (rc != 0)
    return rc;
  if (fs_stat(abs, 0, &size, 0) != 0)
    return -1; /* missing/unstattable: not EOF, an error (fs_stat leaves *size untouched on failure, reading it was a UB
                  bug that hung on tmpfs where the garbage was big) */
  if (off >= size)
    return 0; /* EOF */
  if (len > size - off)
    len = size - off;
  return fs_read_at(abs, off, buf, len);
}

/* A user pointer must lie entirely within the user region, else refuse the call. */
extern void isr128(void); /* the int 0x80 stub (isr.S) */

/* Unix seconds (UTC) to civil date/time (Hinnant's civil-from-days), for SYS_SETRTC. */
void epoch_to_rtc(uint32_t secs, struct rtc_time *t)
{
  uint32_t days = secs / 86400u, rem = secs % 86400u;
  t->hour = (uint8_t)(rem / 3600u);
  t->min = (uint8_t)((rem % 3600u) / 60u);
  t->sec = (uint8_t)(rem % 60u);

  uint32_t z = days + 719468u; /* shift epoch to 0000-03-01 */
  uint32_t era = z / 146097u;
  uint32_t doe = z - era * 146097u; /* [0, 146096] */
  uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  uint32_t y = yoe + era * 400u;
  uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); /* [0, 365] */
  uint32_t mp = (5u * doy + 2u) / 153u;                      /* [0, 11] */
  uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;             /* [1, 31] */
  uint32_t m = mp < 10u ? mp + 3u : mp - 9u;                 /* [1, 12] */

  t->day = (uint8_t)d;
  t->month = (uint8_t)m;
  t->year = (uint16_t)(y + (m <= 2u ? 1u : 0u));
}

void syscall_init(void)
{
  /* 0xEE = present, DPL 3, 32-bit interrupt gate -- ring 3 may invoke it. */
  idt_set_gate(0x80, (uintptr_t)isr128, GDT_KERNEL_CODE, 0xEE);
}

typedef void (*syscall_handler_t)(struct registers *r);

/* Sized by its highest designated index, so adding a syscall is one line here. */
static const syscall_handler_t syscall_handlers[] = {
    [SYS_WRITE] = syscall_io_write,
    [SYS_EXIT] = syscall_process_exit,
    [SYS_READ] = syscall_io_read,
    [SYS_FORK] = syscall_process_fork,
    [SYS_READFILE] = syscall_fs_readfile,
    [SYS_EXEC] = syscall_exec_replace,
    [SYS_WAITPID] = syscall_process_waitpid,
    [SYS_PIPE] = syscall_io_pipe,
    [SYS_CLOSE] = syscall_io_close,
    [SYS_WRITEFILE] = syscall_fs_writefile,
    [SYS_NET_CONNECT] = syscall_net_connect,
    [SYS_NET_SEND] = syscall_net_send,
    [SYS_NET_RECV] = syscall_net_recv,
    [SYS_NET_CLOSE] = syscall_net_close,
    [SYS_WIN_CREATE] = syscall_window_create,
    [SYS_WIN_PRESENT] = syscall_window_present,
    [SYS_WIN_POLL] = syscall_window_poll,
    [SYS_WIN_CLOSE] = syscall_window_close,
    [SYS_READDIR] = syscall_fs_readdir,
    [SYS_WIN_TITLE] = syscall_window_title,
    [SYS_THREAD] = syscall_process_thread,
    [SYS_SLEEP] = syscall_process_sleep,
    [SYS_WIN_RESIZABLE] = syscall_window_resizable,
    [SYS_MSEC] = syscall_process_msec,
    [SYS_FUTEX_WAIT] = syscall_process_futex_wait,
    [SYS_FUTEX_WAKE] = syscall_process_futex_wake,
    [SYS_READFILE_AT] = syscall_fs_readfile_at,
    [SYS_VOLUMES] = syscall_fs_volumes,
    [SYS_UMOUNT] = syscall_fs_umount,
    [SYS_WIN_CURSOR] = syscall_window_cursor,
    [SYS_STREAM_BEGIN] = syscall_fs_stream_begin,
    [SYS_STREAM_WRITE] = syscall_fs_stream_write,
    [SYS_STREAM_END] = syscall_fs_stream_end,
    [SYS_MKDIR] = syscall_fs_mkdir,
    [SYS_RMDIR] = syscall_fs_rmdir,
    [SYS_DELETE] = syscall_fs_delete,
    [SYS_RENAME] = syscall_fs_rename,
    [SYS_STAT] = syscall_fs_stat,
    [SYS_SPAWN] = syscall_exec_spawn,
    [SYS_MOUNT] = syscall_fs_mount,
    [SYS_AUDIO_OPEN] = syscall_audio_open,
    [SYS_AUDIO_WRITE] = syscall_audio_write,
    [SYS_AUDIO_CLOSE] = syscall_audio_close,
    [SYS_SPAWN_CAP] = syscall_exec_spawn_cap,
    [SYS_READ_NB] = syscall_io_read_nb,
    [SYS_CHDIR] = syscall_fs_chdir,
    [SYS_KILL] = syscall_process_kill,
    [SYS_POWER] = syscall_process_power,
    [SYS_SBRK] = syscall_process_sbrk,
    [SYS_GETUID] = syscall_process_getuid,
    [SYS_PORT_OPEN] = syscall_io_port_open,
    [SYS_PORT_ACCEPT] = syscall_io_port_accept,
    [SYS_RTC_EPOCH] = syscall_fs_rtc_epoch,
    [SYS_NET_SETOPT] = syscall_net_setopt,
    [SYS_NET_LISTEN] = syscall_net_listen,
    [SYS_NET_ACCEPT] = syscall_net_accept,
    [SYS_NET_BIND] = syscall_net_bind,
    [SYS_STATFS] = syscall_fs_statfs,
    [SYS_IFCONFIG] = syscall_net_ifconfig,
    [SYS_SETRTC] = syscall_net_setrtc,
    [SYS_FB_INFO] = syscall_graphics_fb_info,
    [SYS_FB_MAP] = syscall_graphics_fb_map,
    [SYS_FB_PRESENT] = syscall_graphics_fb_present,
    [SYS_INPUT_POLL] = syscall_graphics_input_poll,
    [SYS_INPUT_SET_BOUNDS] = syscall_graphics_input_bounds,
    [SYS_WIN_ENUM] = syscall_graphics_win_enum,
    [SYS_WIN_MAP] = syscall_graphics_win_map,
    [SYS_WIN_UNMAP] = syscall_graphics_win_unmap,
    [SYS_WIN_SET_GEOM] = syscall_graphics_win_geom,
    [SYS_WIN_DELIVER] = syscall_graphics_win_deliver,
    [SYS_CRASH_POLL] = syscall_graphics_crash_poll,
    [SYS_WIN_KEY_SPACE] = syscall_graphics_win_key_space,
    [SYS_STACK_ALLOC] = syscall_process_stack_alloc,
    [SYS_STACK_FREE] = syscall_process_stack_free,
    [SYS_SIGACTION] = syscall_signal_action,
    [SYS_SIGPROCMASK] = syscall_signal_procmask,
    [SYS_SIGRETURN] = syscall_signal_return,
    [SYS_GETPID] = syscall_process_getpid,
    [SYS_GETRANDOM] = syscall_entropy_getrandom,
    [SYS_WIN_OWNS] = syscall_window_owns,
    [SYS_WIN_SET_PARENT] = syscall_window_set_parent,
    [SYS_WIN_FB] = syscall_window_fb,
    [SYS_UNAME] = syscall_fs_uname,
    [SYS_GETTIME] = syscall_net_gettime,
    [SYS_SETTIME] = syscall_net_settime,
    [SYS_ADJTIME] = syscall_net_adjtime,
    [SYS_CHMOD] = syscall_fs_chmod,
    [SYS_CHOWN] = syscall_fs_chown,
    [SYS_UMASK] = syscall_fs_umask,
    [SYS_ACCESS] = syscall_fs_access,
    [SYS_TTY] = syscall_io_tty,
    [SYS_ENV] = syscall_env,
    [SYS_FB_CURSOR] = syscall_graphics_fb_cursor,
    [SYS_SET_TLS] = syscall_process_set_tls,
    [SYS_WIN_FULLSCREEN] = syscall_window_fullscreen,
    [SYS_SYMBOLIZE] = syscall_graphics_symbolize,
    [SYS_THREAD_NAME] = syscall_process_thread_name,
};

void syscall_dispatch(struct registers *r)
{
  uint32_t number = r->eax;
  if (number >= sizeof syscall_handlers / sizeof *syscall_handlers || !syscall_handlers[number]) {
    r->eax = (uint32_t)-1;
    return;
  }
  syscall_handlers[number](r);
}
