/* syscall.c — dispatch system calls from user programs.
 *
 * ABI: eax = call number, ebx/ecx/edx = arguments, return value in eax.
 */

#include <stdint.h>
#include "syscall.h"
#include "idt.h"
#include "gdt.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "procfs.h"
#include "sysfs.h"
#include "kstring.h"
#include "power.h"
#include "fat.h"
#include "hfs.h"
#include "ext.h"
#include "iso9660.h"
#include "blk.h"
#include "loader.h"
#include "sched.h"
#include "heap.h"
#include "file.h"
#include "pipe.h"
#include "ipc.h"
#include "net.h"
#include "uwin.h"
#include "rtc.h"
#include "shell.h"
#include "cpu.h"
#include "pmm.h"
#include "pci.h"
#include "firmware.h"
#include "smbios.h"
#include "battery.h"
#include "fb.h"
#include "edid.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "hda.h"
#include "sb16.h"
#include "mouse.h"
#include "crash.h"
#include "paging.h"

#define SYS_WRITE    1   /* write(fd=ebx, buf=ecx, len=edx) -> bytes          */
#define SYS_EXIT     2   /* exit(code=ebx) -- terminates the process          */
#define SYS_READ     3   /* read(fd=ebx, buf=ecx, len=edx) -> bytes (0=EOF)   */
#define SYS_FORK     4   /* fork() -> child pid in parent, 0 in child         */
#define SYS_UPTIME   5   /* uptime() -- seconds since boot                    */
#define SYS_READFILE 6   /* readfile(name=ebx, buf=ecx, max=edx) -> bytes     */
#define SYS_EXEC     7   /* run a new program image (name=ebx)                */
#define SYS_WAITPID  8   /* waitpid(pid=ebx) -> child's exit status           */
#define SYS_PIPE     9   /* pipe(int fds[2]=ebx) -> 0/-1                       */
#define SYS_CLOSE    10  /* close(fd=ebx) -> 0/-1                             */
#define SYS_WRITEFILE 11 /* writefile(name=ebx, buf=ecx, len=edx) -> 0/-1     */
#define SYS_NET_CONNECT 12 /* net_connect(host=ebx, port=ecx, tls=edx) -> h/-e */
#define SYS_NET_SEND   13 /* net_send(h=ebx, buf=ecx, len=edx) -> bytes/-1     */
#define SYS_NET_RECV   14 /* net_recv(h=ebx, buf=ecx, max=edx) -> bytes(0=EOF) */
#define SYS_NET_CLOSE  15 /* net_close(h=ebx) -> 0/-1                          */
#define SYS_WIN_CREATE 16 /* win_create(w=ebx, h=ecx, title=edx) -> id/-1      */
#define SYS_WIN_PRESENT 17 /* win_present(id=ebx) -> 0/-1                       */
#define SYS_WIN_POLL   18 /* win_poll(id=ebx, evt=ecx) -> 1 event/0 none/-1     */
#define SYS_WIN_WAIT   19 /* win_wait(id=ebx) -> 0 (sleep ~a frame for events)  */
#define SYS_WIN_CLOSE  20 /* win_close(id=ebx) -> 0/-1                          */
#define SYS_RTC        21 /* rtc() -> packed (hour<<16)|(min<<8)|sec            */
#define SYS_RTC_EPOCH  58 /* rtc_epoch() -> Unix epoch seconds (UTC)            */
#define SYS_NET_SET_TTL 59 /* net_set_ttl(h=ebx, ttl=ecx) -> 0/-1. Outgoing IP  */
                           /* TTL for a UDP socket (IP_MULTICAST_TTL analog).   */
#define SYS_READDIR    22 /* readdir(path=ebx, idx=ecx, out=edx) -> 1/0/-1      */
#define SYS_WIN_TITLE  23 /* win_settitle(id=ebx, str=ecx) -> 0/-1              */
#define SYS_THREAD     24 /* thread_create(entry=ebx, stacktop=ecx) -> tid/-1   */
#define SYS_SLEEP      25 /* msleep(ms=ebx) -> 0                                */
#define SYS_WIN_RESIZABLE 26 /* win_resizable(id=ebx, on=ecx) -> 0/-1           */
#define SYS_MSEC       27 /* msec() -> milliseconds since boot                  */
#define SYS_READFILE_AT 28 /* pread(args=ebx -> {name,off,buf,len}) -> bytes/0/-1 */
#define SYS_VOLUMES    29 /* volumes(idx=ebx, out=ecx -> struct volinfo) -> 1/0/-1 */
#define SYS_UMOUNT     30 /* umount(point=ebx) -> 0/-1 (unmount the volume at point) */
#define SYS_WIN_CURSOR 31 /* win_cursor(id=ebx, shape=ecx) -> 0/-1 (enum wm_cursor)   */
#define SYS_STREAM_BEGIN 32 /* stream_begin(name=ebx) -> 0/-1 (open a streaming write) */
#define SYS_STREAM_WRITE 33 /* stream_write(buf=ebx, len=ecx) -> 0/-1 (append a chunk) */
#define SYS_STREAM_END   34 /* stream_end() -> bytes committed / -1 (commit + close)   */
#define SYS_MKDIR      35 /* mkdir(path=ebx) -> 0/-1                                    */
#define SYS_RMDIR      36 /* rmdir(path=ebx) -> 0/-1                                    */
#define SYS_DELETE     37 /* delete(path=ebx) -> 0/-1 (rm a file)                       */
#define SYS_RENAME     38 /* rename(old=ebx, new=ecx) -> 0/-1                           */
#define SYS_STAT       39 /* stat(path=ebx, out=ecx -> {int is_dir; u32 size}) -> 0/-1  */
#define SYS_SPAWN      40 /* spawn(path=ebx, arg=ecx|0) -> pid/-1 (detached, like the WM) */
#define SYS_MOUNT      41 /* mount(kind=ebx) -> 0/-1 (mount a present removable device at /media) */
#define SYS_AUDIO_OPEN 42 /* audio_open() -> 0/-1 (open a 48k/S16/stereo PCM stream)       */
#define SYS_AUDIO_WRITE 43 /* audio_write(buf=ebx, len=ecx) -> bytes / -1 / -2 (Ctrl+C)    */
#define SYS_AUDIO_CLOSE 44 /* audio_close() -> 0 (drain + stop the stream)                 */
/* 45 unused (was SYS_INTR, the old per-task Ctrl+C latch — replaced by signals:
 * a caught SIGINT / default terminate, see SYS_SIGACTION below). */
/* 46 unused (was SYS_NET_ALPN; TLS + ALPN moved to user space) — do not reuse
 * without bumping every consumer, the numbers stay in sync with user/lib/lib.c */
#define SYS_SPAWN_CAP  47 /* spawn_cap(req=ebx) -> pid/-1. Packed request (see the
                           * cap_req struct) spawns a /bin child with its stdout (and
                           * optionally stdin) wired to fresh pipe(s); optional uid/gid/
                           * cwd set the child's identity/dir (uid/gid change is root-only). */
#define SYS_READ_NB    48 /* read_nb(fd=ebx, buf=ecx, len=edx) -> >0 bytes / 0 would-block
                           * / -1 EOF or error. Non-blocking sibling of SYS_READ, so a GUI
                           * program can poll a pipe and still service window events. */
#define SYS_CHDIR      49 /* chdir(path=ebx) -> 0/-1: set the caller's own kernel cwd, so
                           * children it spawns resolve relative paths from there. */
