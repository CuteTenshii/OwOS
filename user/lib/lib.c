/* lib.c — OwOS user library: syscall wrappers and small helpers. */

#include "owos.h"

#define SYS_WRITE    1
#define SYS_EXIT     2
#define SYS_READ     3
#define SYS_FORK     4
#define SYS_UPTIME   5
#define SYS_READFILE 6
#define SYS_EXEC     7
#define SYS_WAITPID  8
#define SYS_PIPE     9
#define SYS_CLOSE    10
#define SYS_WRITEFILE 11
#define SYS_NET_CONNECT 12
#define SYS_NET_SEND   13
#define SYS_NET_RECV   14
#define SYS_NET_CLOSE  15
#define SYS_WIN_CREATE 16
#define SYS_WIN_PRESENT 17
#define SYS_WIN_POLL   18
#define SYS_WIN_WAIT   19
#define SYS_WIN_CLOSE  20
#define SYS_RTC        21
#define SYS_READDIR    22
#define SYS_WIN_TITLE  23
#define SYS_THREAD     24
#define SYS_SLEEP      25
#define SYS_WIN_RESIZABLE 26
#define SYS_MSEC       27
#define SYS_READFILE_AT 28
#define SYS_VOLUMES    29
#define SYS_EJECT      30
#define SYS_WIN_CURSOR 31

static inline int syscall(int num, int a, int b, int c)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(num), "b"(a), "c"(b), "d"(c)
	                  : "memory");
	return ret;
}

int write(int fd, const void *buf, int len)
{
	return syscall(SYS_WRITE, fd, (int)buf, len);
}

int read(int fd, void *buf, int len)
{
	return syscall(SYS_READ, fd, (int)buf, len);
}

int pipe(int fds[2])
{
	return syscall(SYS_PIPE, (int)fds, 0, 0);
}

int close(int fd)
{
	return syscall(SYS_CLOSE, fd, 0, 0);
}

void exit(int code)
{
	syscall(SYS_EXIT, code, 0, 0);
	for (;;)
		;                          /* not reached                             */
}

int fork(void)
{
	return syscall(SYS_FORK, 0, 0, 0);
}

int execp(const char *name)
{
	return syscall(SYS_EXEC, (int)name, 0, 0);
}

int execp1(const char *name, const char *arg)
{
	return syscall(SYS_EXEC, (int)name, (int)arg, 0);
}

int waitpid(int pid)
{
	return syscall(SYS_WAITPID, pid, 0, 0);
}

unsigned uptime(void)
{
	return (unsigned)syscall(SYS_UPTIME, 0, 0, 0);
}

unsigned msec(void)
{
	return (unsigned)syscall(SYS_MSEC, 0, 0, 0);
}

int readfile(const char *name, char *buf, int max)
{
	return syscall(SYS_READFILE, (int)name, (int)buf, max);
}

int writefile(const char *name, const void *buf, int len)
{
	return syscall(SYS_WRITEFILE, (int)name, (int)buf, len);
}

int readfile_at(const char *name, unsigned off, void *buf, unsigned len)
{
	unsigned a[4] = { (unsigned)name, off, (unsigned)buf, len };
	return syscall(SYS_READFILE_AT, (int)a, 0, 0);
}

int net_connect(const char *host, int port, int tls)
{
	return syscall(SYS_NET_CONNECT, (int)host, port, tls);
}

int net_send(int h, const void *buf, int len)
{
	return syscall(SYS_NET_SEND, h, (int)buf, len);
}

int net_recv(int h, void *buf, int max)
{
	return syscall(SYS_NET_RECV, h, (int)buf, max);
}

int net_close(int h)
{
	return syscall(SYS_NET_CLOSE, h, 0, 0);
}

int win_create(int w, int h, const char *title)
{
	return syscall(SYS_WIN_CREATE, w, h, (int)title);
}

int win_present(int id)
{
	return syscall(SYS_WIN_PRESENT, id, 0, 0);
}

int win_poll(int id, struct win_event *ev)
{
	return syscall(SYS_WIN_POLL, id, (int)ev, 0);
}

void win_wait(int id)
{
	syscall(SYS_WIN_WAIT, id, 0, 0);
}

int win_close(int id)
{
	return syscall(SYS_WIN_CLOSE, id, 0, 0);
}

int win_settitle(int id, const char *title)
{
	return syscall(SYS_WIN_TITLE, id, (int)title, 0);
}

int win_resizable(int id, int on)
{
	return syscall(SYS_WIN_RESIZABLE, id, on, 0);
}

int win_cursor(int id, int shape)
{
	return syscall(SYS_WIN_CURSOR, id, shape, 0);
}

unsigned now_hms(void)
{
	return (unsigned)syscall(SYS_RTC, 0, 0, 0);
}

int readdir(const char *path, int idx, struct dirent_u *out)
{
	return syscall(SYS_READDIR, (int)path, idx, (int)out);
}

int volumes(int idx, struct volinfo *out)
{
	return syscall(SYS_VOLUMES, idx, (int)out, 0);
}

int eject(const char *path)
{
	return syscall(SYS_EJECT, (int)path, 0, 0);
}

void msleep(unsigned ms)
{
	syscall(SYS_SLEEP, (int)ms, 0, 0);
}

int thread_create(void (*fn)(void), void *stack_top)
{
	return syscall(SYS_THREAD, (int)fn, (int)stack_top, 0);
}

static int slen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

void puts(const char *s)
{
	write(1, s, slen(s));
}

void print_uint(unsigned v)
{
	char tmp[10];
	int i = 0;
	if (v == 0) {
		write(1, "0", 1);
		return;
	}
	while (v) {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i--)
		write(1, &tmp[i], 1);
}

int readline(char *buf, int max)
{
	int len = 0;
	for (;;) {
		char c;
		if (read(0, &c, 1) <= 0)
			break;
		if (c == '\n' || c == '\r') {
			write(1, "\n", 1);
			break;
		}
		if (c == '\b') {                   /* backspace                       */
			if (len > 0) {
				len--;
				write(1, "\b \b", 3);
			}
			continue;
		}
		if (len < max - 1) {
			buf[len++] = c;
			write(1, &c, 1);               /* echo                            */
		}
	}
	buf[len] = '\0';
	return len;
}
