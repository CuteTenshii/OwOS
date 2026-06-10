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

int waitpid(int pid)
{
	return syscall(SYS_WAITPID, pid, 0, 0);
}

unsigned uptime(void)
{
	return (unsigned)syscall(SYS_UPTIME, 0, 0, 0);
}

int readfile(const char *name, char *buf, int max)
{
	return syscall(SYS_READFILE, (int)name, (int)buf, max);
}

int writefile(const char *name, const void *buf, int len)
{
	return syscall(SYS_WRITEFILE, (int)name, (int)buf, len);
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
