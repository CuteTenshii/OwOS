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
#include "syscall_fs.h"
#include "syscall_net.h"
#include "syscall_path.h"
#include "syscall_process.h"
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

#define SYS_WRITE 1    /* write(fd=ebx, buf=ecx, len=edx) -> bytes          */
#define SYS_EXIT 2     /* exit(code=ebx) -- terminates the process          */
#define SYS_READ 3     /* read(fd=ebx, buf=ecx, len=edx) -> bytes (0=EOF)   */
#define SYS_FORK 4     /* fork() -> child pid in parent, 0 in child         */
#define SYS_READFILE 5 /* readfile(name=ebx, buf=ecx, max=edx) -> bytes     */
#define SYS_EXEC 6     /* run a new program image (name=ebx)                */
#define SYS_WAITPID                                                                                                    \
  7                      /* waitpid(pid=ebx, status=ecx, flags=edx): flags&WNOHANG ->                                  \
                          * 1 reaped/0 none (status via *ecx); else block -> exit status */
#define SYS_PIPE 8       /* pipe(int fds[2]=ebx) -> 0/-1                       */
#define SYS_CLOSE 9      /* close(fd=ebx) -> 0/-1                             */
#define SYS_WRITEFILE 10 /* writefile(name=ebx, buf=ecx, len=edx) -> 0/-1 */
#define SYS_NET_CONNECT                                                                                                \
  11 /* net_connect(host=ebx, port=ecx, proto=edx) -> h/-e.                                                            \
      * proto SK_RAWETH opens a raw socket (no host, root-only). */
#define SYS_NET_SEND                                                                                                   \
  12 /* net_send(ebx -> {fd,buf,len,flags,ip|0,port}) -> bytes/-1.                                                     \
      * ip non-NULL = sendto; else connected/stream send. */
#define SYS_NET_RECV                                                                                                   \
  13                       /* net_recv(ebx -> {fd,buf,max,flags,ip|0,port|0}) -> bytes.                                \
                            * ip/port = recvfrom; else flags&MSG_DONTWAIT = non-blocking. */
#define SYS_NET_CLOSE 14   /* net_close(h=ebx) -> 0/-1 */
#define SYS_WIN_CREATE 15  /* win_create(w=ebx, h=ecx, title=edx) -> id/-1      */
#define SYS_WIN_PRESENT 16 /* win_present(id=ebx) -> 0/-1                       */
#define SYS_WIN_POLL                                                                                                   \
  17                         /* win_poll(id=ebx, evt=ecx|0, block=edx) -> 1 event/0 none/-1.                           \
                              * block sleeps ~a frame when no event is queued. */
#define SYS_WIN_CLOSE 18     /* win_close(id=ebx) -> 0/-1                          */
#define SYS_READDIR 19       /* readdir(path=ebx, idx=ecx, out=edx) -> 1/0/-1 */
#define SYS_WIN_TITLE 20     /* win_settitle(id=ebx, str=ecx) -> 0/-1              */
#define SYS_THREAD 21        /* thread_create(entry=ebx, stacktop=ecx) -> tid/-1   */
#define SYS_SLEEP 22         /* msleep(ms=ebx) -> 0                                */
#define SYS_WIN_RESIZABLE 23 /* win_resizable(id=ebx, on=ecx) -> 0/-1           */
#define SYS_MSEC 24          /* msec() -> milliseconds since boot                  */
#define SYS_READFILE_AT 25   /* pread(args=ebx -> {name,off,buf,len}) -> bytes/0/-1 */
#define SYS_VOLUMES 26       /* volumes(idx=ebx, out=ecx -> struct volinfo) -> 1/0/-1 */
#define SYS_UMOUNT 27        /* umount(point=ebx) -> 0/-1 (unmount the volume at point) */
#define SYS_WIN_CURSOR 28    /* win_cursor(id=ebx, shape=ecx) -> 0/-1 (enum wm_cursor)   */
#define SYS_STREAM_BEGIN 29  /* stream_begin(name=ebx) -> 0/-1 (open a streaming write) */
#define SYS_STREAM_WRITE 30  /* stream_write(buf=ebx, len=ecx) -> 0/-1 (append a chunk) */
#define SYS_STREAM_END 31    /* stream_end() -> bytes committed / -1 (commit + close)   */
#define SYS_MKDIR 32         /* mkdir(path=ebx) -> 0/-1                                    */
#define SYS_RMDIR 33         /* rmdir(path=ebx) -> 0/-1                                    */
#define SYS_DELETE 34        /* delete(path=ebx) -> 0/-1 (rm a file)                       */
#define SYS_RENAME 35        /* rename(old=ebx, new=ecx) -> 0/-1                           */
#define SYS_STAT 36          /* stat(path=ebx, out=ecx -> {int is_dir; u32 size}) -> 0/-1  */
#define SYS_SPAWN 37         /* spawn(path=ebx, arg=ecx|0) -> pid/-1 (detached)            */
#define SYS_MOUNT                                                                                                      \
  38                       /* mount(kind=ebx) -> 0/-1 (mount a present removable device at /media)                     \
                            */
#define SYS_AUDIO_OPEN 39  /* audio_open() -> 0/-1 (open a 48k/S16/stereo PCM stream)       */
#define SYS_AUDIO_WRITE 40 /* audio_write(buf=ebx, len=ecx) -> bytes / -1 / -2 (Ctrl+C)    */
#define SYS_AUDIO_CLOSE 41 /* audio_close() -> 0 (drain + stop the stream)                 */
#define SYS_SPAWN_CAP                                                                                                  \
  42 /* spawn_cap(req=ebx) -> pid/-1. Packed request (see the                                                          \
      * cap_req struct) spawns a /bin child with its stdout (and                                                       \
      * optionally stdin) wired to fresh pipe(s); optional uid/gid/                                                    \
      * cwd set the child's identity/dir (uid/gid change is root-only). */
#define SYS_READ_NB                                                                                                    \
  43 /* read_nb(fd=ebx, buf=ecx, len=edx) -> >0 bytes / 0 would-block                                                  \
      * / -1 EOF or error. Non-blocking sibling of SYS_READ, so a GUI                                                  \
      * program can poll a pipe and still service window events. */
#define SYS_CHDIR                                                                                                      \
  44 /* chdir(path=ebx) -> 0/-1: set the caller's own kernel cwd, so                                                   \
      * children it spawns resolve relative paths from there. */
