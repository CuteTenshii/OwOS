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
#define SYS_RTC_EPOCH  58
#define SYS_READDIR    22
#define SYS_WIN_TITLE  23
#define SYS_THREAD     24
#define SYS_SLEEP      25
#define SYS_WIN_RESIZABLE 26
#define SYS_MSEC       27
#define SYS_READFILE_AT 28
#define SYS_VOLUMES    29
#define SYS_UMOUNT     30
#define SYS_WIN_CURSOR 31
#define SYS_STREAM_BEGIN 32
#define SYS_STREAM_WRITE 33
#define SYS_STREAM_END   34
#define SYS_MKDIR      35
#define SYS_RMDIR      36
#define SYS_DELETE     37
#define SYS_RENAME     38
#define SYS_STAT       39
#define SYS_SPAWN      40
#define SYS_MOUNT      41
#define SYS_AUDIO_OPEN 42
#define SYS_AUDIO_WRITE 43
#define SYS_AUDIO_CLOSE 44
#define SYS_INTR       45
#define SYS_NET_ALPN   46
#define SYS_SPAWN_CAP  47
#define SYS_READ_NB    48
#define SYS_CHDIR      49
#define SYS_KILL       50
#define SYS_POWER      51
#define SYS_SBRK       52
#define SYS_NET_RECV_NB 53
#define SYS_GETUID     54
#define SYS_GETGID     55
#define SYS_PORT_OPEN  56
#define SYS_PORT_ACCEPT 57

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

unsigned rtc_epoch(void)
{
	return (unsigned)syscall(SYS_RTC_EPOCH, 0, 0, 0);
}

/* Wall-clock milliseconds since the Unix epoch — the basis for JS Date.now(). The
 * RTC has 1-second resolution, so we anchor to it once and track sub-second time
 * with the monotonic ms counter (which also keeps now_ms() monotonic). */