#define SYS_KILL       50 /* kill(pid=ebx, sig=ecx) -> 0/-1: post a signal to a task
                           * (root, self, or a child of the caller). sig 0 probes. */
#define SYS_POWER      51 /* power(action=ebx): 0 = shut down (ACPI S5), 1 = reboot. Does not
                           * return on success. A real action, so a syscall -- not a /proc file. */
#define SYS_SBRK       52 /* sbrk(inc=ebx) -> previous break / (uint32_t)-1: grow or shrink the
                           * caller's heap by `inc` bytes. The user-space malloc primitive. */
#define SYS_NET_RECV_NB 53 /* net_recv_nb(h=ebx, buf=ecx, max=edx) -> >0 bytes / 0 would-block /
                            * -1 EOF|err / -4 intr. Non-blocking recv for interactive clients
                            * (ssh) that multiplex the socket with stdin. */
#define SYS_GETUID     54 /* getuid() -> the calling task's user id (per-process credentials, */
#define SYS_GETGID     55 /* getgid() -> its group id; user space resolves the name/home from */
                          /* /etc/passwd itself, so there's no /proc identity file.           */
#define SYS_PORT_OPEN  56 /* port_open(name=ebx, as_server=ecx) -> fd/-1. A named local-IPC   */
                          /* port: server registers + accept()s, client connects (ipc.c).    */
#define SYS_PORT_ACCEPT 57 /* port_accept(listen_fd=ebx) -> channel fd/-1 (blocks for a conn) */

/* Server sockets (Stage 3): the passive/bound side of the net_sock_* handle
 * table. LISTEN/ACCEPT are TCP; BIND/RECVFROM/SENDTO are UDP. RECVFROM/SENDTO
 * pass their extra args via a small struct pointer (ebx) since the ABI has only
 * 3 registers. */
#define SYS_NET_LISTEN   60 /* net_listen(port=ebx) -> listener fd/-1              */
#define SYS_NET_ACCEPT   61 /* net_accept(listen_fd=ebx) -> conn fd/-1/-4 (blocks) */
#define SYS_NET_BIND     62 /* net_bind(port=ebx) -> bound UDP fd/-1               */
#define SYS_NET_RECVFROM 63 /* net_recvfrom(ebx -> {fd,buf,max,ip,port}) -> bytes/0/-1/-4 */
#define SYS_NET_SENDTO   64 /* net_sendto(ebx -> {fd,ip,port,buf,len}) -> len/-1   */
#define SYS_STATFS       65 /* statfs(path=ebx, out=ecx -> {u64 total,u64 free}) -> 0/-1 */
#define SYS_NET_RAW_OPEN 66 /* net_raw_open() -> raw-Ethernet fd/-1 (root-only, AF_PACKET) */
#define SYS_IFCONFIG     67 /* ifconfig(ip=ebx, ecx -> {mask,gw,dns} 3x u8[4]) -> 0/-1 (root) */
#define SYS_SETRTC       68 /* setrtc(unix_secs=ebx) -> 0/-1. Set the wall clock; root-only */

/* Phase 1 of the user-space compositor: additive primitives a /bin/compositor
 * needs. All root-only (uid 0), like SYS_IFCONFIG — the compositor runs as root.
 * Unconsumed until the compositor exists; the in-kernel WM still drives the
 * display. */
#define SYS_FB_INFO         69 /* fb_info(out=ebx -> struct fbinfo) -> 0/-1 (root)   */
#define SYS_FB_MAP          70 /* fb_map() -> COMPOSITOR_FB_VADDR / (uint)-1 (root): maps
                                * the shared cached-RAM BACK buffer into the caller.  */
#define SYS_FB_PRESENT      71 /* fb_present(ebx -> {x,y,w,h}) -> 0/-1 (root): flush a
                                * back-buffer rect to VRAM (damage-rect present).      */
#define SYS_INPUT_POLL      72 /* input_poll(out=ebx -> struct inputstate) -> 0/-1 (root):
                                * pointer state + drained key events, non-blocking.    */
#define SYS_INPUT_SET_BOUNDS 73 /* input_set_bounds(w=ebx, h=ecx) -> 0/-1 (root)      */
#define SYS_WIN_ENUM        74 /* win_enum(ebx -> winrec[], ecx=max) -> count/-1 (root) */
#define SYS_WIN_MAP         75 /* win_map(id=ebx, out=ecx -> struct winmap) -> 0/-1 (root):
                                * maps window id's buffer at WIN_MAP_BASE+id*slot.      */
#define SYS_WIN_UNMAP       76 /* win_unmap(id=ebx) -> 0/-1 (root): drop the map+pin   */
#define SYS_WIN_SET_GEOM    77 /* win_set_geom(id=ebx, ecx -> {x,y,w,h}) -> 0/-1 (root) */
#define SYS_WIN_DELIVER     78 /* win_deliver(id=ebx, ev=ecx -> struct winevent) -> 0/-1 (root) */
#define SYS_WIN_SET_FOCUS   79 /* win_set_focus(id=ebx) -> 0/-1 (root; id<=0 = none)   */
#define SYS_WIN_KEY_SPACE   81 /* win_key_space(id=ebx) -> free slots in a window's event
                                * ring (huge if not queued): paste flow control, root-only */
#define SYS_WAITPID_NB      82 /* waitpid_nb(pid=ebx, status_out=ecx) -> 1 reaped / 0 none:
                                * non-blocking reap (the compositor reaps its detached apps) */
#define SYS_STACK_ALLOC     84 /* stack_alloc(pages=ebx) -> guarded thread-stack top / 0    */
#define SYS_STACK_FREE      85 /* stack_free(top=ebx) -> 0: release a guarded thread stack   */
#define SYS_CRASH_POLL      80 /* crash_poll(out=ebx -> crash_rec[], ecx=max) -> count/-1
                                * (root): drain pending user-crash reports for the
                                * compositor to render as crash windows.             */
#define SYS_SIGACTION       86 /* sigaction(sig=ebx, act=ecx -> k_sigaction, old=edx) -> 0/-1 */
#define SYS_SIGPROCMASK     87 /* sigprocmask(how=ebx, set=ecx, have_set=edx) -> old blocked mask */
/* SYS_SIGRETURN (88) is defined in signal.h — it's baked into the kernel-built
 * signal trampoline; user code never issues it directly. */
#define SYS_GETPID          89 /* getpid() -> the caller's pid                            */

/* Net transports are general sockets: SYS_NET_CONNECT's `proto` arg selects
 * TCP/TLS/UDP/raw-ICMP, so ping/dig/resolve are libs over sockets, not
 * bespoke syscalls. */

#define NET_HOST_MAX 128

#define SYS_PATH_MAX 256

/* Turn a user-supplied name into an absolute owofs path: an absolute name
 * ("/foo/bar") passes through; a relative one is joined onto the calling
 * process's cwd, so `curl -o out.txt` (and any program's readfile/writefile)
 * lands in the directory the user is actually in. owofs resolves "." / ".." in
 * the result, so the join can be naive. */
static void join_cwd(const char *name, char *out, int outsz)
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
 * when a relative name isn't found -- the same PATH-style lookup the shell does,
 * so a program can read or run another by bare name (fork+run "curl" ->
 * /bin/curl). */