#define SYS_KILL                                                                                                       \
  45 /* kill(pid=ebx, sig=ecx) -> 0/-1: post a signal to a task                                                        \
      * (root, self, or a child of the caller). sig 0 probes. */
#define SYS_POWER                                                                                                      \
  46 /* power(action=ebx): 0 = shut down (ACPI S5), 1 = reboot. Does not                                               \
      * return on success. A real action, so a syscall -- not a /proc file. */
#define SYS_SBRK                                                                                                       \
  47 /* sbrk(inc=ebx) -> previous break / (uint32_t)-1: grow or shrink the                                             \
      * caller's heap by `inc` bytes. The user-space malloc primitive. */
#define SYS_GETUID                                                                                                     \
  48                     /* getuid() -> the calling task's user id (per-process credentials;                           \
                          * user space resolves the name/home from /etc/passwd itself). */
#define SYS_PORT_OPEN 49 /* port_open(name=ebx, as_server=ecx) -> fd/-1. A named local-IPC   */
/* port: server registers + accept()s, client connects (ipc.c).    */
#define SYS_PORT_ACCEPT 50 /* port_accept(listen_fd=ebx) -> channel fd/-1 (blocks for a conn) */
#define SYS_RTC_EPOCH 51   /* rtc_epoch() -> Unix epoch seconds (UTC)            */
#define SYS_NET_SETOPT                                                                                                 \
  52 /* net_setopt(fd=ebx, opt=ecx, val=edx) -> 0/-1. Socket option;                                                   \
      * NET_OPT_TTL sets a UDP socket's outgoing IP TTL. */

/* Server sockets: the passive/bound side of the net_sock_* handle table.
 * LISTEN/ACCEPT are TCP; BIND is UDP (recv/send-to go through the unified
 * NET_RECV/NET_SEND with a source/dest address). */
#define SYS_NET_LISTEN 53 /* net_listen(port=ebx) -> listener fd/-1              */
#define SYS_NET_ACCEPT 54 /* net_accept(listen_fd=ebx) -> conn fd/-1/-4 (blocks) */
#define SYS_NET_BIND 55   /* net_bind(port=ebx) -> bound UDP fd/-1               */
#define SYS_STATFS 56     /* statfs(path=ebx, out=ecx -> {u64 total,u64 free}) -> 0/-1 */
#define SYS_IFCONFIG 57   /* ifconfig(ip=ebx, ecx -> {mask,gw,dns} 3x u8[4]) -> 0/-1 (root) */
#define SYS_SETRTC 58     /* setrtc(unix_secs=ebx) -> 0/-1. Set the wall clock; root-only */

/* User-space compositor primitives. All root-only (uid 0), like SYS_IFCONFIG.
 */
#define SYS_FB_INFO 59 /* fb_info(out=ebx -> struct fbinfo) -> 0/-1 (root) */
#define SYS_FB_MAP                                                                                                     \
  60 /* fb_map() -> COMPOSITOR_FB_VADDR / (uint)-1 (root): maps                                                        \
      * the shared cached-RAM BACK buffer into the caller.  */
#define SYS_FB_PRESENT                                                                                                 \
  61 /* fb_present(ebx -> {x,y,w,h}) -> 0/-1 (root): flush a                                                           \
      * back-buffer rect to VRAM (damage-rect present).      */
#define SYS_INPUT_POLL                                                                                                 \
  62                            /* input_poll(out=ebx -> struct inputstate) -> 0/-1 (root):                            \
                                 * pointer state + drained key events, non-blocking.    */
#define SYS_INPUT_SET_BOUNDS 63 /* input_set_bounds(w=ebx, h=ecx) -> 0/-1 (root)      */
#define SYS_WIN_ENUM 64         /* win_enum(ebx -> winrec[], ecx=max) -> count/-1 (root) */
#define SYS_WIN_MAP                                                                                                    \
  65                        /* win_map(id=ebx, out=ecx -> struct winmap) -> 0/-1 (root):                               \
                             * maps window id's buffer at WIN_MAP_BASE+id*slot.      */
#define SYS_WIN_UNMAP 66    /* win_unmap(id=ebx) -> 0/-1 (root): drop the map+pin   */
#define SYS_WIN_SET_GEOM 67 /* win_set_geom(id=ebx, ecx -> {x,y,w,h}) -> 0/-1 (root) */
#define SYS_WIN_DELIVER 68  /* win_deliver(id=ebx, ev=ecx -> struct winevent) -> 0/-1 (root) */
#define SYS_CRASH_POLL                                                                                                 \
  69 /* crash_poll(out=ebx -> crash_rec[], ecx=max) -> count/-1                                                        \
      * (root): drain pending user-crash reports for the                                                               \
      * compositor to render as crash windows.             */
#define SYS_WIN_KEY_SPACE                                                                                              \
  70                       /* win_key_space(id=ebx) -> free slots in a window's event                                  \
                            * ring (huge if not queued): paste flow control, root-only */
#define SYS_STACK_ALLOC 71 /* stack_alloc(pages=ebx) -> guarded thread-stack top / 0    */
#define SYS_STACK_FREE 72  /* stack_free(top=ebx) -> 0: release a guarded thread stack   */
#define SYS_SIGACTION 73   /* sigaction(sig=ebx, act=ecx -> k_sigaction, old=edx) -> 0/-1 */
#define SYS_SIGPROCMASK 74 /* sigprocmask(how=ebx, set=ecx, have_set=edx) -> old blocked mask */
/* SYS_SIGRETURN (75) is defined in signal.h — it's baked into the kernel-built
 * signal trampoline; user code never issues it directly. */
#define SYS_GETPID 76 /* getpid() -> the caller's pid                            */
#define SYS_GETRANDOM                                                                                                  \
  77 /* getrandom(buf=ebx, len=ecx) -> len/-1: CSPRNG bytes from                                                       \
      * the kernel entropy pool (not the clock-seeded fallback). */
#define SYS_WIN_OWNS                                                                                                   \
  78 /* win_owned(pid=ebx) -> 1 if that pid owns a window, else 0.                                                     \
      * Lets the terminal detach windowed apps with no app list. */
#define SYS_WIN_SET_PARENT                                                                                             \
  79 /* win_set_parent(id=ebx, parent=ecx, modal=edx) -> 0/-1:                                                         \
      * make `id` a transient (dialog) child of `parent`; modal                                                        \
      * blocks input to the rest of the parent's window family. */
