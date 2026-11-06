/* lib.c: OwOS user library: syscall wrappers and small helpers. */

#include "owos.h"
#include "syscall_nr.h"

static inline int syscall(int num, int a, int b, int c)
{
  int ret;
  __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
  return ret;
}

int write(int fd, const void *buf, int len) { return syscall(SYS_WRITE, fd, (int)buf, len); }

int read(int fd, void *buf, int len) { return syscall(SYS_READ, fd, (int)buf, len); }

int pipe(int fds[2]) { return syscall(SYS_PIPE, (int)fds, 0, 0); }

int close(int fd) { return syscall(SYS_CLOSE, fd, 0, 0); }

void exit(int code)
{
  syscall(SYS_EXIT, code, 0, 0);
  for (;;)
    ; /* not reached                             */
}

int fork(void) { return syscall(SYS_FORK, 0, 0, 0); }

int execp(const char *name) { return syscall(SYS_EXEC, (int)name, 0, 0); }

int execp1(const char *name, const char *arg) { return syscall(SYS_EXEC, (int)name, (int)arg, 0); }

int waitpid(int pid) { return syscall(SYS_WAITPID, pid, 0, 0); }

int waitpid_nb(int pid, int *status) { return syscall(SYS_WAITPID, pid, (int)status, WNOHANG); }

unsigned uptime(void) { return msec() / 1000u; /* seconds since boot, derived from the ms counter */ }

unsigned msec(void) { return (unsigned)syscall(SYS_MSEC, 0, 0, 0); }

unsigned rtc_epoch(void) { return (unsigned)syscall(SYS_RTC_EPOCH, 0, 0, 0); }

/* Wall-clock milliseconds since the Unix epoch, the basis for JS Date.now().
 * The RTC has 1-second resolution, so we anchor to it once and track sub-second
 * time with the monotonic ms counter (which also keeps now_ms() monotonic). */
long long now_ms(void)
{
  static unsigned base_epoch = 0, base_msec = 0;
  if (base_epoch == 0) {
    base_epoch = rtc_epoch();
    base_msec = msec();
  }
  return (long long)base_epoch * 1000 + (long long)(msec() - base_msec);
}

int readfile(const char *name, char *buf, int max) { return syscall(SYS_READFILE, (int)name, (int)buf, max); }

int writefile(const char *name, const void *buf, int len) { return syscall(SYS_WRITEFILE, (int)name, (int)buf, len); }

int readfile_at(const char *name, unsigned off, void *buf, unsigned len)
{
  unsigned a[4] = {(unsigned)name, off, (unsigned)buf, len};
  return syscall(SYS_READFILE_AT, (int)a, 0, 0);
}

int stream_begin(const char *name) { return syscall(SYS_STREAM_BEGIN, (int)name, 0, 0); }

int stream_write(const void *buf, int len) { return syscall(SYS_STREAM_WRITE, (int)buf, len, 0); }

int stream_end(void) { return syscall(SYS_STREAM_END, 0, 0, 0); }

int net_connect(const char *host, int port, int tls) { return syscall(SYS_NET_CONNECT, (int)host, port, tls); }

/* All net send/recv variants use two syscalls with a 6-word request {fd, buf, len, flags, ip|0, port};
 * non-NULL ip selects addressed form, MSG_DONTWAIT a non-blocking recv. */
int net_send(int h, const void *buf, int len)
{
  unsigned a[6] = {(unsigned)h, (unsigned)buf, (unsigned)len, 0, 0, 0};
  return syscall(SYS_NET_SEND, (int)a, 0, 0);
}

int net_recv(int h, void *buf, int max)
{
  unsigned a[6] = {(unsigned)h, (unsigned)buf, (unsigned)max, 0, 0, 0};
  return syscall(SYS_NET_RECV, (int)a, 0, 0);
}

int net_recv_nb(int h, void *buf, int max)
{
  unsigned a[6] = {(unsigned)h, (unsigned)buf, (unsigned)max, MSG_DONTWAIT, 0, 0};
  return syscall(SYS_NET_RECV, (int)a, 0, 0);
}