long long now_ms(void)
{
	static unsigned base_epoch = 0, base_msec = 0;
	if (base_epoch == 0) { base_epoch = rtc_epoch(); base_msec = msec(); }
	return (long long)base_epoch * 1000 + (long long)(msec() - base_msec);
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

int stream_begin(const char *name)
{
	return syscall(SYS_STREAM_BEGIN, (int)name, 0, 0);
}

int stream_write(const void *buf, int len)
{
	return syscall(SYS_STREAM_WRITE, (int)buf, len, 0);
}

int stream_end(void)
{
	return syscall(SYS_STREAM_END, 0, 0, 0);
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

int net_recv_nb(int h, void *buf, int max)
{
	return syscall(SYS_NET_RECV_NB, h, (int)buf, max);
}

int net_close(int h)
{
	return syscall(SYS_NET_CLOSE, h, 0, 0);
}

int net_alpn(int h)
{
	return syscall(SYS_NET_ALPN, h, 0, 0);
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

int mkdir(const char *path)  { return syscall(SYS_MKDIR,  (int)path, 0, 0); }
int rmdir(const char *path)  { return syscall(SYS_RMDIR,  (int)path, 0, 0); }
int unlink(const char *path) { return syscall(SYS_DELETE, (int)path, 0, 0); }
int rename(const char *old, const char *neu)
{
	return syscall(SYS_RENAME, (int)old, (int)neu, 0);
}

int stat(const char *path, struct statinfo *out)
{
	return syscall(SYS_STAT, (int)path, (int)out, 0);
}

int spawn(const char *path, const char *arg)
{
	return syscall(SYS_SPAWN, (int)path, (int)arg, 0);
}

/* Spawn a /bin child capturing its stdout into a pipe, passing a full argv
 * vector (argv[0] is conventionally the program name; the kernel uses `path`
 * for the image and argv[1..] as arguments). On success returns the pid and
 * stores the read-end fd in *rfd; drain it with read_nb until EOF. */
int spawn_cap(const char *path, char *const argv[], int argc, int *rfd)
{
	struct { const char *path; char *const *argv; int argc; int *rfd; int *wfd; } req =
		{ path, argv, argc, rfd, 0 };
	return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* Like spawn_cap, but also wires the child's stdin (fd 0) to a pipe whose write
 * end is stored in *wfd. Feed the child keystrokes by write()'ing into *wfd; the
 * child reads them with read()/read_nb on fd 0. For interactive children (ssh)
 * whose input the terminal forwards from its own keystrokes. */
int spawn_cap_io(const char *path, char *const argv[], int argc, int *rfd, int *wfd)
{
	struct { const char *path; char *const *argv; int argc; int *rfd; int *wfd; } req =
		{ path, argv, argc, rfd, wfd };
	*wfd = -1;
	return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* Non-blocking read: >0 bytes, 0 if it would block, -1 at EOF. */
int read_nb(int fd, void *buf, int len)
{
	return syscall(SYS_READ_NB, fd, (int)buf, len);
}

int chdir(const char *path)
{
	return syscall(SYS_CHDIR, (int)path, 0, 0);
}

int kill(int pid)
{
	return syscall(SYS_KILL, pid, 0, 0);
}

/* action: 0 = shut down, 1 = reboot. Does not return on success. */
int power(int action)
{
	return syscall(SYS_POWER, action, 0, 0);
}

int intr_take(void)
{
	return syscall(SYS_INTR, 0, 0, 0);
}


int volumes(int idx, struct volinfo *out)
{
	return syscall(SYS_VOLUMES, idx, (int)out, 0);
}

int umount(const char *point)
{
	return syscall(SYS_UMOUNT, (int)point, 0, 0);
}

int mount(const char *dev, const char *point)
{
	return syscall(SYS_MOUNT, (int)dev, (int)point, 0);
}



int audio_open(void)
{
	return syscall(SYS_AUDIO_OPEN, 0, 0, 0);
}

int audio_write(const void *pcm, int len)
{
	return syscall(SYS_AUDIO_WRITE, (int)pcm, len, 0);
}

int audio_close(void)
{
	return syscall(SYS_AUDIO_CLOSE, 0, 0, 0);
}

void msleep(unsigned ms)
{
	syscall(SYS_SLEEP, (int)ms, 0, 0);
}

int thread_create(void (*fn)(void), void *stack_top)
{
	return syscall(SYS_THREAD, (int)fn, (int)stack_top, 0);
}

/* ------------------------------------------------------- identity / hostname */
/* Per-process credentials. The kernel only hands out the numeric ids; the login
 * name and home are resolved here from /etc/passwd, the Unix way. */
int getuid(void) { return syscall(SYS_GETUID, 0, 0, 0); }
int getgid(void) { return syscall(SYS_GETGID, 0, 0, 0); }

/* Copy the ':'-separated field #idx (0-based) of the line [s,end) into out. */
static void passwd_field(const char *s, const char *end, int idx, char *out, int max)
{
	int f = 0;
	while (f < idx && s < end) { if (*s == ':') f++; s++; }
	int i = 0;
	while (s < end && *s != ':' && i < max - 1) out[i++] = *s++;
	out[i] = '\0';
}

/* Resolve the calling process's login from its uid via /etc/passwd
 * (name:pwhash:uid:gid:home). `name` and/or `home` may be NULL. Returns 1 if a
 * matching account was found, 0 otherwise. */
int user_ident(char *name, int nmax, char *home, int hmax)
{
	static char pw[2048];
	int uid = getuid();
	int n = readfile("/etc/passwd", pw, (int)sizeof pw - 1);
	if (n <= 0) return 0;
	pw[n] = '\0';
	for (const char *p = pw; *p; ) {
		const char *eol = p;
		while (*eol && *eol != '\n') eol++;
		char uf[12];
		passwd_field(p, eol, 2, uf, sizeof uf);
		int u = 0;
		for (const char *c = uf; *c >= '0' && *c <= '9'; c++) u = u * 10 + (*c - '0');
		if (u == uid) {
			if (name) passwd_field(p, eol, 0, name, nmax);
			if (home) passwd_field(p, eol, 4, home, hmax);
			return 1;
		}
		p = (*eol == '\n') ? eol + 1 : eol;
	}
	return 0;
}

/* The system hostname from /etc/hostname (default "owos" when unset). Returns
 * the length written. */
int gethostname(char *buf, int max)
{
	int n = readfile("/etc/hostname", buf, max - 1);
	if (n <= 0) {
		const char *d = "owos";
		int i = 0;
		while (d[i] && i < max - 1) { buf[i] = d[i]; i++; }
		buf[i] = '\0';
		return i;
	}
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
	buf[n] = '\0';
	return n;
}

/* Persist a new system hostname to /etc/hostname. Returns 0/-1. */
int sethostname(const char *name)
{
	return writefile("/etc/hostname", name, (int)strlen(name)) < 0 ? -1 : 0;
}

/* ----------------------------------------------------------- IPC: local ports */
/* A named local port (the unix-socket analog). A server opens its name with
 * as_server=1 then port_accept()s connections; a client opens the same name
 * with as_server=0 to connect. Both get a bidirectional fd that read()/write()
 * and close() drive like any other. */
int port_open(const char *name, int as_server)
{
	return syscall(SYS_PORT_OPEN, (int)name, as_server, 0);
}

int port_accept(int listen_fd)
{
	return syscall(SYS_PORT_ACCEPT, listen_fd, 0, 0);
}

/* ----------------------------------------------------------- malloc / free */
/* A K&R-style first-fit allocator (The C Programming Language §8.7) over the
 * SYS_SBRK heap, with a small lock so the threads of one process (e.g. the
 * browser's UI + fetch worker) can allocate concurrently — they share the one
 * heap because they share the address space. Blocks carry an 8-byte header; the
 * free list is kept address-ordered so free() coalesces neighbours. */

void *sbrk(int inc) { return (void *)syscall(SYS_SBRK, inc, 0, 0); }

typedef long long Align;                  /* 8-byte block + payload alignment */
union header {
	struct { union header *ptr; unsigned size; } s;   /* size in header units */
	Align x;
};
typedef union header Header;

static Header  base;                       /* degenerate block: starts the list */
static Header *freep;                       /* roving free-list pointer          */

/* Spinlock guarding the free list. xchg is atomic; on contention we yield to the
 * lock holder (single CPU — the timer also preempts a spinner as a backstop). */
static volatile int heap_lock;
static inline int xchg32(volatile int *p, int v)
{
	__asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
	return v;
}
static void hlock(void)   { while (xchg32(&heap_lock, 1)) msleep(0); }
static void hunlock(void) { __asm__ volatile ("" ::: "memory"); heap_lock = 0; }

#define NALLOC 2048u                       /* min header-units to request at once */

/* free() body without the lock (so morecore can reuse it under the held lock). */
static void free_nolock(void *ap)
{
	Header *bp = (Header *)ap - 1, *p;
	for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
		if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
			break;                         /* freed block at the list's edge   */
	if (bp + bp->s.size == p->s.ptr) {     /* coalesce with the upper neighbour */
		bp->s.size += p->s.ptr->s.size;
		bp->s.ptr   = p->s.ptr->s.ptr;
	} else {
		bp->s.ptr = p->s.ptr;
	}
	if (p + p->s.size == bp) {             /* coalesce with the lower neighbour */
		p->s.size += bp->s.size;
		p->s.ptr   = bp->s.ptr;
	} else {
		p->s.ptr = bp;
	}
	freep = p;
}

/* Grow the heap by at least `nu` header units via sbrk and fold it into the free
 * list. Returns the (updated) free pointer, or 0 if sbrk failed. */
static Header *morecore(unsigned nu)
{
	if (nu < NALLOC)
		nu = NALLOC;
	void *cp = sbrk((int)(nu * sizeof(Header)));
	if (cp == (void *)-1)
		return 0;
	Header *up = (Header *)cp;
	up->s.size = nu;
	free_nolock((void *)(up + 1));
	return freep;
}

void *malloc(unsigned nbytes)
{
	if (nbytes == 0)
		return 0;
	if (nbytes > 0xFFFFFFFFu - 2u * sizeof(Header))   /* size math would overflow */
		return 0;
	unsigned nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
	hlock();
	Header *prevp = freep;
	if (prevp == 0) {                      /* first call: one empty block       */
		base.s.ptr = freep = prevp = &base;
		base.s.size = 0;
	}
	for (Header *p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
		if (p->s.size >= nunits) {         /* big enough                        */
			if (p->s.size == nunits) {
				prevp->s.ptr = p->s.ptr;   /* exact fit: unlink                 */
			} else {
				p->s.size -= nunits;       /* split: carve the tail             */
				p += p->s.size;
				p->s.size = nunits;
			}
			freep = prevp;
			hunlock();
			return (void *)(p + 1);
		}
		if (p == freep) {                  /* wrapped the list: grow the heap   */
			if ((p = morecore(nunits)) == 0) {
				hunlock();
				return 0;
			}
		}
	}
}

void free(void *ptr)
{
	if (!ptr)
		return;
	hlock();
	free_nolock(ptr);
	hunlock();
}

void *calloc(unsigned nmemb, unsigned size)
{
	unsigned total = nmemb * size;
	if (size && total / size != nmemb)     /* overflow                          */
		return 0;
	void *p = malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}

void *realloc(void *ptr, unsigned size)
{
	if (!ptr)
		return malloc(size);
	if (size == 0) {
		free(ptr);
		return 0;
	}
	/* Old payload = (block units - 1) header-sized cells. Copy the smaller of the
	 * old and new sizes into a fresh block, then release the old one. */
	Header *h = (Header *)ptr - 1;
	unsigned oldbytes = (h->s.size - 1) * sizeof(Header);
	void *np = malloc(size);
	if (!np)
		return 0;
	memcpy(np, ptr, oldbytes < size ? oldbytes : size);
	free(ptr);
	return np;
}

/* mem* — shared by every program (gcc lowers struct copies / array inits to
 * these at -O0) and by libs like minimp3 and the HTTP/2 HPACK tables. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
	unsigned char *a = d; const unsigned char *b = s;
	while (n--) *a++ = *b++;
	return d;
}
void *memset(void *d, int c, __SIZE_TYPE__ n)
{
	unsigned char *a = d;
	while (n--) *a++ = (unsigned char)c;
	return d;
}
void *memmove(void *d, const void *s, __SIZE_TYPE__ n)
{
	unsigned char *a = d; const unsigned char *b = s;
	if (a < b) { while (n--) *a++ = *b++; }
	else if (a > b) { a += n; b += n; while (n--) *--a = *--b; }
	return d;
}

int strlen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void strcpy(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

void strcat(char *d, const char *s)
{
	while (*d)
		d++;
	while ((*d++ = *s++))
		;
}

/* ------------------------------------------------------------ UTF-8 codec ---
 * System-wide so any program handles non-Latin / multibyte text uniformly: the
 * browser renders it, an editor counts/moves by codepoint, the shell echoes it.
 * Encoding is the canonical 1..4-byte form; decoding is tolerant (bad/truncated
 * sequences yield U+FFFD and advance one byte, so a malformed stream never
 * stalls). Codepoints above U+10FFFF or in the surrogate range are not special-
 * cased on encode — callers pass real scalar values. */

int utf8_seqlen(unsigned char lead)
{
	if (lead < 0x80) return 1;
	if ((lead & 0xE0) == 0xC0) return 2;
	if ((lead & 0xF0) == 0xE0) return 3;
	if ((lead & 0xF8) == 0xF0) return 4;
	return 1;                                  /* continuation / invalid lead */
}

int utf8_decode(const char *s, const char *end, unsigned *cp)
{
	unsigned char c = (unsigned char)s[0];
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xE0) == 0xC0 && s + 1 < end && (s[1] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x1F) << 6) | (unsigned char)(s[1] & 0x3F);
		return 2;
	}
	if ((c & 0xF0) == 0xE0 && s + 2 < end &&
	    (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x0F) << 12) |
		      ((unsigned)(s[1] & 0x3F) << 6) | (unsigned char)(s[2] & 0x3F);
		return 3;
	}
	if ((c & 0xF8) == 0xF0 && s + 3 < end &&
	    (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
		      ((unsigned)(s[2] & 0x3F) << 6) | (unsigned char)(s[3] & 0x3F);
		return 4;
	}
	*cp = 0xFFFD;
	return 1;
}

int utf8_encode(unsigned cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

void puts(const char *s)
{
	write(1, s, strlen(s));
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