static int read_with_path(const char *name, void *buf, uint32_t max)
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
static int resolve_path(const char *name, char *out, int outcap)
{
	uint32_t sz;
	join_cwd(name, out, (uint32_t)outcap);
	if (fs_stat(out, 0, &sz, 0) == 0)
		return 0;
	if (name[0] == '/')
		return -1;
	int j = 0;
	const char *pfx = "/bin/";
	for (int i = 0; pfx[i]; i++) out[j++] = pfx[i];
	for (int i = 0; name[i] && j < outcap - 1; i++) out[j++] = name[i];
	out[j] = '\0';
	return fs_stat(out, 0, &sz, 0) == 0 ? 0 : -1;
}

/* pread: read up to `len` bytes at byte `off`. Clamps to the file size and
 * returns a short count (0 = EOF), so a program can stream a file far
 * larger than its 1.5 MiB window — e.g. an image viewer decoding a big PNG. */
static int read_at_with_path(const char *name, uint32_t off, void *buf, uint32_t len)
{
	char abs[SYS_PATH_MAX];
	uint32_t size = 0;
	if (resolve_path(name, abs, sizeof abs) != 0)
		return -1;
	if (fs_stat(abs, 0, &size, 0) != 0)
		return -1;                     /* missing/unstattable: not EOF, an error
		                                * (fs_stat leaves *size untouched on
		                                * failure — reading it was a UB bug that
		                                * hung on tmpfs where the garbage was big) */
	if (off >= size)
		return 0;                      /* EOF                                  */
	if (len > size - off)
		len = size - off;
	return fs_read_at(abs, off, buf, len);
}

/* A user pointer must stay entirely within the user region, or we refuse
 * the call -- otherwise a program could make the kernel touch its memory. */
static int user_range(uint32_t addr, uint32_t len)
{
	return addr >= USER_BASE
	    && addr + len <= USER_REGION_END
	    && addr + len >= addr;          /* no overflow                       */
}

extern void isr128(void);          /* the int 0x80 stub (isr.S)               */

/* Unix seconds (UTC) -> civil date/time (Howard Hinnant's days-from-civil
 * inverse). Any time source can drive SYS_SETRTC; rtc_set wants broken-down
 * fields, so the conversion lives here rather than in each caller. */
static void epoch_to_rtc(uint32_t secs, struct rtc_time *t)
{
	uint32_t days = secs / 86400u, rem = secs % 86400u;
	t->hour = (uint8_t)(rem / 3600u);
	t->min  = (uint8_t)((rem % 3600u) / 60u);
	t->sec  = (uint8_t)(rem % 60u);

	uint32_t z   = days + 719468u;          /* shift epoch to 0000-03-01       */
	uint32_t era = z / 146097u;
	uint32_t doe = z - era * 146097u;                       /* [0, 146096]     */
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t y   = yoe + era * 400u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);   /* [0, 365]    */
	uint32_t mp  = (5u * doy + 2u) / 153u;                       /* [0, 11]     */
	uint32_t d   = doy - (153u * mp + 2u) / 5u + 1u;            /* [1, 31]      */
	uint32_t m   = mp < 10u ? mp + 3u : mp - 9u;               /* [1, 12]      */

	t->day   = (uint8_t)d;
	t->month = (uint8_t)m;
	t->year  = (uint16_t)(y + (m <= 2u ? 1u : 0u));
}


void syscall_init(void)
{
	/* 0xEE = present, DPL 3, 32-bit interrupt gate -- ring 3 may invoke it. */
	idt_set_gate(0x80, (uint32_t)isr128, GDT_KERNEL_CODE, 0xEE);
}

/* Which audio back end the open stream is using: 0 none, 1 HDA, 2 SB16. Global
 * because owofs-style there is one audio stream at a time across the system. */