int net_close(int h) { return syscall(SYS_NET_CLOSE, h, 0, 0); }

int net_set_ttl(int h, int ttl) { return syscall(SYS_NET_SETOPT, h, NET_OPT_TTL, ttl); }

int net_raw_open(void) { return net_connect(0, 0, SK_RAWETH); /* raw L2 socket via the unified connect */ }

int ifconfig(const unsigned char ip[4], const unsigned char mask[4], const unsigned char gw[4],
             const unsigned char dns[4])
{
  unsigned char rest[12]; /* {mask, gw, dns} packed for the single ecx ptr */
  for (int i = 0; i < 4; i++) {
    rest[i] = mask[i];
    rest[4 + i] = gw[i];
    rest[8 + i] = dns[i];
  }
  return syscall(SYS_IFCONFIG, (int)ip, (int)rest, 0);
}

int setrtc(unsigned secs) { return syscall(SYS_SETRTC, (int)secs, 0, 0); }

int net_listen(int port) { return syscall(SYS_NET_LISTEN, port, 0, 0); }

int net_accept(int listen_fd) { return syscall(SYS_NET_ACCEPT, listen_fd, 0, 0); }

int net_bind(int port) { return syscall(SYS_NET_BIND, port, 0, 0); }

int net_recvfrom(int fd, void *buf, int max, unsigned char src_ip[4], int *src_port)
{
  unsigned a[6] = {(unsigned)fd, (unsigned)buf, (unsigned)max, 0, (unsigned)src_ip, (unsigned)src_port};
  return syscall(SYS_NET_RECV, (int)a, 0, 0);
}

int net_sendto(int fd, const unsigned char ip[4], int port, const void *buf, int len)
{
  unsigned a[6] = {(unsigned)fd, (unsigned)buf, (unsigned)len, 0, (unsigned)ip, (unsigned)port};
  return syscall(SYS_NET_SEND, (int)a, 0, 0);
}

int win_create(int w, int h, const char *title) { return syscall(SYS_WIN_CREATE, w, h, (int)title); }

int win_present(int id) { return syscall(SYS_WIN_PRESENT, id, 0, 0); }

int win_present_rect(int id, int x, int y, int w, int h)
{
  if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF)
    return win_present(id);
  return syscall(SYS_WIN_PRESENT, id, x | y << 16, w | h << 16);
}

int win_poll(int id, struct win_event *ev) { return syscall(SYS_WIN_POLL, id, (int)ev, 0); }

void win_wait(int id) { syscall(SYS_WIN_POLL, id, 0, 1); /* ev=NULL, block=1: sleep ~one frame */ }

int win_close(int id) { return syscall(SYS_WIN_CLOSE, id, 0, 0); }

int win_owned(int pid) { return syscall(SYS_WIN_OWNS, pid, 0, 0); }

int win_set_parent(int id, int parent, int modal) { return syscall(SYS_WIN_SET_PARENT, id, parent, modal); }

unsigned *win_fb_of(int id)
{
  unsigned v = (unsigned)syscall(SYS_WIN_FB, id, 0, 0);
  return v ? (unsigned *)v : (unsigned *)0;
}

int win_settitle(int id, const char *title) { return syscall(SYS_WIN_TITLE, id, (int)title, 0); }

int win_resizable(int id, int on) { return syscall(SYS_WIN_RESIZABLE, id, on, 0); }

int win_cursor(int id, int shape) { return syscall(SYS_WIN_CURSOR, id, shape, 0); }

/* ---- Phase 1 user-space compositor wrappers (root-only) ---- */
int fb_info(struct fbinfo *out) { return syscall(SYS_FB_INFO, (int)out, 0, 0); }

unsigned *fb_map(void)
{
  int v = syscall(SYS_FB_MAP, 0, 0, 0);
  return v == -1 ? (unsigned *)0 : (unsigned *)v;
}

