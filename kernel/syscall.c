/* syscall.c — dispatch system calls from ring 3.
 *
 * ABI: eax = call number, ebx/ecx/edx = arguments, return value in eax.
 */

#include <stdint.h>
#include "syscall.h"
#include "idt.h"
#include "gdt.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "loader.h"
#include "sched.h"
#include "heap.h"
#include "file.h"
#include "pipe.h"
#include "net.h"
#include "uwin.h"
#include "rtc.h"

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
#define SYS_READDIR    22 /* readdir(path=ebx, idx=ecx, out=edx) -> 1/0/-1      */
#define SYS_WIN_TITLE  23 /* win_settitle(id=ebx, str=ecx) -> 0/-1              */
#define SYS_THREAD     24 /* thread_create(entry=ebx, stacktop=ecx) -> tid/-1   */
#define SYS_SLEEP      25 /* msleep(ms=ebx) -> 0                                */
#define SYS_WIN_RESIZABLE 26 /* win_resizable(id=ebx, on=ecx) -> 0/-1           */

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
 * so a program can read or run another by bare name (fork+run "worker" ->
 * /bin/worker). */
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

/* A user pointer must stay entirely within the ring-3 region, or we refuse
 * the call -- otherwise a program could make the kernel touch its memory. */
static int user_range(uint32_t addr, uint32_t len)
{
	return addr >= USER_BASE
	    && addr + len <= USER_REGION_END
	    && addr + len >= addr;          /* no overflow                       */
}

extern void isr128(void);          /* the int 0x80 stub (isr.S)               */

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
				vga_putc(buf[i]);
			r->eax = len;
		} else if (f->type == FD_PIPE_WR) {
			r->eax = (uint32_t)pipe_write(f->pipe, buf, (int)len);
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

		void *buf = kmalloc(64 * 1024);
		if (!buf) {
			r->eax = (uint32_t)-1;
			break;
		}
		int n = read_with_path(name, buf, 64 * 1024);
		uint32_t *dir;
		uint32_t entry;
		if (n < 0 || load_elf(buf, (uint32_t)n, &dir, &entry) != 0) {
			kfree(buf);
			r->eax = (uint32_t)-1;
			break;
		}
		kfree(buf);
		char *argv[1] = { name };
		uint32_t user_esp = setup_args(dir, 1, argv);
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
			while (!keyboard_poll(&c))
				__asm__ volatile ("hlt");
			__asm__ volatile ("cli");
			uint32_t n = 0;
			buf[n++] = c;
			while (n < len && keyboard_poll(&c))
				buf[n++] = c;
			r->eax = n;
		} else if (f->type == FD_PIPE_RD) {
			r->eax = (uint32_t)pipe_read(f->pipe, buf, (int)len);
		} else {
			r->eax = (uint32_t)-1;
		}
		break;
	}
	case SYS_UPTIME:
		r->eax = timer_seconds();
		break;
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
		r->eax = (uint32_t)fs_create(abs, (const void *)buf, len);
		break;
	}
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
	case SYS_NET_CLOSE:
		r->eax = (uint32_t)net_sock_close((int)r->ebx);
		break;
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
	case SYS_READDIR: {
		struct rd_out { char name[20]; int is_dir; uint32_t size; } *o;
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
		char nm[20];
		int isd = 0;
		uint32_t sz = 0;
		int got = fs_enum_dir(path[0] ? path : "/", (int)r->ecx, nm, &isd, &sz, 0);
		if (got) {
			int i = 0;
			for (; i < 19 && nm[i]; i++) o->name[i] = nm[i];
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
	default:
		r->eax = (uint32_t)-1;
		break;
	}
}