static int audio_be;

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
		sched_exit((int)r->ebx);   /* terminates the task; does not return     */
		break;
	case SYS_FORK:
		r->eax = (uint32_t)sched_fork(r);   /* child pid to parent, 0 to child */
		break;
	case SYS_WAITPID:
		r->eax = (uint32_t)sched_waitpid((int)r->ebx);
		break;
	case SYS_WAITPID_NB: {
		/* Non-blocking reap: status via *ecx (if given), returns 1 reaped / 0 none. */
		int status = 0;
		int got = sched_try_reap((int)r->ebx, &status);
		if (got && r->ecx && user_range(r->ecx, 4))
			*(int *)r->ecx = status;
		r->eax = (uint32_t)got;
		break;
	}
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
			if (fds[i].type == FD_NONE) { rfd = i; break; }
		for (int i = 0; i < MAX_FD; i++)
			if (fds[i].type == FD_NONE && i != rfd) { wfd = i; break; }
		if (rfd < 0 || wfd < 0) {
			pipe_close(id, 0);
			pipe_close(id, 1);
			r->eax = (uint32_t)-1;
			break;
		}
		fds[rfd].type = FD_PIPE_RD; fds[rfd].pipe = (uint8_t)id;
		fds[wfd].type = FD_PIPE_WR; fds[wfd].pipe = (uint8_t)id;
		int *out = (int *)r->ebx;
		out[0] = rfd;
		out[1] = wfd;
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
			pipe_close(f->pipe, 0);    /* this end reads rx  */
			pipe_close(f->tx, 1);      /* and writes tx      */
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
		const char *uname = (const char *)r->ebx;
		int i = 0;
		for (; i < 23 && uname[i]; i++)
			name[i] = uname[i];
		name[i] = '\0';

		/* Optional argv[1] (ecx): a single string argument, copied in before
		 * we touch the new address space. NULL/0 means no argument. Bounds-check
		 * the pointer per byte (like SYS_READFILE_AT) so a bogus ecx can't make
		 * the kernel read outside the user window. */
		char arg[128];
		int have_arg = 0;
		uint32_t uarg = r->ecx;
		if (uarg) {
			int j = 0;
			while (j < (int)sizeof(arg) - 1 && user_range(uarg + (uint32_t)j, 1)) {
				char ch = ((const char *)uarg)[j];
				if (!ch)
					break;
				arg[j++] = ch;
			}
			arg[j] = '\0';
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
		char *argv[2] = { name, arg };
		uint32_t user_esp = setup_args(dir, have_arg ? 2 : 1, argv);
		sched_set_brk(brk);                       /* the new image's heap base   */
		sched_replace(dir, entry, user_esp, r);   /* returns into the new image */
		break;
	}
	case SYS_READ: {
		int fd = (int)r->ebx;
		char *buf = (char *)r->ecx;
		uint32_t len = r->edx;
		if (fd < 0 || fd >= MAX_FD || len == 0 || !user_range(r->ecx, len)) {
			r->eax = (uint32_t)-1;
			break;
		}
		struct fd *f = &sched_fds()[fd];
		if (f->type == FD_CONSOLE) {
			/* Block for the first key (interrupts on so the timer/keyboard
			 * keep firing), then drain whatever else is buffered. */
			char c;
			__asm__ volatile ("sti");
			while (!keyboard_poll(&c, 0))
				__asm__ volatile ("hlt");
			__asm__ volatile ("cli");
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
	case SYS_UPTIME:
		r->eax = timer_seconds();
		break;
	case SYS_MSEC:
		r->eax = timer_us() / 1000u;   /* ms since boot (wraps ~49.7 days) */
		break;
	case SYS_GETUID:
		r->eax = sched_uid();          /* per-process credentials (see sched.c) */
		break;
	case SYS_GETGID:
		r->eax = sched_gid();
		break;
	case SYS_PORT_OPEN: {
		const char *name = (const char *)r->ebx;
		int as_server = (int)r->ecx;
		if (!user_range(r->ebx, 16)) { r->eax = (uint32_t)-1; break; }
		struct fd *fds = sched_fds();
		int nfd = -1;
		for (int i = 0; i < MAX_FD; i++)
			if (fds[i].type == FD_NONE) { nfd = i; break; }
		if (nfd < 0) { r->eax = (uint32_t)-1; break; }
		if (as_server) {
			int pid = ipc_port_create(name);
			if (pid < 0) { r->eax = (uint32_t)-1; break; }
			fds[nfd].type = FD_PORT; fds[nfd].pipe = (uint8_t)pid;
		} else {
			int rx = 0, tx = 0;
			if (ipc_connect(name, &rx, &tx) < 0) { r->eax = (uint32_t)-1; break; }
			fds[nfd].type = FD_CHANNEL; fds[nfd].pipe = (uint8_t)rx; fds[nfd].tx = (uint8_t)tx;
		}
		r->eax = (uint32_t)nfd;
		break;
	}
	case SYS_PORT_ACCEPT: {
		int lfd = (int)r->ebx;
		struct fd *fds = sched_fds();
		if (lfd < 0 || lfd >= MAX_FD || fds[lfd].type != FD_PORT) { r->eax = (uint32_t)-1; break; }
		int rx = 0, tx = 0;
		if (ipc_accept(fds[lfd].pipe, &rx, &tx) < 0) { r->eax = (uint32_t)-1; break; }
		int nfd = -1;
		for (int i = 0; i < MAX_FD; i++)
			if (fds[i].type == FD_NONE) { nfd = i; break; }
		if (nfd < 0) {                  /* no slot: don't leak the new channel   */
			pipe_close(rx, 0); pipe_close(tx, 1);
			r->eax = (uint32_t)-1; break;
		}
		fds[nfd].type = FD_CHANNEL; fds[nfd].pipe = (uint8_t)rx; fds[nfd].tx = (uint8_t)tx;
		r->eax = (uint32_t)nfd;
		break;
	}
	case SYS_READFILE: {
		const char *name = (const char *)r->ebx;
		uint32_t buf = r->ecx, max = r->edx;
		if (!user_range(r->ebx, 24) || !user_range(buf, max)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)read_with_path(name, (void *)buf, max);
		break;
	}
	case SYS_WRITEFILE: {
		const char *name = (const char *)r->ebx;
		uint32_t buf = r->ecx, len = r->edx;
		if (!user_range(r->ebx, 24) || !user_range(buf, len)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char abs[SYS_PATH_MAX];
		join_cwd(name, abs, sizeof abs);
		const char *proc = procfs_subpath(abs);
		if (proc && proc[0]) {             /* /proc/<x> is a control node, not a file */
			r->eax = (uint32_t)procfs_write(proc, (const char *)buf, (int)len);
			break;
		}
		const char *sysp = sysfs_subpath(abs);   /* /sys/<...> writable attribute */
		if (sysp && sysp[0]) {
			r->eax = (uint32_t)sysfs_write(sysp, (const char *)buf, (int)len);
			break;
		}
		r->eax = (uint32_t)fs_create(abs, (const void *)buf, len);
		break;
	}
	/* Streaming write: begin -> write* -> end, for a file too big to hold in one
	 * user buffer (a browser download). Mirrors fs_stream_* one-to-one; owofs
	 * allows a single open stream at a time. */
	case SYS_STREAM_BEGIN: {
		const char *name = (const char *)r->ebx;
		if (!user_range(r->ebx, 24)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char abs[SYS_PATH_MAX];
		join_cwd(name, abs, sizeof abs);
		r->eax = (uint32_t)fs_stream_begin(abs);
		break;
	}
	case SYS_STREAM_WRITE: {
		uint32_t buf = r->ebx, len = r->ecx;
		if (!user_range(buf, len)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)fs_stream_write((const void *)buf, len);
		break;
	}
	case SYS_STREAM_END:
		r->eax = (uint32_t)fs_stream_end();
		break;
	case SYS_NET_CONNECT: {
		/* Copy the host string out of user memory, bounded. */
		if (!user_range(r->ebx, 1)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char host[NET_HOST_MAX];
		const char *uh = (const char *)r->ebx;
		int i = 0;
		for (; i < NET_HOST_MAX - 1; i++) {
			if (!user_range(r->ebx + (uint32_t)i, 1))
				break;
			char c = uh[i];
			if (!c)
				break;
			host[i] = c;
		}
		host[i] = '\0';
		r->eax = (uint32_t)net_sock_connect(host, (uint16_t)r->ecx,
		                                     (int)r->edx);
		break;
	}
	case SYS_NET_SEND: {
		uint32_t len = r->edx;
		if (len == 0 || !user_range(r->ecx, len)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)net_sock_send((int)r->ebx,
		                                 (const void *)r->ecx, len);
		break;
	}
	case SYS_NET_RECV: {
		uint32_t max = r->edx;
		if (max == 0 || !user_range(r->ecx, max)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)net_sock_recv((int)r->ebx, (void *)r->ecx, max);
		break;
	}
	case SYS_NET_RECV_NB: {
		uint32_t max = r->edx;
		if (max == 0 || !user_range(r->ecx, max)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)net_sock_recv_nb((int)r->ebx, (void *)r->ecx, max);
		break;
	}
	case SYS_NET_CLOSE:
		r->eax = (uint32_t)net_sock_close((int)r->ebx);
		break;
	case SYS_NET_SET_TTL:
		r->eax = (uint32_t)net_sock_set_ttl((int)r->ebx, (int)r->ecx);
		break;
	case SYS_NET_RAW_OPEN:                     /* AF_PACKET analog: root-only */
		r->eax = sched_uid() != 0 ? (uint32_t)-1
		                          : (uint32_t)net_raw_open();
		break;
	case SYS_IFCONFIG: {                       /* set kernel IPv4 config: root-only */
		/* ebx -> ip[4]; ecx -> {mask[4], gw[4], dns[4]} (12 bytes). */
		if (sched_uid() != 0 || !user_range(r->ebx, 4) || !user_range(r->ecx, 12)) {
			r->eax = (uint32_t)-1;
			break;
		}
		uint8_t ip[4], mask[4], gw[4], dns[4];
		const uint8_t *ipp = (const uint8_t *)r->ebx;
		const uint8_t *rest = (const uint8_t *)r->ecx;
		for (int i = 0; i < 4; i++) {
			ip[i]   = ipp[i];
			mask[i] = rest[i];
			gw[i]   = rest[4 + i];
			dns[i]  = rest[8 + i];
		}
		net_ifconfig(ip, mask, gw, dns);
		r->eax = 0;
		break;
	}
	case SYS_SETRTC: {                         /* set the wall clock: root-only */
		if (sched_uid() != 0) {
			r->eax = (uint32_t)-1;
			break;
		}
		struct rtc_time t;
		epoch_to_rtc((uint32_t)r->ebx, &t);
		rtc_set(&t);
		r->eax = 0;
		break;
	}
	case SYS_NET_LISTEN:
		r->eax = (uint32_t)net_sock_listen((int)r->ebx);
		break;
	case SYS_NET_ACCEPT:
		r->eax = (uint32_t)net_sock_accept((int)r->ebx);
		break;
	case SYS_NET_BIND:
		r->eax = (uint32_t)net_sock_bind((int)r->ebx);
		break;
	case SYS_NET_RECVFROM: {
		/* ebx -> {int fd, void *buf, u32 max, u8 *ip[4], int *port}: recv one
		 * datagram, then copy the sender's IP/port out to the user pointers. */
		if (!user_range(r->ebx, 20)) {
			r->eax = (uint32_t)-1;
			break;
		}
		const uint32_t *a = (const uint32_t *)r->ebx;
		uint32_t max = a[2], bp = a[1], ipp = a[3], portp = a[4];
		if (max == 0 || !user_range(bp, max) ||
		    (ipp && !user_range(ipp, 4)) ||
		    (portp && !user_range(portp, 4))) {
			r->eax = (uint32_t)-1;
			break;
		}
		uint8_t ip[4];
		uint16_t port = 0;
		int n = net_sock_recvfrom((int)a[0], (void *)bp, max,
		                          ipp ? ip : NULL, &port);
		if (n > 0) {
			if (ipp)
				for (int i = 0; i < 4; i++) ((uint8_t *)ipp)[i] = ip[i];
			if (portp)
				*(int *)portp = port;
		}
		r->eax = (uint32_t)n;
		break;
	}
	case SYS_NET_SENDTO: {
		/* ebx -> {int fd, const u8 ip[4], int port, const void *buf, u32 len}. */
		if (!user_range(r->ebx, 20)) {
			r->eax = (uint32_t)-1;
			break;
		}
		const uint32_t *a = (const uint32_t *)r->ebx;
		uint32_t ipp = a[1], len = a[4], bp = a[3];
		if (len == 0 || !user_range(bp, len) || !user_range(ipp, 4)) {
			r->eax = (uint32_t)-1;
			break;
		}
		uint8_t ip[4];
		for (int i = 0; i < 4; i++) ip[i] = ((const uint8_t *)ipp)[i];
		r->eax = (uint32_t)net_sock_sendto((int)a[0], ip, (int)a[2],
		                                   (const void *)bp, len);
		break;
	}
	case SYS_WIN_CREATE: {
		int w = (int)r->ebx, h = (int)r->ecx;
		char title[64];
		int n = 0;
		uint32_t tp = r->edx;
		if (tp)
			while (n < 63 && user_range(tp + (uint32_t)n, 1)) {
				char ch = ((const char *)tp)[n];
				if (!ch)
					break;
				title[n++] = ch;
			}
		title[n] = '\0';
		r->eax = (uint32_t)uwin_create(w, h, title);
		break;
	}
	case SYS_WIN_PRESENT:
		r->eax = (uint32_t)uwin_present((int)r->ebx);
		break;
	case SYS_WIN_POLL:
		if (!user_range(r->ecx, sizeof(struct win_event))) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)uwin_poll((int)r->ebx,
		                             (struct win_event *)r->ecx);
		break;
	case SYS_WIN_WAIT:
		/* The int 0x80 gate runs with IF=0; timer_sleep() halts on a bare hlt,
		 * which would wedge the CPU forever (the timer couldn't wake it). Enable
		 * interrupts first so the tick fires, other tasks (the WM!) run, and the
		 * sleep returns. */
		__asm__ volatile ("sti");
		timer_sleep(15);           /* sleep ~one frame; events poll after */
		r->eax = 0;
		break;
	case SYS_WIN_CLOSE:
		r->eax = (uint32_t)uwin_close((int)r->ebx);
		break;
	case SYS_RTC: {
		struct rtc_time t;
		rtc_now(&t);
		r->eax = ((uint32_t)t.hour << 16) | ((uint32_t)t.min << 8) | t.sec;
		break;
	}
	case SYS_RTC_EPOCH:
		r->eax = rtc_epoch();                      /* Unix epoch seconds (UTC) */
		break;
	case SYS_READDIR: {
		/* name[64] mirrors struct dirent_u (owos.h) / owofs FS_NAME_LEN. */
		struct rd_out { char name[64]; int is_dir; uint32_t size; } *o;
		if (!user_range(r->ebx, 1) || !user_range(r->edx, sizeof *o)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char path[SYS_PATH_MAX];
		int n = 0;
		for (; n < SYS_PATH_MAX - 1 && user_range(r->ebx + (uint32_t)n, 1); n++) {
			char c = ((const char *)r->ebx)[n];
			if (!c) break;
			path[n] = c;
		}
		path[n] = '\0';
		o = (struct rd_out *)r->edx;
		char nm[64];
		int isd = 0;
		uint32_t sz = 0;
		int got = fs_enum_dir(path[0] ? path : "/", (int)r->ecx, nm, &isd, &sz, 0);
		if (got) {
			int i = 0;
			for (; i < (int)sizeof o->name - 1 && nm[i]; i++) o->name[i] = nm[i];
			o->name[i] = '\0';
			o->is_dir = isd;
			o->size = sz;
		}
		r->eax = (uint32_t)got;
		break;
	}
	case SYS_WIN_TITLE: {
		char title[64];
		int n = 0;
		uint32_t tp = r->ecx;
		if (tp)
			while (n < 63 && user_range(tp + (uint32_t)n, 1)) {
				char c = ((const char *)tp)[n];
				if (!c) break;
				title[n++] = c;
			}
		title[n] = '\0';
		r->eax = (uint32_t)uwin_set_title((int)r->ebx, title);
		break;
	}
	case SYS_THREAD:
		/* entry + stack top must both lie inside the caller's user window. */
		if (!user_range(r->ebx, 1) || !user_range(r->ecx, 4)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)sched_create_user_thread(r->ebx, r->ecx);
		break;
	case SYS_SLEEP:
		__asm__ volatile ("sti");      /* int-gate IF=0; let the tick wake us */
		timer_sleep(r->ebx);
		r->eax = 0;
		break;
	case SYS_WIN_RESIZABLE:
		r->eax = (uint32_t)uwin_set_resizable((int)r->ebx, (int)r->ecx);
		break;
	case SYS_WIN_CURSOR:
		r->eax = (uint32_t)uwin_set_cursor((int)r->ebx, (int)r->ecx);
		break;
	case SYS_READFILE_AT: {
		/* ebx -> {name, offset, buf, len}. Copy the name in with a per-byte
		 * bounds check, validate the destination buffer, then stream-read. */
		if (!user_range(r->ebx, 16)) {
			r->eax = (uint32_t)-1;
			break;
		}
		const uint32_t *a = (const uint32_t *)r->ebx;
		uint32_t np = a[0], off = a[1], bp = a[2], len = a[3];
		char name[SYS_PATH_MAX];
		int k = 0;
		while (k < SYS_PATH_MAX - 1 && user_range(np + (uint32_t)k, 1)) {
			char ch = ((const char *)np)[k];
			if (!ch)
				break;
			name[k++] = ch;
		}
		name[k] = '\0';
		if (len && !user_range(bp, len)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)read_at_with_path(name, off, (void *)bp, len);
		break;
	}
	/* Report present removable devices, so the file explorer can list them as
	 * devices -- mounted OR not, so it can offer a "Connect" button. Indexed by
	 * ebx; returns 1 if that index exists, 0 past the end. `mounted` says whether
	 * the device currently backs /media (and so has a valid `path`). */
	case SYS_VOLUMES: {
		/* One entry per present removable device; cross-referenced against the
		 * mount table so the UI knows whether it's mounted and where. */
		struct vinfo { char dev[16]; char kind[8]; char label[24];
		               char point[40]; int mounted; } *o;
		if (!user_range(r->ecx, sizeof *o)) {
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
				mounted = 1; point = mi.point;
				if (mi.label && mi.label[0]) label = mi.label;
				break;
			}
		if (!label || !label[0]) label = d->kind;
		int i = 0;
		for (; i < 15 && d->dev[i]; i++)  o->dev[i]  = d->dev[i];  o->dev[i]  = '\0';
		for (i = 0; i < 7  && d->kind[i]; i++) o->kind[i] = d->kind[i]; o->kind[i] = '\0';
		for (i = 0; i < 23 && label[i]; i++)   o->label[i] = label[i]; o->label[i] = '\0';
		for (i = 0; i < 39 && point[i]; i++)   o->point[i] = point[i]; o->point[i] = '\0';
		o->mounted = mounted;
		r->eax = 1;
		break;
	}

	/* Mount a removable device at a mount point: mount(dev=ebx, point=ecx), e.g.
	 * mount("usb0", "/media/usb0"). fs.c probes the device's filesystem and adds
	 * it to the mount table. Both strings are bounds-checked per byte. */
	case SYS_MOUNT: {
		if (!user_range(r->ebx, 1) || !user_range(r->ecx, 1)) { r->eax = (uint32_t)-1; break; }
		char dev[16], point[48];
		int j = 0;
		for (; j < (int)sizeof(dev) - 1 && user_range(r->ebx + (uint32_t)j, 1); j++) {
			char ch = ((const char *)r->ebx)[j];
			if (!ch) break;
			dev[j] = ch;
		}
		dev[j] = '\0';
		j = 0;
		for (; j < (int)sizeof(point) - 1 && user_range(r->ecx + (uint32_t)j, 1); j++) {
			char ch = ((const char *)r->ecx)[j];
			if (!ch) break;
			point[j] = ch;
		}
		point[j] = '\0';
		__asm__ volatile ("sti");          /* probing reads sectors (may wait on IRQ) */
		r->eax = (uint32_t)fs_mount(dev, point);
		break;
	}

	/* Unmount the volume at a mount point: umount(point=ebx). */
	case SYS_UMOUNT: {
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char point[48];
		int j = 0;
		for (; j < (int)sizeof(point) - 1 && user_range(r->ebx + (uint32_t)j, 1); j++) {
			char ch = ((const char *)r->ebx)[j];
			if (!ch) break;
			point[j] = ch;
		}
		point[j] = '\0';
		r->eax = (uint32_t)fs_unmount(point);
		break;
	}

	/* Filesystem management for a shell. Each joins a relative name onto
	 * the caller's cwd (like readfile/writefile) before hitting owofs. */
	case SYS_MKDIR: {
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char abs[SYS_PATH_MAX];
		join_cwd((const char *)r->ebx, abs, sizeof abs);
		r->eax = (uint32_t)fs_mkdir(abs);
		break;
	}
	case SYS_RMDIR: {
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char abs[SYS_PATH_MAX];
		join_cwd((const char *)r->ebx, abs, sizeof abs);
		r->eax = (uint32_t)fs_rmdir(abs);
		break;
	}
	case SYS_DELETE: {
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char abs[SYS_PATH_MAX];
		join_cwd((const char *)r->ebx, abs, sizeof abs);
		r->eax = (uint32_t)fs_delete(abs);
		break;
	}
	case SYS_RENAME: {
		if (!user_range(r->ebx, 1) || !user_range(r->ecx, 1)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char a[SYS_PATH_MAX], b[SYS_PATH_MAX];
		join_cwd((const char *)r->ebx, a, sizeof a);
		join_cwd((const char *)r->ecx, b, sizeof b);
		r->eax = (uint32_t)fs_rename(a, b);
		break;
	}
	/* Launch a /bin program as a DETACHED process (like the Start menu), rather
	 * than fork+exec — a windowed shell can't fork itself cleanly. `path` must be
	 * resolved by the caller (the shell prepends /bin/ or passes an absolute
	 * path); `arg` is an optional single argv[1]. Returns the child pid. */
	case SYS_SPAWN: {
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char path[SYS_PATH_MAX];
		int n = 0;
		for (; n < SYS_PATH_MAX - 1 && user_range(r->ebx + (uint32_t)n, 1); n++) {
			char c = ((const char *)r->ebx)[n];
			if (!c) break;
			path[n] = c;
		}
		path[n] = '\0';
		/* Size the buffer to the file (like apps.c launch_bin), not a fixed
		 * 256 KiB -- a large fixed kmalloc fails on a pressured/fragmented heap
		 * (real drivers, VMware) even when the smaller program would fit, which
		 * showed up as "failed to launch" for GUI apps. */
		int fsz = fs_size(path);
		if (fsz < 0) { r->eax = (uint32_t)-1; break; }
		uint8_t *buf = kmalloc((uint32_t)fsz);
		if (!buf) { r->eax = (uint32_t)-1; break; }
		int sz = fs_read(path, buf, (uint32_t)fsz);
		if (sz < 0) { kfree(buf); r->eax = (uint32_t)-1; break; }
		char arg[SYS_PATH_MAX];
		char *argv[2];
		int argc = 1;
		argv[0] = path;
		if (r->ecx && user_range(r->ecx, 1)) {
			int j = 0;
			for (; j < SYS_PATH_MAX - 1 && user_range(r->ecx + (uint32_t)j, 1); j++) {
				char c = ((const char *)r->ecx)[j];
				if (!c) break;
				arg[j] = c;
			}
			arg[j] = '\0';
			argv[1] = arg;
			argc = 2;
		}
		int pid = exec_program(buf, (uint32_t)sz, argc, argv);
		kfree(buf);
		if (pid >= 0)
			sched_set_cwd(pid, sched_cwd());   /* child inherits the caller's cwd */
		r->eax = (uint32_t)pid;
		break;
	}
	/* Spawn a /bin child with its stdout (fd 1) wired into a fresh pipe, and hand
	 * the read end back to the caller. The terminal uses this to render a CLI
	 * tool's output inline (replacing the old kernel-shell bridge): drain the read
	 * fd with read_nb until EOF. The pipe is created with one reader (us) and one
	 * writer (the child); when the child exits, fds_close_all drops its write end
	 * and our read returns EOF. */
	case SYS_SPAWN_CAP: {
		/* Args arrive as one packed request so the shell can pass a full argv
		 * vector (curl --http2 <url>) -- it tokenizes (with quotes) in user space, the
		 * kernel just marshals the strings in and never parses a command line. */
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
		struct cap_req { uint32_t path; uint32_t argv; int argc; uint32_t rfd; uint32_t wfd;
		                 int uid; int gid; uint32_t cwd; };
		if (!user_range(r->ebx, sizeof(struct cap_req))) { r->eax = (uint32_t)-1; break; }
		struct cap_req req = *(const struct cap_req *)r->ebx;
		if (!user_range(req.path, 1) || !user_range(req.rfd, 4)) { r->eax = (uint32_t)-1; break; }
		int want_stdin = req.wfd != 0;
		if (want_stdin && !user_range(req.wfd, 4)) { r->eax = (uint32_t)-1; break; }
		if (req.cwd && !user_range(req.cwd, 1)) { r->eax = (uint32_t)-1; break; }

		/* Copy the optional cwd out of user space (before we touch a new address
		 * space). Enforce the privilege rule up front so we never spawn a child we
		 * would then have to tear down. */
		uint32_t caller_uid = sched_uid();
		int set_uid = req.uid >= 0, set_gid = req.gid >= 0;
		if (caller_uid != 0 &&
		    ((set_uid && (uint32_t)req.uid != caller_uid) ||
		     (set_gid && (uint32_t)req.gid != sched_gid()))) {
			r->eax = (uint32_t)-1;        /* non-root may not change creds */
			break;
		}
		char cwdbuf[SYS_PATH_MAX];
		if (req.cwd) {
			int ci = 0;
			for (; ci < SYS_PATH_MAX - 1 && user_range(req.cwd + (uint32_t)ci, 1); ci++) {
				char c = ((const char *)req.cwd)[ci];
				if (!c) break;
				cwdbuf[ci] = c;
			}
			cwdbuf[ci] = '\0';
		}

		char path[SYS_PATH_MAX];
		int n = 0;
		for (; n < SYS_PATH_MAX - 1 && user_range(req.path + (uint32_t)n, 1); n++) {
			char c = ((const char *)req.path)[n];
			if (!c) break;
			path[n] = c;
		}
		path[n] = '\0';

		/* Marshal argv: up to MAXARGS strings, each MAXARG bytes, copied out of the
		 * user image (bounds-checked per byte) before we touch a new address space. */
		#define MAXARGS 16
		#define MAXARG  192
		static char argbuf[MAXARGS][MAXARG];   /* syscalls don't re-enter on this OS */
		char *argv[MAXARGS];
		int argc = req.argc;
		if (argc < 1) argc = 1;
		if (argc > MAXARGS) argc = MAXARGS;
		argv[0] = path;
		for (int ai = 1; ai < argc; ai++) {
			argv[ai] = argbuf[ai];
			argbuf[ai][0] = '\0';
			uint32_t pp = req.argv + (uint32_t)ai * 4;
			if (!user_range(pp, 4)) { argc = ai; break; }
			uint32_t sp = *(const uint32_t *)pp;
			int j = 0;
			for (; j < MAXARG - 1 && user_range(sp + (uint32_t)j, 1); j++) {
				char c = ((const char *)sp)[j];
				if (!c) break;
				argbuf[ai][j] = c;
			}
			argbuf[ai][j] = '\0';
		}

		int fsz = fs_size(path);
		if (fsz < 0) { r->eax = (uint32_t)-1; break; }
		/* Reserve a free fd and a pipe before loading the image, so a failure here
		 * doesn't leak a half-spawned child. */
		struct fd *fds = sched_fds();
		int rfd = -1;
		for (int i = 0; i < MAX_FD; i++)
			if (fds[i].type == FD_NONE) { rfd = i; break; }
		if (rfd < 0) { r->eax = (uint32_t)-1; break; }
		int id = pipe_alloc();
		if (id < 0) { r->eax = (uint32_t)-1; break; }
		uint8_t *buf = kmalloc((uint32_t)fsz);
		if (!buf) { pipe_close(id, 0); pipe_close(id, 1); r->eax = (uint32_t)-1; break; }
		int sz = fs_read(path, buf, (uint32_t)fsz);
		int pid = sz < 0 ? -1 : exec_program(buf, (uint32_t)sz, argc, argv);
		kfree(buf);
		if (pid < 0) { pipe_close(id, 0); pipe_close(id, 1); r->eax = (uint32_t)-1; break; }
		fds[rfd].type = FD_PIPE_RD; fds[rfd].pipe = (uint8_t)id;  /* our read end   */
		sched_set_pipe_fd(pid, 1, 1, id);          /* child fd 1 (stdout) -> write end */
		/* Creds + cwd, atomic with the fd wiring (the int-0x80 gate runs IF=0, so
		 * no timer tick runs the child before it is fully set up). */
		if (set_uid || set_gid)
			sched_set_creds(pid, set_uid ? (uint32_t)req.uid : sched_uid(),
			                     set_gid ? (uint32_t)req.gid : sched_gid());
		sched_set_cwd(pid, req.cwd ? cwdbuf : sched_cwd());  /* explicit cwd, else inherit */
		*(int *)req.rfd = rfd;
		/* Optional stdin pipe: our write end -> child fd 0 (its read end). If we
		 * can't allocate it, the child still runs (stdout-only); we just report no
		 * write fd. The child already exists, so don't fail the whole spawn. */
		if (want_stdin) {
			int wfd = -1;
			for (int i = 0; i < MAX_FD; i++)
				if (fds[i].type == FD_NONE) { wfd = i; break; }
			int id2 = wfd >= 0 ? pipe_alloc() : -1;
			if (wfd >= 0 && id2 >= 0) {
				fds[wfd].type = FD_PIPE_WR; fds[wfd].pipe = (uint8_t)id2;
				sched_set_pipe_fd(pid, 0, 0, id2);   /* child fd 0 (stdin) -> read end */
				*(int *)req.wfd = wfd;
			} else {
				if (id2 >= 0) { pipe_close(id2, 0); pipe_close(id2, 1); }
				*(int *)req.wfd = -1;
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
		if (fd < 0 || fd >= MAX_FD || len == 0 || !user_range(r->ecx, len)) {
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
		if (!user_range(r->ebx, 1)) { r->eax = (uint32_t)-1; break; }
		char path[SYS_PATH_MAX];
		int n = 0;
		for (; n < SYS_PATH_MAX - 1 && user_range(r->ebx + (uint32_t)n, 1); n++) {
			char c = ((const char *)r->ebx)[n];
			if (!c) break;
			path[n] = c;
		}
		path[n] = '\0';
		char abs[SYS_PATH_MAX];
		int isd = 0;
		if (resolve_path(path, abs, sizeof abs) != 0 || fs_stat(abs, &isd, 0, 0) != 0 || !isd) {
			r->eax = (uint32_t)-1;
			break;
		}
		sched_set_cwd_self(abs);
		r->eax = 0;
		break;
	}
	case SYS_KILL:
		r->eax = (uint32_t)sched_signal_send((int)r->ebx, (int)r->ecx);
		break;
	case SYS_POWER:
		if (r->ebx == 1) power_reset();
		else             power_off();
		r->eax = (uint32_t)-1;             /* only reached if the action failed */
		break;
	case SYS_SBRK:
		r->eax = sched_sbrk((int)r->ebx);  /* grow/shrink the caller's heap */
		break;
	case SYS_GETPID:
		r->eax = sched_current_id();
		break;
	case SYS_SIGACTION: {
		struct k_sigaction kact, kold;
		const struct k_sigaction *ap = 0;
		if (r->ecx) {
			if (!user_range(r->ecx, sizeof kact)) { r->eax = (uint32_t)-1; break; }
			kact = *(const struct k_sigaction *)r->ecx;
			ap = &kact;
		}
		int rc = sched_sigaction((int)r->ebx, ap, r->edx ? &kold : 0);
		if (rc == 0 && r->edx) {
			if (!user_range(r->edx, sizeof kold)) { r->eax = (uint32_t)-1; break; }
			*(struct k_sigaction *)r->edx = kold;
		}
		r->eax = (uint32_t)rc;
		break;
	}
	case SYS_SIGPROCMASK:
		r->eax = sched_sigprocmask((int)r->ebx, (uint32_t)r->ecx, (int)r->edx);
		break;
	case SYS_SIGRETURN:
		/* Restore the interrupted context from the trampoline's user frame; it sets
		 * r->eax itself (to the saved eax), so don't overwrite it below. */
		sched_sigreturn(r);
		break;
	case SYS_STAT: {
		struct st_out { int is_dir; uint32_t size; uint32_t mtime; } *o;
		if (!user_range(r->ebx, 1) || !user_range(r->ecx, sizeof *o)) {
			r->eax = (uint32_t)-1;
			break;
		}
		char abs[SYS_PATH_MAX];
		join_cwd((const char *)r->ebx, abs, sizeof abs);
		int isd = 0;
		uint32_t sz = 0, mt = 0;
		int rc = fs_stat(abs, &isd, &sz, &mt);
		o = (struct st_out *)r->ecx;
		if (rc == 0) {
			o->is_dir = isd;
			o->size = sz;
			o->mtime = mt;          /* owofs packed wall-clock stamp (0 = unset) */
		}
		r->eax = (uint32_t)rc;
		break;
	}

	case SYS_STATFS: {
		struct sfs_out { uint64_t total; uint64_t free; } *o;
		if (!user_range(r->ecx, sizeof *o)) {
			r->eax = (uint32_t)-1;
			break;
		}
		/* One owofs volume today: `path` selects it, but every path maps to it. */
		uint64_t total = (uint64_t)blk_sectors() * 512;
		uint64_t used  = fs_used_bytes();
		o = (struct sfs_out *)r->ecx;
		o->total = total;
		o->free  = total > used ? total - used : 0;
		r->eax = 0;
		break;
	}


	/* Streaming audio: /bin/play resamples a WAV to 48 kHz/S16/stereo in user space
	 * and pushes it here in bounded chunks. One stream at a time (like the file
	 * stream); the chosen back end is latched at open. */
	case SYS_AUDIO_OPEN:
		if (hda_present())       audio_be = (hda_stream_open()  == 0) ? 1 : 0;
		else if (sb16_present()) audio_be = (sb16_stream_open() == 0) ? 2 : 0;
		else                     audio_be = 0;
		r->eax = audio_be ? 0 : (uint32_t)-1;
		break;
	case SYS_AUDIO_WRITE: {
		uint32_t buf = r->ebx, len = r->ecx;
		if (len == 0 || !user_range(buf, len)) { r->eax = (uint32_t)-1; break; }
		__asm__ volatile ("sti");          /* back-pressure waits need the tick */
		if (audio_be == 1)      r->eax = (uint32_t)hda_stream_write((const void *)buf, len);
		else if (audio_be == 2) r->eax = (uint32_t)sb16_stream_write((const void *)buf, len);
		else                    r->eax = (uint32_t)-1;
		break;
	}
	case SYS_AUDIO_CLOSE:
		__asm__ volatile ("sti");
		if (audio_be == 1)      hda_stream_close();
		else if (audio_be == 2) sb16_stream_close();
		audio_be = 0;
		r->eax = 0;
		break;

	/* -------- Phase 1 user-space compositor primitives (all root-only) -------- */
	case SYS_FB_INFO: {
		struct fbinfo { int w, h, pitch, bpp; } *o;
		if (sched_uid() != 0 || !user_range(r->ebx, sizeof *o)) {
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
		if (sched_uid() != 0) { r->eax = (uint32_t)-1; break; }
		uint32_t phys = fb_backbuffer_phys();
		uint32_t bytes = fb_backbuffer_bytes();
		if (!phys || !bytes) { r->eax = (uint32_t)-1; break; }  /* text/single-buffered */
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
		struct fbrect { int x, y, w, h; } *rr;
		if (sched_uid() != 0 || !user_range(r->ebx, sizeof *rr)) {
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
		struct inputstate { int mx, my, buttons, wheel; int nkeys;
		                    struct { int c, mods; } keys[INPUT_KEYS]; } *o;
		if (sched_uid() != 0 || !user_range(r->ebx, sizeof *o)) {
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
		o->nkeys = nk;
		r->eax = 0;
		#undef INPUT_KEYS
		break;
	}
	case SYS_INPUT_SET_BOUNDS:
		if (sched_uid() != 0) { r->eax = (uint32_t)-1; break; }
		mouse_set_bounds((int)r->ebx, (int)r->ecx);
		r->eax = 0;
		break;
	case SYS_WIN_ENUM: {
		int max = (int)r->ecx;
		if (sched_uid() != 0 || max <= 0 ||
		    !user_range(r->ebx, (uint32_t)max * sizeof(struct winrec))) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)uwin_enum((struct winrec *)r->ebx, max);
		break;
	}
	case SYS_WIN_MAP: {
		struct winmap { uint32_t vaddr; int w, h, stride; } *o;
		if (sched_uid() != 0 || !user_range(r->ecx, sizeof *o)) {
			r->eax = (uint32_t)-1;
			break;
		}
		int id = (int)r->ebx;
		if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) { r->eax = (uint32_t)-1; break; }
		uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
		int bw = 0, bh = 0, stride = 0;
		if (uwin_map(id, at, &bw, &bh, &stride) < 0) { r->eax = (uint32_t)-1; break; }
		o = (struct winmap *)r->ecx;
		o->vaddr = at;
		o->w = bw; o->h = bh; o->stride = stride;
		r->eax = 0;
		break;
	}
	case SYS_WIN_UNMAP: {
		if (sched_uid() != 0) { r->eax = (uint32_t)-1; break; }
		int id = (int)r->ebx;
		if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) { r->eax = (uint32_t)-1; break; }
		uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
		r->eax = (uint32_t)uwin_unmap(id, sched_current_dir(), at);
		break;
	}
	case SYS_WIN_SET_GEOM: {
		struct wg { int x, y, w, h; } *g;
		if (sched_uid() != 0 || !user_range(r->ecx, sizeof *g)) {
			r->eax = (uint32_t)-1;
			break;
		}
		g = (struct wg *)r->ecx;
		r->eax = (uint32_t)uwin_set_geom((int)r->ebx, g->x, g->y, g->w, g->h);
		break;
	}
	case SYS_WIN_DELIVER: {
		if (sched_uid() != 0 || !user_range(r->ecx, sizeof(struct winevent))) {
			r->eax = (uint32_t)-1;
			break;
		}
		struct winevent ev = *(const struct winevent *)r->ecx;
		r->eax = (uint32_t)uwin_deliver((int)r->ebx, &ev);
		break;
	}
	case SYS_WIN_SET_FOCUS:
		if (sched_uid() != 0) { r->eax = (uint32_t)-1; break; }
		r->eax = (uint32_t)uwin_set_focus((int)r->ebx);
		break;
	case SYS_WIN_KEY_SPACE:
		if (sched_uid() != 0) { r->eax = (uint32_t)-1; break; }
		r->eax = (uint32_t)uwin_key_space((int)r->ebx);
		break;
	case SYS_CRASH_POLL: {
		int max = (int)r->ecx;
		if (sched_uid() != 0 || max <= 0 ||
		    !user_range(r->ebx, (uint32_t)max * sizeof(struct crash_rec))) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)crash_poll((struct crash_rec *)r->ebx, max);
		break;
	}
	case SYS_STACK_ALLOC:
		r->eax = user_stack_alloc((int)r->ebx);
		break;
	case SYS_STACK_FREE:
		user_stack_free(r->ebx);
		r->eax = 0;
		break;

	default:
		r->eax = (uint32_t)-1;
		break;
	}
}