int fb_present_rects(const struct fbrect *r, int n) { return syscall(SYS_FB_PRESENT, (int)r, n, 0); }

int input_poll(struct inputstate *out) { return syscall(SYS_INPUT_POLL, (int)out, 0, 0); }

int input_set_bounds(int w, int h) { return syscall(SYS_INPUT_SET_BOUNDS, w, h, 0); }

int win_enum(struct winrec *out, int max) { return syscall(SYS_WIN_ENUM, (int)out, max, 0); }

int win_map(int id, struct winmap *m) { return syscall(SYS_WIN_MAP, id, (int)m, 0); }

int win_unmap(int id) { return syscall(SYS_WIN_UNMAP, id, 0, 0); }

int win_set_geom(int id, int x, int y, int w, int h)
{
  int g[4] = {x, y, w, h};
  return syscall(SYS_WIN_SET_GEOM, id, (int)g, 0);
}

int win_deliver(int id, const struct winevent *ev) { return syscall(SYS_WIN_DELIVER, id, (int)ev, 0); }

int win_key_space(int id) { return syscall(SYS_WIN_KEY_SPACE, id, 0, 0); }

int crash_poll(struct crash_rec *out, int max) { return syscall(SYS_CRASH_POLL, (int)out, max, 0); }

/* Packed (hour<<16)|(min<<8)|sec, UTC, decoded in user space from the epoch
 * primitive rather than a dedicated syscall. */
unsigned now_hms(void)
{
  unsigned s = rtc_epoch();
  unsigned sec = s % 60u, min = (s / 60u) % 60u, hour = (s / 3600u) % 24u;
  return (hour << 16) | (min << 8) | sec;
}

int readdir(const char *path, int idx, struct dirent_u *out) { return syscall(SYS_READDIR, (int)path, idx, (int)out); }

int mkdir(const char *path) { return syscall(SYS_MKDIR, (int)path, 0, 0); }
int rmdir(const char *path) { return syscall(SYS_RMDIR, (int)path, 0, 0); }
int unlink(const char *path) { return syscall(SYS_DELETE, (int)path, 0, 0); }
int rename(const char *old, const char *neu) { return syscall(SYS_RENAME, (int)old, (int)neu, 0); }

int stat(const char *path, struct statinfo *out) { return syscall(SYS_STAT, (int)path, (int)out, 0); }

int statfs(const char *path, struct statfs_info *out) { return syscall(SYS_STATFS, (int)path, (int)out, 0); }

int spawn(const char *path, const char *arg) { return syscall(SYS_SPAWN, (int)path, (int)arg, 0); }

