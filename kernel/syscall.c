/* syscall.c — dispatch system calls from user programs.
 *
 * ABI: eax = call number, ebx/ecx/edx = arguments, return value in eax.
 */

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

/* Build stamp for SYS_UNAME's release/version; the Makefile passes it, this is
 * the fallback so a stray build path still compiles. */
#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

/* Net transports are general sockets: SYS_NET_CONNECT's `proto` arg selects
 * TCP/TLS/UDP/raw-ICMP, so ping/dig/resolve are libs over sockets, not
 * bespoke syscalls. */

/* Turn a user-supplied name into an absolute owofs path: an absolute name
 * ("/foo/bar") passes through; a relative one is joined onto the calling
 * process's cwd, so `curl -o out.txt` (and any program's readfile/writefile)
 * lands in the directory the user is actually in. owofs resolves "." / ".." in
 * the result, so the join can be naive. */
void join_cwd(const char *name, char *out, int outsz)
{
  int n = 0;
  if (name[0] != '/') {
    const char *c = sched_cwd();
    while (*c && n < outsz - 1)
      out[n++] = *c++;
    if ((n == 0 || out[n - 1] != '/') && n < outsz - 1)
      out[n++] = '/';
  }
  while (*name && n < outsz - 1)
    out[n++] = *name++;
  out[n] = '\0';
}

/* Read a file by `name` (resolved against the cwd), falling back to /bin/<name>
 * when a relative name isn't found -- the same PATH-style lookup the shell
 * does, so a program can read or run another by bare name (fork+run "curl" ->
 * /bin/curl). */
int read_with_path(const char *name, void *buf, uint32_t max)
{
  char abs[SYS_PATH_MAX];
  join_cwd(name, abs, sizeof abs);
  int n = fs_read(abs, buf, max);
  if (n < 0 && name[0] != '/') {
    char p[SYS_PATH_MAX];
    int j = 0;
    const char *pfx = "/bin/";
    for (int i = 0; pfx[i]; i++)
      p[j++] = pfx[i];
    for (int i = 0; name[i] && j < (int)sizeof(p) - 1; i++)
      p[j++] = name[i];
    p[j] = '\0';
    n = fs_read(p, buf, max);
  }
  return n;
}

/* Resolve `name` (cwd-relative, then /bin) to an absolute owofs path that
 * exists; fills `out`, returns 0 / -1. */
int resolve_path(const char *name, char *out, int outcap)
{
  uint32_t sz;
  join_cwd(name, out, (uint32_t)outcap);
  if (fs_stat(out, 0, &sz, 0) == 0)
    return 0;
  if (name[0] == '/')
    return -1;
  int j = 0;
  const char *pfx = "/bin/";
  for (int i = 0; pfx[i]; i++)
    out[j++] = pfx[i];
  for (int i = 0; name[i] && j < outcap - 1; i++)
    out[j++] = name[i];
  out[j] = '\0';
  return fs_stat(out, 0, &sz, 0) == 0 ? 0 : -1;
}

/* pread: read up to `len` bytes at byte `off`. Clamps to the file size and
 * returns a short count (0 = EOF), so a program can stream a file far
 * larger than its 1.5 MiB window — e.g. an image viewer decoding a big PNG. */
int read_at_with_path(const char *name, uint32_t off, void *buf, uint32_t len)
{
  char abs[SYS_PATH_MAX];
  uint32_t size = 0;
  if (resolve_path(name, abs, sizeof abs) != 0)
    return -1;
  if (fs_stat(abs, 0, &size, 0) != 0)
    return -1; /* missing/unstattable: not EOF, an error
                * (fs_stat leaves *size untouched on
                * failure — reading it was a UB bug that
                * hung on tmpfs where the garbage was big) */
  if (off >= size)
    return 0; /* EOF                                  */
  if (len > size - off)
    len = size - off;
  return fs_read_at(abs, off, buf, len);
}

/* A user pointer must stay entirely within the user region, or we refuse
 * the call -- otherwise a program could make the kernel touch its memory. */
extern void isr128(void); /* the int 0x80 stub (isr.S)               */

/* Unix seconds (UTC) -> civil date/time (Howard Hinnant's days-from-civil
 * inverse). Any time source can drive SYS_SETRTC; rtc_set wants broken-down
 * fields, so the conversion lives here rather than in each caller. */
void epoch_to_rtc(uint32_t secs, struct rtc_time *t)
{
  uint32_t days = secs / 86400u, rem = secs % 86400u;
  t->hour = (uint8_t)(rem / 3600u);
  t->min = (uint8_t)((rem % 3600u) / 60u);
  t->sec = (uint8_t)(rem % 60u);

  uint32_t z = days + 719468u; /* shift epoch to 0000-03-01       */
  uint32_t era = z / 146097u;
  uint32_t doe = z - era * 146097u; /* [0, 146096]     */
  uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  uint32_t y = yoe + era * 400u;
  uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); /* [0, 365]    */
  uint32_t mp = (5u * doy + 2u) / 153u;                      /* [0, 11]     */
  uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;             /* [1, 31]      */
  uint32_t m = mp < 10u ? mp + 3u : mp - 9u;                 /* [1, 12]      */

  t->day = (uint8_t)d;
  t->month = (uint8_t)m;
  t->year = (uint16_t)(y + (m <= 2u ? 1u : 0u));
}

void syscall_init(void)
{
  /* 0xEE = present, DPL 3, 32-bit interrupt gate -- ring 3 may invoke it. */
  idt_set_gate(0x80, (uint32_t)isr128, GDT_KERNEL_CODE, 0xEE);
}

typedef void (*syscall_handler_t)(struct registers *r);

static const syscall_handler_t syscall_handlers[SYS_UNAME + 1] = {
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
};

void syscall_dispatch(struct registers *r)
{
  uint32_t number = r->eax;
  if (number > SYS_UNAME || !syscall_handlers[number]) {
    r->eax = (uint32_t)-1;
    return;
  }
  syscall_handlers[number](r);
}