#define SYS_WIN_FB                                                                                                     \
  80 /* win_fb_of(id=ebx) -> the client buffer's user vaddr, or 0.                                                     \
      * The first window is always at WIN_FB_VADDR; a dialog's                                                         \
      * second window lands in its own slot, fetched here.       */
#define SYS_UNAME                                                                                                      \
  81 /* uname(out=ebx -> struct utsname) -> 0/-1. POSIX system                                                         \
      * identity; the kernel is the only authority for `machine`                                                       \
      * (user programs are always 32-bit ELF). libc fills nodename                                                     \
      * from the hostname afterwards.                            */

/* Syscall argument flags / options (mirror user/lib/owos.h). */
#define WNOHANG 1      /* SYS_WAITPID: non-blocking reap                     */
#define MSG_DONTWAIT 1 /* SYS_NET_RECV: non-blocking recv */
#define NET_OPT_TTL 1  /* SYS_NET_SETOPT: outgoing IP TTL                    */

/* Net transports are general sockets: SYS_NET_CONNECT's `proto` arg selects
 * TCP/TLS/UDP/raw-ICMP, so ping/dig/resolve are libs over sockets, not
 * bespoke syscalls. */

#define NET_HOST_MAX 128

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

void syscall_dispatch(struct registers *r)
{
  switch (r->eax) {
  case SYS_WRITE: {
    int fd = (int)r->ebx;
    const char *buf = (const char *)r->ecx;
    uint32_t len = r->edx;
    if (fd < 0 || fd >= MAX_FD || !user_range(r->ecx, len)) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *f = &sched_fds()[fd];
    if (f->type == FD_CONSOLE) {
      for (uint32_t i = 0; i < len; i++)
        con_putc(buf[i]);
      r->eax = len;
    } else if (f->type == FD_PIPE_WR) {
      r->eax = (uint32_t)pipe_write(f->pipe, buf, (int)len);
    } else if (f->type == FD_CHANNEL) {
      r->eax = (uint32_t)pipe_write(f->tx, buf, (int)len);
    } else {
      r->eax = (uint32_t)-1;
    }
    break;
  }
  case SYS_EXIT:
    syscall_process_exit(r);
    break;
  case SYS_FORK:
    syscall_process_fork(r);
    break;
  case SYS_WAITPID:
    /* edx & WNOHANG -> non-blocking reap: return 1 reaped / 0 none, exit status
     * via *ecx. Otherwise block and return the child's exit status directly. */
    if ((int)r->edx & WNOHANG) {
      int status = 0;
      int got = sched_try_reap((int)r->ebx, &status);
      if (got && r->ecx && copyout(r->ecx, &status, sizeof status) != 0)
        got = -1;
      r->eax = (uint32_t)got;
    } else {
      r->eax = (uint32_t)sched_waitpid((int)r->ebx);
    }
    break;
  case SYS_PIPE: {
    if (!user_range(r->ebx, 8)) {
      r->eax = (uint32_t)-1;
      break;
    }
    int id = pipe_alloc();
    if (id < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *fds = sched_fds();
    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        rfd = i;
        break;
      }
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE && i != rfd) {
        wfd = i;
        break;
      }
    if (rfd < 0 || wfd < 0) {
      pipe_close(id, 0);
      pipe_close(id, 1);
      r->eax = (uint32_t)-1;
      break;
    }
    fds[rfd].type = FD_PIPE_RD;
    fds[rfd].pipe = (uint8_t)id;
    fds[wfd].type = FD_PIPE_WR;
    fds[wfd].pipe = (uint8_t)id;
    int out[2] = {rfd, wfd};
    if (copyout(r->ebx, out, sizeof out) != 0) {
      pipe_close(id, 0);
      pipe_close(id, 1);
      fds[rfd].type = FD_NONE;
      fds[wfd].type = FD_NONE;
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = 0;
    break;
  }
  case SYS_CLOSE: {
    int fd = (int)r->ebx;
    if (fd < 0 || fd >= MAX_FD) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *f = &sched_fds()[fd];
    if (f->type == FD_PIPE_RD)
      pipe_close(f->pipe, 0);
    else if (f->type == FD_PIPE_WR)
      pipe_close(f->pipe, 1);
    else if (f->type == FD_CHANNEL) {
      pipe_close(f->pipe, 0); /* this end reads rx  */
      pipe_close(f->tx, 1);   /* and writes tx      */
    } else if (f->type == FD_PORT)
      ipc_port_destroy(f->pipe);
    f->type = FD_NONE;
    r->eax = 0;
    break;
  }
  case SYS_EXEC: {
    if (!user_range(r->ebx, 24)) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* Copy the name out of the soon-to-be-freed user image. */
    char name[24];
    if (copyin_str(r->ebx, name, sizeof name) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }

    /* Optional argv[1] (ecx): a single string argument, copied in before
     * we touch the new address space. NULL/0 means no argument. Bounds-check
     * the pointer per byte (like SYS_READFILE_AT) so a bogus ecx can't make
     * the kernel read outside the user window. */
    char arg[128];
    int have_arg = 0;
    uint32_t uarg = r->ecx;
    if (uarg) {
      if (copyin_str(uarg, arg, sizeof arg) != 0) {
        r->eax = (uint32_t)-1;
        break;
      }
      have_arg = 1;
    }

    /* Size the buffer to the actual file. Programs are 120+ KiB now, so the
     * old fixed 64 KiB buffer truncated the ELF and load_elf failed silently
     * -- which is exactly why opening a file from the Files app (fork+exec on
     * the 124 KiB editor) did nothing. SYS_SPAWN and launch_bin already size
     * to the file; SYS_EXEC was the straggler. */
    char abs[SYS_PATH_MAX];
    if (resolve_path(name, abs, sizeof abs) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    int fsz = fs_size(abs);
    if (fsz <= 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    void *buf = kmalloc((uint32_t)fsz);
    if (!buf) {
      r->eax = (uint32_t)-1;
      break;
    }
    int n = fs_read(abs, buf, (uint32_t)fsz);
    uint32_t *dir;
    uint32_t entry, brk;
    if (n < 0 || load_elf(buf, (uint32_t)n, &dir, &entry, &brk) != 0) {
      kfree(buf);
      r->eax = (uint32_t)-1;
      break;
    }
    kfree(buf);
    char *argv[2] = {name, arg};
    uint32_t user_esp = setup_args(dir, have_arg ? 2 : 1, argv);
    if (sched_replace(dir, entry, user_esp, brk, r) < 0) {
      paging_free_process_dir(dir);
      r->eax = (uint32_t)-1;
    }
    break;
  }
  case SYS_READ: {
    int fd = (int)r->ebx;
    char *buf = (char *)r->ecx;
    uint32_t len = r->edx;
    if (fd < 0 || fd >= MAX_FD || len == 0 || !user_out_range(r->ecx, len)) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *f = &sched_fds()[fd];
    if (f->type == FD_CONSOLE) {
      /* Block for the first key (interrupts on so the timer/keyboard
       * keep firing), then drain whatever else is buffered. A pending
       * signal breaks the wait with EINTR; keep sti;hlt (context_switch
       * doesn't restore EFLAGS, so a bare hlt could halt forever). */
      char c;
      int got = 0;
      __asm__ volatile("sti");
      while (!(got = keyboard_poll(&c, 0))) {
        if (sched_intr_pending())
          break;
        __asm__ volatile("hlt");
      }
      __asm__ volatile("cli");
      if (!got) {
        r->eax = (uint32_t)-1; /* interrupted before any input (EINTR) */
        break;
      }
      uint32_t n = 0;
      buf[n++] = c;
      while (n < len && keyboard_poll(&c, 0))
        buf[n++] = c;
      r->eax = n;
    } else if (f->type == FD_PIPE_RD) {
      r->eax = (uint32_t)pipe_read(f->pipe, buf, (int)len);
    } else if (f->type == FD_CHANNEL) {
      r->eax = (uint32_t)pipe_read(f->pipe, buf, (int)len);
    } else {
      r->eax = (uint32_t)-1;
    }
    break;
  }
  case SYS_MSEC:
    syscall_process_msec(r);
    break;
  case SYS_GETUID:
    syscall_process_getuid(r);
    break;
  case SYS_PORT_OPEN: {
    const char *name = (const char *)r->ebx;
    int as_server = (int)r->ecx;
    if (!user_range(r->ebx, 16)) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *fds = sched_fds();
    int nfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        nfd = i;
        break;
      }
    if (nfd < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    if (as_server) {
      int pid = ipc_port_create(name);
      if (pid < 0) {
        r->eax = (uint32_t)-1;
        break;
      }
      fds[nfd].type = FD_PORT;
      fds[nfd].pipe = (uint8_t)pid;
    } else {
      int rx = 0, tx = 0;
      if (ipc_connect(name, &rx, &tx) < 0) {
        r->eax = (uint32_t)-1;
        break;
      }
      fds[nfd].type = FD_CHANNEL;
      fds[nfd].pipe = (uint8_t)rx;
      fds[nfd].tx = (uint8_t)tx;
    }
    r->eax = (uint32_t)nfd;
    break;
  }
  case SYS_PORT_ACCEPT: {
    int lfd = (int)r->ebx;
    struct fd *fds = sched_fds();
    if (lfd < 0 || lfd >= MAX_FD || fds[lfd].type != FD_PORT) {
      r->eax = (uint32_t)-1;
      break;
    }
    int rx = 0, tx = 0;
    if (ipc_accept(fds[lfd].pipe, &rx, &tx) < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    int nfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        nfd = i;
        break;
      }
    if (nfd < 0) { /* no slot: don't leak the new channel   */
      pipe_close(rx, 0);
      pipe_close(tx, 1);
      r->eax = (uint32_t)-1;
      break;
    }
    fds[nfd].type = FD_CHANNEL;
    fds[nfd].pipe = (uint8_t)rx;
    fds[nfd].tx = (uint8_t)tx;
    r->eax = (uint32_t)nfd;
    break;
  }
  case SYS_READFILE: {
    syscall_fs_readfile(r);
    break;
  }
  case SYS_WRITEFILE: {
    syscall_fs_writefile(r);
    break;
  }
  /* Streaming write: begin -> write* -> end, for a file too big to hold in one
   * user buffer (a browser download). Mirrors fs_stream_* one-to-one; owofs
   * allows a single open stream at a time. */
  case SYS_STREAM_BEGIN: {
    syscall_fs_stream_begin(r);
    break;
  }
  case SYS_STREAM_WRITE: {
    syscall_fs_stream_write(r);
    break;
  }
  case SYS_STREAM_END:
    syscall_fs_stream_end(r);
    break;
  case SYS_NET_CONNECT: {
    syscall_net_connect(r);
    break;
  }
  case SYS_NET_SEND: {
    syscall_net_send(r);
    break;
  }
  case SYS_NET_RECV: {
    syscall_net_recv(r);
    break;
  }
  case SYS_NET_CLOSE:
    syscall_net_close(r);
    break;
  case SYS_NET_SETOPT: /* ebx=fd, ecx=opt, edx=val */
    syscall_net_setopt(r);
    break;
  case SYS_IFCONFIG: { /* set kernel IPv4 config: root-only */
    syscall_net_ifconfig(r);
    break;
  }
  case SYS_SETRTC: { /* set the wall clock: root-only */
    syscall_net_setrtc(r);
    break;
  }
  case SYS_NET_LISTEN:
    syscall_net_listen(r);
    break;
  case SYS_NET_ACCEPT:
    syscall_net_accept(r);
    break;
  case SYS_NET_BIND:
    syscall_net_bind(r);
    break;
  case SYS_GETRANDOM: {
    uint32_t buf = r->ebx, len = r->ecx;
    if (len == 0 || len > 256 || !user_out_range(buf, len)) {
      r->eax = (uint32_t)-1;
      break;
    }
    entropy_get((void *)buf, len);
    r->eax = len;
    break;
  }
  case SYS_WIN_CREATE: {
    syscall_window_create(r);
    break;
  }
  case SYS_WIN_PRESENT:
    syscall_window_present(r);
    break;
  case SYS_WIN_POLL: {
    syscall_window_poll(r);
    break;
  }
  case SYS_WIN_CLOSE:
    syscall_window_close(r);
    break;
  case SYS_RTC_EPOCH:
    r->eax = rtc_epoch(); /* Unix epoch seconds (UTC) */
    break;
  case SYS_READDIR: {
    /* name[64] mirrors struct dirent_u (owos.h) / owofs FS_NAME_LEN. */
    struct rd_out {
      char name[64];
      int is_dir;
      uint32_t size;
    } *o;
    if (!user_out_range(r->edx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    char path[SYS_PATH_MAX];
    if (copyin_str(r->ebx, path, sizeof path) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct rd_out *)r->edx;
    char nm[64];
    int isd = 0;
    uint32_t sz = 0;
    int got = fs_enum_dir(path[0] ? path : "/", (int)r->ecx, nm, &isd, &sz, 0);
    if (got) {
      int i = 0;
      for (; i < (int)sizeof o->name - 1 && nm[i]; i++)
        o->name[i] = nm[i];
      o->name[i] = '\0';
      o->is_dir = isd;
      o->size = sz;
    }
    r->eax = (uint32_t)got;
    break;
  }
  case SYS_WIN_TITLE: {
    syscall_window_title(r);
    break;
  }
  case SYS_THREAD:
    syscall_process_thread(r);
    break;
  case SYS_SLEEP:
    syscall_process_sleep(r);
    break;
  case SYS_WIN_RESIZABLE:
    syscall_window_resizable(r);
    break;
  case SYS_WIN_CURSOR:
    syscall_window_cursor(r);
    break;
  case SYS_WIN_SET_PARENT:
    syscall_window_set_parent(r);
    break;
  case SYS_WIN_FB:
    syscall_window_fb(r);
    break;
  case SYS_READFILE_AT: {
    syscall_fs_readfile_at(r);
    break;
  }
  /* Report present removable devices, so the file explorer can list them as
   * devices -- mounted OR not, so it can offer a "Connect" button. Indexed by
   * ebx; returns 1 if that index exists, 0 past the end. `mounted` says whether
   * the device currently backs /media (and so has a valid `path`). */
  case SYS_VOLUMES: {
    /* One entry per present removable device; cross-referenced against the
     * mount table so the UI knows whether it's mounted and where. */
    struct vinfo {
      char dev[16];
      char kind[8];
      char label[24];
      char point[40];
      int mounted;
    } *o;
    if (!user_out_range(r->ecx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct vinfo *)r->ecx;
    struct blk_devinfo devs[8];
    int n = blk_dev_list(devs, 8);
    int idx = (int)r->ebx;
    if (idx < 0 || idx >= n) {
      r->eax = 0;
      break;
    }
    struct blk_devinfo *d = &devs[idx];
    const char *point = "", *label = d->model;
    int mounted = 0;
    struct fs_mountinfo mi;
    for (int i = 0; fs_mount_enum(i, &mi); i++)
      if (streq(mi.dev, d->dev)) {
        mounted = 1;
        point = mi.point;
        if (mi.label && mi.label[0])
          label = mi.label;
        break;
      }
    if (!label || !label[0])
      label = d->kind;
    int i = 0;
    for (; i < 15 && d->dev[i]; i++)
      o->dev[i] = d->dev[i];
    o->dev[i] = '\0';
    for (i = 0; i < 7 && d->kind[i]; i++)
      o->kind[i] = d->kind[i];
    o->kind[i] = '\0';
    for (i = 0; i < 23 && label[i]; i++)
      o->label[i] = label[i];
    o->label[i] = '\0';
    for (i = 0; i < 39 && point[i]; i++)
      o->point[i] = point[i];
    o->point[i] = '\0';
    o->mounted = mounted;
    r->eax = 1;
    break;
  }

  /* Mount a removable device at a mount point: mount(dev=ebx, point=ecx), e.g.
   * mount("usb0", "/media/usb0"). fs.c probes the device's filesystem and adds
   * it to the mount table. Both strings are bounds-checked per byte. */
  case SYS_MOUNT: {
    char dev[16], point[48];
    if (copyin_str(r->ebx, dev, sizeof dev) != 0 || copyin_str(r->ecx, point, sizeof point) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    __asm__ volatile("sti"); /* probing reads sectors (may wait on IRQ) */
    r->eax = (uint32_t)fs_mount(dev, point);
    break;
  }

  /* Unmount the volume at a mount point: umount(point=ebx). */
  case SYS_UMOUNT: {
    char point[48];
    if (copyin_str(r->ebx, point, sizeof point) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)fs_unmount(point);
    break;
  }

  /* Filesystem management for a shell. Each joins a relative name onto
   * the caller's cwd (like readfile/writefile) before hitting owofs. */
  case SYS_MKDIR: {
    syscall_fs_mkdir(r);
    break;
  }
  case SYS_RMDIR: {
    syscall_fs_rmdir(r);
    break;
  }
  case SYS_DELETE: {
    syscall_fs_delete(r);
    break;
  }
  case SYS_RENAME: {
    syscall_fs_rename(r);
    break;
  }
  /* Launch a /bin program as a DETACHED process (like the Start menu), rather
   * than fork+exec — a windowed shell can't fork itself cleanly. `path` must be
   * resolved by the caller (the shell prepends /bin/ or passes an absolute
   * path); `arg` is an optional single argv[1]. Returns the child pid. */
  case SYS_SPAWN: {
    char path[SYS_PATH_MAX];
    if (copyin_str(r->ebx, path, sizeof path) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* Size the buffer to the file (like apps.c launch_bin), not a fixed
     * 256 KiB -- a large fixed kmalloc fails on a pressured/fragmented heap
     * (real drivers, VMware) even when the smaller program would fit, which
     * showed up as "failed to launch" for GUI apps. */
    int fsz = fs_size(path);
    if (fsz < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    uint8_t *buf = kmalloc((uint32_t)fsz);
    if (!buf) {
      r->eax = (uint32_t)-1;
      break;
    }
    int sz = fs_read(path, buf, (uint32_t)fsz);
    if (sz < 0) {
      kfree(buf);
      r->eax = (uint32_t)-1;
      break;
    }
    char arg[SYS_PATH_MAX];
    char *argv[2];
    int argc = 1;
    argv[0] = path;
    if (r->ecx) {
      if (copyin_str(r->ecx, arg, sizeof arg) != 0) {
        kfree(buf);
        r->eax = (uint32_t)-1;
        break;
      }
      argv[1] = arg;
      argc = 2;
    }
    int pid = exec_program(buf, (uint32_t)sz, argc, argv);
    kfree(buf);
    if (pid >= 0)
      sched_set_cwd(pid, sched_cwd()); /* child inherits the caller's cwd */
    r->eax = (uint32_t)pid;
    break;
  }
  /* Spawn a /bin child with its stdout (fd 1) wired into a fresh pipe, and hand
   * the read end back to the caller. The terminal uses this to render a CLI
   * tool's output inline (replacing the old kernel-shell bridge): drain the
   * read fd with read_nb until EOF. The pipe is created with one reader (us)
   * and one writer (the child); when the child exits, fds_close_all drops its
   * write end and our read returns EOF. */
  case SYS_SPAWN_CAP: {
    /* Args arrive as one packed request so the shell can pass a full argv
     * vector (curl --http2 <url>) -- it tokenizes (with quotes) in user space,
     * the kernel just marshals the strings in and never parses a command line.
     */
    /* `wfd` is optional (0 = stdout-only capture, as the CLI tools use). When
     * non-zero, also wire the child's stdin (fd 0) to a second pipe and hand
     * the *write* end back in *wfd, so the terminal can feed keystrokes to an
     * interactive child (ssh). All callers pass the 5-field struct. */
    /* uid/gid/cwd are optional: uid<0/gid<0/cwd==0 mean "inherit" (the child
     * keeps the caller's identity/cwd, the historical behavior). PRIVILEGE
     * RULE: only a caller running as root (uid 0) may hand a child a uid/gid
     * different from its own; a non-root caller asking for a different uid or
     * gid is rejected. This is the primitive that lets /bin/sshd drop from
     * root to the authenticated user for a login session. */
    struct cap_req {
      uint32_t path;
      uint32_t argv;
      int argc;
      uint32_t rfd;
      uint32_t wfd;
      int uid;
      int gid;
      uint32_t cwd;
    };
    if (!user_range(r->ebx, sizeof(struct cap_req))) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct cap_req req;
    if (copyin(&req, r->ebx, sizeof req) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    if (!user_range(req.path, 1) || !user_out_range(req.rfd, 4)) {
      r->eax = (uint32_t)-1;
      break;
    }
    int want_stdin = req.wfd != 0;
    if (want_stdin && !user_out_range(req.wfd, 4)) {
      r->eax = (uint32_t)-1;
      break;
    }
    if (req.cwd && !user_range(req.cwd, 1)) {
      r->eax = (uint32_t)-1;
      break;
    }

    /* Copy the optional cwd out of user space (before we touch a new address
     * space). Enforce the privilege rule up front so we never spawn a child we
     * would then have to tear down. */
    uint32_t caller_uid = sched_uid();
    int set_uid = req.uid >= 0, set_gid = req.gid >= 0;
    if (caller_uid != 0 &&
        ((set_uid && (uint32_t)req.uid != caller_uid) || (set_gid && (uint32_t)req.gid != sched_gid()))) {
      r->eax = (uint32_t)-1; /* non-root may not change creds */
      break;
    }
    char cwdbuf[SYS_PATH_MAX];
    if (req.cwd) {
      if (copyin_str(req.cwd, cwdbuf, sizeof cwdbuf) != 0) {
        r->eax = (uint32_t)-1;
        break;
      }
    }

    char path[SYS_PATH_MAX];
    if (copyin_str(req.path, path, sizeof path) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }

/* Marshal argv: up to MAXARGS strings, each MAXARG bytes, copied out of the
 * user image (bounds-checked per byte) before we touch a new address space. */
#define MAXARGS 16
#define MAXARG 192
    static char argbuf[MAXARGS][MAXARG]; /* syscalls don't re-enter on this OS */
    char *argv[MAXARGS];
    int argc = req.argc;
    if (argc < 1)
      argc = 1;
    if (argc > MAXARGS)
      argc = MAXARGS;
    argv[0] = path;
    for (int ai = 1; ai < argc; ai++) {
      argv[ai] = argbuf[ai];
      argbuf[ai][0] = '\0';
      uint32_t pp = req.argv + (uint32_t)ai * 4;
      if (!user_range(pp, 4)) {
        argc = ai;
        break;
      }
      uint32_t sp;
      if (copyin(&sp, pp, sizeof sp) != 0 || copyin_str(sp, argbuf[ai], MAXARG) != 0) {
        argc = ai;
        break;
      }
    }

    int fsz = fs_size(path);
    if (fsz < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* Reserve a free fd and a pipe before loading the image, so a failure here
     * doesn't leak a half-spawned child. */
    struct fd *fds = sched_fds();
    int rfd = -1;
    for (int i = 0; i < MAX_FD; i++)
      if (fds[i].type == FD_NONE) {
        rfd = i;
        break;
      }
    if (rfd < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    int id = pipe_alloc();
    if (id < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    uint8_t *buf = kmalloc((uint32_t)fsz);
    if (!buf) {
      pipe_close(id, 0);
      pipe_close(id, 1);
      r->eax = (uint32_t)-1;
      break;
    }
    int sz = fs_read(path, buf, (uint32_t)fsz);
    int pid = sz < 0 ? -1 : exec_program(buf, (uint32_t)sz, argc, argv);
    kfree(buf);
    if (pid < 0) {
      pipe_close(id, 0);
      pipe_close(id, 1);
      r->eax = (uint32_t)-1;
      break;
    }
    fds[rfd].type = FD_PIPE_RD;
    fds[rfd].pipe = (uint8_t)id;      /* our read end   */
    sched_set_pipe_fd(pid, 1, 1, id); /* child fd 1 (stdout) -> write end */
    /* Creds + cwd, atomic with the fd wiring (the int-0x80 gate runs IF=0, so
     * no timer tick runs the child before it is fully set up). */
    if (set_uid || set_gid)
      sched_set_creds(pid, set_uid ? (uint32_t)req.uid : sched_uid(), set_gid ? (uint32_t)req.gid : sched_gid());
    sched_set_cwd(pid, req.cwd ? cwdbuf : sched_cwd()); /* explicit cwd, else inherit */
    if (copyout(req.rfd, &rfd, sizeof rfd) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* Optional stdin pipe: our write end -> child fd 0 (its read end). If we
     * can't allocate it, the child still runs (stdout-only); we just report no
     * write fd. The child already exists, so don't fail the whole spawn. */
    if (want_stdin) {
      int wfd = -1;
      for (int i = 0; i < MAX_FD; i++)
        if (fds[i].type == FD_NONE) {
          wfd = i;
          break;
        }
      int id2 = wfd >= 0 ? pipe_alloc() : -1;
      if (wfd >= 0 && id2 >= 0) {
        fds[wfd].type = FD_PIPE_WR;
        fds[wfd].pipe = (uint8_t)id2;
        sched_set_pipe_fd(pid, 0, 0, id2); /* child fd 0 (stdin) -> read end */
        copyout(req.wfd, &wfd, sizeof wfd);
      } else {
        if (id2 >= 0) {
          pipe_close(id2, 0);
          pipe_close(id2, 1);
        }
        int nofd = -1;
        copyout(req.wfd, &nofd, sizeof nofd);
      }
    }
    r->eax = (uint32_t)pid;
#undef MAXARGS
#undef MAXARG
    break;
  }
  case SYS_READ_NB: {
    int fd = (int)r->ebx;
    char *buf = (char *)r->ecx;
    uint32_t len = r->edx;
    if (fd < 0 || fd >= MAX_FD || len == 0 || !user_out_range(r->ecx, len)) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct fd *f = &sched_fds()[fd];
    if (f->type == FD_PIPE_RD)
      r->eax = (uint32_t)pipe_read_nb(f->pipe, buf, (int)len);
    else
      r->eax = (uint32_t)-1;
    break;
  }
  case SYS_CHDIR: {
    syscall_fs_chdir(r);
    break;
  }
  case SYS_KILL:
    syscall_process_kill(r);
    break;
  case SYS_POWER:
    syscall_process_power(r);
    break;
  case SYS_SBRK:
    syscall_process_sbrk(r);
    break;
  case SYS_GETPID:
    syscall_process_getpid(r);
    break;
  case SYS_WIN_OWNS:
    syscall_window_owns(r);
    break;
  case SYS_SIGACTION: {
    struct k_sigaction kact, kold;
    const struct k_sigaction *ap = 0;
    if (r->ecx) {
      if (!user_range(r->ecx, sizeof kact)) {
        r->eax = (uint32_t)-1;
        break;
      }
      if (copyin(&kact, r->ecx, sizeof kact) != 0) {
        r->eax = (uint32_t)-1;
        break;
      }
      ap = &kact;
    }
    /* Validate the oldact output pointer BEFORE mutating the disposition, so a
     * bogus oldact can't leave the handler installed while reporting failure.
     */
    if (r->edx && !user_out_range(r->edx, sizeof kold)) {
      r->eax = (uint32_t)-1;
      break;
    }
    int rc = sched_sigaction((int)r->ebx, ap, r->edx ? &kold : 0);
    if (rc == 0 && r->edx && copyout(r->edx, &kold, sizeof kold) != 0)
      rc = -1;
    r->eax = (uint32_t)rc;
    break;
  }
  case SYS_SIGPROCMASK:
    syscall_process_sigprocmask(r);
    break;
  case SYS_SIGRETURN:
    /* Restore the interrupted context from the trampoline's user frame; it sets
     * r->eax itself (to the saved eax), so don't overwrite it below. */
    sched_sigreturn(r);
    break;
  case SYS_STAT: {
    struct st_out {
      int is_dir;
      uint32_t size;
      uint32_t mtime;
    } *o;
    char name[SYS_PATH_MAX];
    if (copyin_str(r->ebx, name, sizeof name) != 0 || !user_out_range(r->ecx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    char abs[SYS_PATH_MAX];
    join_cwd(name, abs, sizeof abs);
    int isd = 0;
    uint32_t sz = 0, mt = 0;
    int rc = fs_stat(abs, &isd, &sz, &mt);
    o = (struct st_out *)r->ecx;
    if (rc == 0) {
      o->is_dir = isd;
      o->size = sz;
      o->mtime = mt; /* owofs packed wall-clock stamp (0 = unset) */
    }
    r->eax = (uint32_t)rc;
    break;
  }

  case SYS_STATFS: {
    struct sfs_out {
      uint64_t total;
      uint64_t free;
    } *o;
    if (!user_out_range(r->ecx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* One owofs volume today: `path` selects it, but every path maps to it. */
    uint64_t total = (uint64_t)blk_sectors() * 512;
    uint64_t used = fs_used_bytes();
    o = (struct sfs_out *)r->ecx;
    o->total = total;
    o->free = total > used ? total - used : 0;
    r->eax = 0;
    break;
  }

  case SYS_UNAME: {
    /* struct utsname (mirror in owos.h): five fixed 65-byte fields. The
     * kernel owns everything but nodename, which libc adds from the
     * hostname; `machine` is authoritative here — a 32-bit user program
     * can't tell the kernel's arch from its own ELF class. */
    struct k_utsname {
      char sysname[65], nodename[65], release[65], version[65], machine[65];
    } *o;
    if (!user_out_range(r->ebx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct k_utsname *)r->ebx;
    memset(o, 0, sizeof *o);
    strlcpy(o->sysname, "OwOS", sizeof o->sysname);
    strlcpy(o->release, OWOS_VERSION, sizeof o->release);
    strlcpy(o->version, OWOS_VERSION, sizeof o->version);
#if defined(__x86_64__)
    strlcpy(o->machine, "x86_64", sizeof o->machine);
#elif defined(__aarch64__)
    strlcpy(o->machine, "aarch64", sizeof o->machine);
#elif defined(__arm__)
    strlcpy(o->machine, "arm", sizeof o->machine);
#else
    strlcpy(o->machine, "i386", sizeof o->machine);
#endif
    r->eax = 0;
    break;
  }

  /* Streaming audio: /bin/play resamples a WAV to 48 kHz/S16/stereo in user
   * space and pushes it here in bounded chunks. One stream at a time (like the
   * file stream); the chosen back end is latched at open. */
  case SYS_AUDIO_OPEN:
    syscall_audio_open(r);
    break;
  case SYS_AUDIO_WRITE: {
    syscall_audio_write(r);
    break;
  }
  case SYS_AUDIO_CLOSE:
    syscall_audio_close(r);
    break;

  /* -------- Phase 1 user-space compositor primitives (all root-only) --------
   */
  case SYS_FB_INFO: {
    struct fbinfo {
      int w, h, pitch, bpp;
    } *o;
    if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct fbinfo *)r->ebx;
    fb_dims(&o->w, &o->h, &o->pitch, &o->bpp);
    r->eax = 0;
    break;
  }
  case SYS_FB_MAP: {
    /* Map the shared cached-RAM BACK buffer (never VRAM) into the caller at the
     * fixed COMPOSITOR_FB_VADDR; the compositor draws here, then SYS_FB_PRESENT
     * flushes damaged rects. */
    if (sched_uid() != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    uint32_t phys = fb_backbuffer_phys();
    uint32_t bytes = fb_backbuffer_bytes();
    if (!phys || !bytes) {
      r->eax = (uint32_t)-1;
      break;
    } /* text/single-buffered */
    int npages = (int)((bytes + 4095u) / 4096u);
    uint32_t *dir = sched_current_dir();
    if (paging_map_user_at(dir, COMPOSITOR_FB_VADDR, phys, npages) < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    /* First mapper of the back buffer is the compositor: mark it active so
     * kernel_main stops draining the physical keyboard into the recovery
     * console (the compositor now owns input via SYS_INPUT_POLL). Cleared when
     * the task exits (uwin_task_exiting). */
    uwin_compositor_register(sched_current_id());
    r->eax = COMPOSITOR_FB_VADDR;
    break;
  }
  case SYS_FB_PRESENT: {
    struct fbrect {
      int x, y, w, h;
    } *rr;
    if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *rr)) {
      r->eax = (uint32_t)-1;
      break;
    }
    rr = (struct fbrect *)r->ebx;
    fb_present_rect(rr->x, rr->y, rr->w, rr->h);
    r->eax = 0;
    break;
  }
  case SYS_INPUT_POLL: {
/* {int mx,my,buttons,wheel; int nkeys; struct{int c,mods;} keys[K];}.
 * Non-blocking: current pointer state + whatever keys are buffered now. */
#define INPUT_KEYS 32
    struct inputstate {
      int mx, my, buttons, wheel;
      int nkeys;
      struct {
        int c, mods;
      } keys[INPUT_KEYS];
    } *o;
    if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct inputstate *)r->ebx;
    o->mx = mouse_x();
    o->my = mouse_y();
    o->buttons = mouse_buttons();
    o->wheel = mouse_wheel();
    int nk = 0;
    char c;
    unsigned char mods;
    while (nk < INPUT_KEYS && keyboard_poll(&c, &mods)) {
      o->keys[nk].c = (unsigned char)c;
      o->keys[nk].mods = (int)mods;
      nk++;
    }
    /* Ctrl+C is a latch, not a queued key (it must also interrupt a busy kernel
     * wait — a recovery-shell command or an audio DMA drain — that never reads
     * the key ring). Surface it to the focused window as a KEY_INTR event too:
     * the conversion the in-kernel WM used to do, now that the compositor owns
     * input. This is the sole consumer of the latch while a compositor runs. */
    if (nk < INPUT_KEYS && keyboard_take_interrupt()) {
      o->keys[nk].c = KEY_INTR;
      o->keys[nk].mods = 0;
      nk++;
    }
    /* Print Screen is latched the same way (the IRQ can't touch the
     * framebuffer); hand it to the compositor as KEY_PRTSC to capture. */
    if (nk < INPUT_KEYS && keyboard_take_screenshot()) {
      o->keys[nk].c = KEY_PRTSC;
      o->keys[nk].mods = 0;
      nk++;
    }
    o->nkeys = nk;
    r->eax = 0;
#undef INPUT_KEYS
    break;
  }
  case SYS_INPUT_SET_BOUNDS:
    if (sched_uid() != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    mouse_set_bounds((int)r->ebx, (int)r->ecx);
    r->eax = 0;
    break;
  case SYS_WIN_ENUM: {
    int max = (int)r->ecx;
    if (max > 4096)
      max = 4096; /* cap so the size math can't overflow */
    if (sched_uid() != 0 || max <= 0 || !user_out_range(r->ebx, (uint32_t)max * sizeof(struct winrec))) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)uwin_enum((struct winrec *)r->ebx, max);
    break;
  }
  case SYS_WIN_MAP: {
    struct winmap {
      uint32_t vaddr;
      int w, h, stride;
    } *o;
    if (sched_uid() != 0 || !user_out_range(r->ecx, sizeof *o)) {
      r->eax = (uint32_t)-1;
      break;
    }
    int id = (int)r->ebx;
    if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) {
      r->eax = (uint32_t)-1;
      break;
    }
    uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
    int bw = 0, bh = 0, stride = 0;
    if (uwin_map(id, at, &bw, &bh, &stride) < 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    o = (struct winmap *)r->ecx;
    o->vaddr = at;
    o->w = bw;
    o->h = bh;
    o->stride = stride;
    r->eax = 0;
    break;
  }
  case SYS_WIN_UNMAP: {
    if (sched_uid() != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    int id = (int)r->ebx;
    if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) {
      r->eax = (uint32_t)-1;
      break;
    }
    uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
    r->eax = (uint32_t)uwin_unmap(id, sched_current_dir(), at);
    break;
  }
  case SYS_WIN_SET_GEOM: {
    struct wg {
      int x, y, w, h;
    } geom;
    if (sched_uid() != 0 || !user_range(r->ecx, sizeof geom)) {
      r->eax = (uint32_t)-1;
      break;
    }
    if (copyin(&geom, r->ecx, sizeof geom) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)uwin_set_geom((int)r->ebx, geom.x, geom.y, geom.w, geom.h);
    break;
  }
  case SYS_WIN_DELIVER: {
    if (sched_uid() != 0 || !user_range(r->ecx, sizeof(struct winevent))) {
      r->eax = (uint32_t)-1;
      break;
    }
    struct winevent ev;
    if (copyin(&ev, r->ecx, sizeof ev) != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)uwin_deliver((int)r->ebx, &ev);
    break;
  }
  case SYS_WIN_KEY_SPACE:
    if (sched_uid() != 0) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)uwin_key_space((int)r->ebx);
    break;
  case SYS_CRASH_POLL: {
    int max = (int)r->ecx;
    if (max > 4096)
      max = 4096; /* cap so the size math can't overflow */
    if (sched_uid() != 0 || max <= 0 || !user_out_range(r->ebx, (uint32_t)max * sizeof(struct crash_rec))) {
      r->eax = (uint32_t)-1;
      break;
    }
    r->eax = (uint32_t)crash_poll((struct crash_rec *)r->ebx, max);
    break;
  }
  case SYS_STACK_ALLOC:
    syscall_process_stack_alloc(r);
    break;
  case SYS_STACK_FREE:
    syscall_process_stack_free(r);
    break;

  default:
    r->eax = (uint32_t)-1;
    break;
  }
}