/* spawn_cap with a full argv; the kernel uses `path` for the image. */
int spawn_cap(const char *path, char *const argv[], int argc, int *rfd)
{
  struct {
    const char *path;
    char *const *argv;
    int argc;
    int *rfd;
    int *wfd;
    int uid;
    int gid;
    const char *cwd;
  } req = {path, argv, argc, rfd, 0, -1, -1, 0};
  return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* Like spawn_cap, plus a stdin pipe whose write end is stored in *wfd. */
int spawn_cap_io(const char *path, char *const argv[], int argc, int *rfd, int *wfd)
{
  struct {
    const char *path;
    char *const *argv;
    int argc;
    int *rfd;
    int *wfd;
    int uid;
    int gid;
    const char *cwd;
  } req = {path, argv, argc, rfd, wfd, -1, -1, 0};
  *wfd = -1;
  return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* Like spawn_cap_io, plus uid/gid and cwd (uid/gid < 0 or cwd == 0 inherit).
 * Changing to another uid/gid is root-only. */
int spawn_cap_io_as(const char *path, char *const argv[], int argc, int uid, int gid, const char *cwd, int *rfd,
                    int *wfd)
{
  struct {
    const char *path;
    char *const *argv;
    int argc;
    int *rfd;
    int *wfd;
    int uid;
    int gid;
    const char *cwd;
  } req = {path, argv, argc, rfd, wfd, uid, gid, cwd};
  *wfd = -1;
  return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* Non-blocking read: >0 bytes, 0 if it would block, -1 at EOF. */
int read_nb(int fd, void *buf, int len) { return syscall(SYS_READ_NB, fd, (int)buf, len); }

int chdir(const char *path) { return syscall(SYS_CHDIR, (int)path, 0, 0); }

int kill(int pid, int sig) { return syscall(SYS_KILL, pid, sig, 0); }

/* action: 0 = shut down, 1 = reboot. Does not return on success. */
int power(int action) { return syscall(SYS_POWER, action, 0, 0); }

int getpid(void) { return syscall(SYS_GETPID, 0, 0, 0); }

/* sigprocmask with set==NULL only queries the mask via *old. */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
  return syscall(SYS_SIGACTION, sig, (int)act, (int)old);
}

sighandler_t signal(int sig, sighandler_t handler)
{
  struct sigaction act, old;
  act.handler = (unsigned)handler;
  act.mask = 0;
  act.flags = 0;
  if (sigaction(sig, &act, &old) < 0)
    return SIG_ERR;
  return (sighandler_t)old.handler;
}

unsigned sigprocmask(int how, const unsigned *set, unsigned *old)
{
  unsigned prev = (unsigned)syscall(SYS_SIGPROCMASK, how, set ? (int)*set : 0, set ? 1 : 0);
  if (old)
    *old = prev;
  return prev;
}

int raise(int sig) { return kill(getpid(), sig); }

int volumes(int idx, struct volinfo *out) { return syscall(SYS_VOLUMES, idx, (int)out, 0); }

int umount(const char *point) { return syscall(SYS_UMOUNT, (int)point, 0, 0); }

int mount(const char *dev, const char *point) { return syscall(SYS_MOUNT, (int)dev, (int)point, 0); }

int audio_open(void) { return syscall(SYS_AUDIO_OPEN, 0, 0, 0); }

int audio_write(const void *pcm, int len) { return syscall(SYS_AUDIO_WRITE, (int)pcm, len, 0); }

int audio_close(void) { return syscall(SYS_AUDIO_CLOSE, 0, 0, 0); }

void msleep(unsigned ms) { syscall(SYS_SLEEP, (int)ms, 0, 0); }

int thread_create(void (*fn)(void), void *stack_top) { return syscall(SYS_THREAD, (int)fn, (int)stack_top, 0); }

/* Stack with an unmapped guard page below, so overflow faults. Returns the top, or 0 (caller falls back to malloc). */
void *thread_stack_alloc(int bytes)
{
  int pages = (bytes + 4095) / 4096;
  return (void *)syscall(SYS_STACK_ALLOC, pages, 0, 0);
}
void thread_stack_free(void *top)
{
  if (top)
    syscall(SYS_STACK_FREE, (int)top, 0, 0);
}

/* ------------------------------------------------------- identity / hostname
 */
/* Per-process credentials. The kernel only hands out the numeric ids; the login
 * name and home are resolved here from /etc/passwd, the Unix way. */
int getuid(void) { return syscall(SYS_GETUID, 0, 0, 0); }

/* Copy the ':'-separated field #idx (0-based) of the line [s,end) into out. */
static void passwd_field(const char *s, const char *end, int idx, char *out, int max)
{
  int f = 0;
  while (f < idx && s < end) {
    if (*s == ':')
      f++;
    s++;
  }
  int i = 0;
  while (s < end && *s != ':' && i < max - 1)
    out[i++] = *s++;
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
  if (n <= 0)
    return 0;
  pw[n] = '\0';
  for (const char *p = pw; *p;) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    char uf[12];
    passwd_field(p, eol, 2, uf, sizeof uf);
    int u = 0;
    for (const char *c = uf; *c >= '0' && *c <= '9'; c++)
      u = u * 10 + (*c - '0');
    if (u == uid) {
      if (name)
        passwd_field(p, eol, 0, name, nmax);
      if (home)
        passwd_field(p, eol, 4, home, hmax);
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
    while (d[i] && i < max - 1) {
      buf[i] = d[i];
      i++;
    }
    buf[i] = '\0';
    return i;
  }
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = '\0';
  return n;
}

/* Persist a new system hostname to /etc/hostname. Returns 0/-1. */
int sethostname(const char *name) { return writefile("/etc/hostname", name, (int)strlen(name)) < 0 ? -1 : 0; }

/* POSIX uname(2). The kernel fills sysname/release/version/machine (machine is
 * its compile-time arch, the one fact a 32-bit user program can't know); we
 * add nodename from the hostname, keeping /etc/hostname the single source. */
int uname(struct utsname *out)
{
  if (syscall(SYS_UNAME, (int)out, 0, 0) != 0)
    return -1;
  gethostname(out->nodename, (int)sizeof out->nodename);
  return 0;
}

/* ----------------------------------------------------------- IPC: local ports
 */
/* Named local port: as_server=1 listens (then port_accept), 0 connects. */
int port_open(const char *name, int as_server) { return syscall(SYS_PORT_OPEN, (int)name, as_server, 0); }

int port_accept(int listen_fd) { return syscall(SYS_PORT_ACCEPT, listen_fd, 0, 0); }

/* ----------------------------------------------------------- malloc / free */
/* K&R first-fit allocator (§8.7) over SYS_SBRK, locked for threads. 8-byte block header;
 * the free list is address-ordered so free() coalesces. */

void *sbrk(int inc) { return (void *)syscall(SYS_SBRK, inc, 0, 0); }

typedef long long Align; /* 8-byte block + payload alignment */
union header {
  struct {
    union header *ptr;
    unsigned size;
  } s; /* size in header units */
  Align x;
};
typedef union header Header;

static Header base;   /* degenerate block: starts the list */
static Header *freep; /* roving free-list pointer          */

/* Exact-size LIFO bins for small blocks (<= SBIN_MAX units): O(1) free/malloc. The
 * address-ordered coalescing list made free() O(n), which froze the browser on DOM-heavy pages. */
#define SBIN_MAX 1024u
static Header *sbins[SBIN_MAX + 1];

/* Spinlock guarding the free list. xchg is atomic; on contention we yield to
 * the lock holder (single CPU, the timer also preempts a spinner as a
 * backstop). */
static volatile int heap_lock;
static inline int xchg32(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}
static void hlock(void)
{
  while (xchg32(&heap_lock, 1))
    msleep(0);
}
static void hunlock(void)
{
  __asm__ volatile("" ::: "memory");
  heap_lock = 0;
}

/* Crash recovery only: drop the heap lock a longjmp'd-over frame may have held.
 */
void heap_lock_reset(void)
{
  __asm__ volatile("" ::: "memory");
  heap_lock = 0;
}

#define NALLOC 2048u /* min header-units to request at once */

/* free() body without the lock (so morecore can reuse it under the held lock).
 */
static void free_nolock(void *ap)
{
  Header *bp = (Header *)ap - 1, *p;
  if (bp->s.size <= SBIN_MAX) { /* small block: O(1) bin push, no coalesce */
    bp->s.ptr = sbins[bp->s.size];
    sbins[bp->s.size] = bp;
    return;
  }
  for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;                         /* freed block at the list's edge   */
  if (bp + bp->s.size == p->s.ptr) { /* coalesce with the upper neighbour */
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else {
    bp->s.ptr = p->s.ptr;
  }
  if (p + p->s.size == bp) { /* coalesce with the lower neighbour */
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else {
    p->s.ptr = bp;
  }
  freep = p;
}

/* Grow the heap by at least `nu` header units via sbrk and fold it into the
 * free list. Returns the (updated) free pointer, or 0 if sbrk failed. */
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
  if (nbytes > 0xFFFFFFFFu - 2u * sizeof(Header)) /* size math would overflow */
    return 0;
  unsigned nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
  hlock();
  if (nunits <= SBIN_MAX && sbins[nunits]) { /* exact-size bin hit: O(1) reuse */
    Header *p = sbins[nunits];
    sbins[nunits] = p->s.ptr;
    hunlock();
    return (void *)(p + 1);
  }
  Header *prevp = freep;
  if (prevp == 0) { /* first call: one empty block       */
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for (Header *p = prevp->s.ptr;; prevp = p, p = p->s.ptr) {
    if (p->s.size >= nunits) { /* big enough                        */
      if (p->s.size == nunits) {
        prevp->s.ptr = p->s.ptr; /* exact fit: unlink                 */
      } else {
        p->s.size -= nunits; /* split: carve the tail             */
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      hunlock();
      return (void *)(p + 1);
    }
    if (p == freep) { /* wrapped the list: grow the heap   */
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
  if (size && total / size != nmemb) /* overflow                          */
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
  /* Old payload = (block units - 1) header-sized cells. */
  Header *h = (Header *)ptr - 1;
  unsigned oldbytes = (h->s.size - 1) * sizeof(Header);
  if (size <= oldbytes)
    return ptr;
  void *np = malloc(size);
  if (!np)
    return 0;
  memcpy(np, ptr, oldbytes < size ? oldbytes : size);
  free(ptr);
  return np;
}

/* mem*: shared by every program (gcc lowers struct copies / array inits to
 * these at -O0) and by libs like minimp3 and the HTTP/2 HPACK tables. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
  int d0, d1, d2;
  __asm__ volatile("rep ; movsl\n\t"
                   "movl %4, %%ecx\n\t"
                   "andl $3, %%ecx\n\t"
                   "rep ; movsb"
                   : "=&c"(d0), "=&D"(d1), "=&S"(d2)
                   : "0"((unsigned)n >> 2), "g"((unsigned)n), "1"(d), "2"(s)
                   : "memory");
  return d;
}
void *memset(void *d, int c, __SIZE_TYPE__ n)
{
  int d0, d1;
  unsigned cc = (unsigned char)c;
  unsigned pat = cc | (cc << 8) | (cc << 16) | (cc << 24);
  __asm__ volatile("rep ; stosl\n\t"
                   "movl %4, %%ecx\n\t"
                   "andl $3, %%ecx\n\t"
                   "rep ; stosb"
                   : "=&c"(d0), "=&D"(d1)
                   : "a"(pat), "0"((unsigned)n >> 2), "g"((unsigned)n), "1"(d)
                   : "memory");
  return d;
}
void *memmove(void *d, const void *s, __SIZE_TYPE__ n)
{
  unsigned char *a = d;
  const unsigned char *b = s;
  if (a < b) {
    return memcpy(d, s, n);
  } else if (a > b) {
    a += n;
    b += n;
    while (n--)
      *--a = *--b;
  }
  return d;
}

int memcmp(const void *a, const void *b, __SIZE_TYPE__ n)
{
  const unsigned char *p = a, *q = b;
  for (; n; n--, p++, q++)
    if (*p != *q)
      return (int)*p - (int)*q;
  return 0;
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
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void strcpy(char *d, const char *s)
{
  while ((*d++ = *s++))
    ;
}

void strlcpy(char *dst, const char *src, int size)
{
  int i = 0;
  if (size <= 0)
    return;
  if (src)
    for (; src[i] && i < size - 1; i++)
      dst[i] = src[i];
  dst[i] = '\0';
}

void strcat(char *d, const char *s)
{
  while (*d)
    d++;
  while ((*d++ = *s++))
    ;
}

/* ------------------------------------------------------------ UTF-8 codec ---
 * Decoding is tolerant: bad/truncated sequences yield U+FFFD and advance one
 * byte. Encode does not special-case surrogates or > U+10FFFF. */

int utf8_seqlen(unsigned char lead)
{
  if (lead < 0x80)
    return 1;
  if ((lead & 0xE0) == 0xC0)
    return 2;
  if ((lead & 0xF0) == 0xE0)
    return 3;
  if ((lead & 0xF8) == 0xF0)
    return 4;
  return 1; /* continuation / invalid lead */
}

int utf8_decode(const char *s, const char *end, unsigned *cp)
{
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && s + 1 < end && (s[1] & 0xC0) == 0x80) {
    *cp = ((unsigned)(c & 0x1F) << 6) | (unsigned char)(s[1] & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && s + 2 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned char)(s[2] & 0x3F);
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && s + 3 < end && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    *cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) |
          (unsigned char)(s[3] & 0x3F);
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

void puts(const char *s) { write(1, s, strlen(s)); }

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

/* ----------------------------------------------------------- printf family */

/* A minimal freestanding printf, shared by snprintf and printf, it writes one
 * byte at a time through `sink` so callers needn't size a buffer. Mirrors the
 * kernel's kvformat (kstring.c); see owos.h for the supported conversions. */
static void emit_uint(void (*sink)(char, void *), void *ctx, unsigned v, unsigned base, int upper, int width, int zero,
                      int neg)
{
  const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[32];
  int n = 0;
  do {
    tmp[n++] = digs[v % base];
    v /= base;
  } while (v);
  int total = n + (neg ? 1 : 0);
  if (zero) { /* "-0007": sign, then zero padding */
    if (neg)
      sink('-', ctx);
    for (int pad = width - total; pad > 0; pad--)
      sink('0', ctx);
  } else { /* "  -7": space padding, then sign */
    for (int pad = width - total; pad > 0; pad--)
      sink(' ', ctx);
    if (neg)
      sink('-', ctx);
  }
  while (n--)
    sink(tmp[n], ctx);
}

static void vfmt(void (*sink)(char, void *), void *ctx, const char *fmt, va_list ap)
{
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      sink(*fmt, ctx);
      continue;
    }
    fmt++;
    int zero = 0, width = 0;
    if (*fmt == '0') {
      zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }
    switch (*fmt) {
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      while (*s)
        sink(*s++, ctx);
      break;
    }
    case 'c':
      sink((char)va_arg(ap, int), ctx);
      break;
    case 'd':
    case 'i': {
      int v = va_arg(ap, int), neg = v < 0;
      emit_uint(sink, ctx, neg ? -(unsigned)v : (unsigned)v, 10, 0, width, zero, neg);
      break;
    }
    case 'u':
      emit_uint(sink, ctx, va_arg(ap, unsigned), 10, 0, width, zero, 0);
      break;
    case 'x':
      emit_uint(sink, ctx, va_arg(ap, unsigned), 16, 0, width, zero, 0);
      break;
    case 'X':
      emit_uint(sink, ctx, va_arg(ap, unsigned), 16, 1, width, zero, 0);
      break;
    case 'p':
      sink('0', ctx);
      sink('x', ctx);
      emit_uint(sink, ctx, (unsigned)(__SIZE_TYPE__)va_arg(ap, void *), 16, 0, 8, 1, 0);
      break;
    case '%':
      sink('%', ctx);
      break;
    case '\0':
      return; /* trailing '%', stop */
    default:
      sink('%', ctx);
      sink(*fmt, ctx);
      break;
    }
  }
}

/* snprintf sink: write into a buffer, but keep counting past the end so the
 * return value reports the would-be length (C99). */
struct fmt_buf {
  char *p;
  unsigned len, cap;
};
static void buf_put(char c, void *ctx)
{
  struct fmt_buf *b = ctx;
  if (b->len + 1 < b->cap) /* leave room for the NUL */
    b->p[b->len] = c;
  b->len++;
}

int vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap)
{
  struct fmt_buf b = {buf, 0, size};
  vfmt(buf_put, &b, fmt, ap);
  if (size)
    buf[b.len < size ? b.len : size - 1] = '\0';
  return (int)b.len;
}

int snprintf(char *buf, unsigned size, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

/* printf sink: accumulate into a buffer and write(1) in chunks, so an
 * arbitrary- length format costs a handful of syscalls instead of one per
 * character. */
struct out_buf {
  char b[256];
  int n;
  int total;
};
static void out_flush(struct out_buf *o)
{
  if (o->n) {
    write(1, o->b, o->n);
    o->n = 0;
  }
}
static void out_put(char c, void *ctx)
{
  struct out_buf *o = ctx;
  o->b[o->n++] = c;
  o->total++;
  if (o->n == (int)sizeof o->b)
    out_flush(o);
}

int printf(const char *fmt, ...)
{
  struct out_buf o = {.n = 0, .total = 0};
  va_list ap;
  va_start(ap, fmt);
  vfmt(out_put, &o, fmt, ap);
  va_end(ap);
  out_flush(&o);
  return o.total;
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
    if (c == '\b') { /* backspace                       */
      if (len > 0) {
        len--;
        write(1, "\b \b", 3);
      }
      continue;
    }
    if (len < max - 1) {
      buf[len++] = c;
      write(1, &c, 1); /* echo                            */
    }
  }
  buf[len] = '\0';
  return len;
}

/* ------------------------------------------------------- randomness ------- *
 * rng_bytes: RDRAND run directly in ring 3; without it, falls back to a
 * clock/TSC-seeded SplitMix64 (NOT cryptographic). */
static int rng_have_rdrand(void)
{
  static int cached = -1;
  if (cached < 0) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    cached = (int)((c >> 30) & 1u); /* CPUID.01H:ECX.RDRAND[bit 30] */
  }
  return cached;
}
static int rng_rdrand32(unsigned *out)
{
  unsigned char ok = 0;
  for (int i = 0; i < 10; i++) { /* retry a momentarily-stalled RDRAND */
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    if (ok)
      return 1;
  }
  return 0;
}
static unsigned long long rng_tsc(void)
{
  unsigned lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)hi << 32) | lo;
}

/* getrandom(buf, len): CSPRNG bytes from the kernel entropy pool (interrupt
 * jitter, ratcheted through SHA-256). Returns bytes written, or -1. */
int getrandom(void *buf, int len) { return syscall(SYS_GETRANDOM, (int)buf, len, 0); }

void rng_bytes(void *out, int n)
{
  static unsigned long long st;
  static int seeded;
  unsigned char *p = (unsigned char *)out;
  int i = 0;
  if (n <= 0)
    return;
  if (rng_have_rdrand()) { /* hardware CSPRNG: the real path */
    while (i < n) {
      unsigned r;
      int k = n - i < 4 ? n - i : 4;
      if (!rng_rdrand32(&r))
        break; /* persistent stall: top up below */
      memcpy(p + i, &r, (unsigned)k);
      i += k;
    }
    if (i >= n)
      return;
  }
  /* No RDRAND, or it stalled mid-buffer: fill the rest from the kernel entropy
   * pool. This is the CSPRNG path on hardware without RDRAND, the clock-seeded
   * SplitMix below is not cryptographic and must never carry key material. */
  while (i < n) {
    int k = n - i < 256 ? n - i : 256;
    if (getrandom(p + i, k) != k)
      break; /* syscall unavailable: last resort */
    i += k;
  }
  if (i >= n)
    return;
  if (!seeded) { /* fallback seed: every clock + the TSC */
    unsigned long long s = (unsigned long long)now_ms();
    s ^= ((unsigned long long)msec() << 21) ^ ((unsigned long long)uptime() << 32) ^
         ((unsigned long long)rtc_epoch() << 13);
    s ^= rng_tsc() ^ (unsigned long long)(unsigned long)(void *)&s;
    st = s ? s : 0x9E3779B97F4A7C15ULL;
    seeded = 1;
  }
  st ^= rng_tsc() ^ (unsigned long long)now_ms();
  while (i < n) {
    unsigned long long z = (st += 0x9E3779B97F4A7C15ULL);
    int k = n - i < 8 ? n - i : 8;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    memcpy(p + i, &z, (unsigned)k);
    i += k;
  }
}
