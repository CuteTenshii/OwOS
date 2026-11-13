/* lib.c: OwOS user library: syscall wrappers and small helpers */

#include "mutex.h"
#include "opt.h"
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
    ; /* not reached */
}

int fork(void) { return syscall(SYS_FORK, 0, 0, 0); }

int execp(const char *name) { return syscall(SYS_EXEC, (int)name, 0, 0); }

int execp1(const char *name, const char *arg) { return syscall(SYS_EXEC, (int)name, (int)arg, 0); }

int waitpid(int pid) { return syscall(SYS_WAITPID, pid, 0, 0); }

int waitpid_nb(int pid, int *status) { return syscall(SYS_WAITPID, pid, (int)status, WNOHANG); }

unsigned uptime(void) { return msec() / 1000u; /* seconds since boot, derived from the ms counter */ }

bool futex_wait(unsigned *addr, unsigned expected, unsigned ms)
{
  return syscall(SYS_FUTEX_WAIT, (int)addr, (int)expected, (int)ms) == 1;
}

int futex_wake(unsigned *addr, int n) { return syscall(SYS_FUTEX_WAKE, (int)addr, n, 0); }

unsigned msec(void) { return (unsigned)syscall(SYS_MSEC, 0, 0, 0); }

unsigned rtc_epoch(void) { return (unsigned)syscall(SYS_RTC_EPOCH, 0, 0, 0); }

/* Wall-clock milliseconds since the Unix epoch, the basis for JS Date.now() */
long long now_ms(void)
{
  struct timeval64 tv;
  gettime(&tv);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
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

/* All net send/recv variants use two syscalls with a 6-word request {fd, buf, len, flags, ip|0 */
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

int gettime(struct timeval64 *tv) { return syscall(SYS_GETTIME, (int)tv, 0, 0); }

int settime(const struct timeval64 *tv) { return syscall(SYS_SETTIME, (int)tv, 0, 0); }

int adjtime(const struct timeval64 *delta, struct timeval64 *old)
{
  return syscall(SYS_ADJTIME, (int)delta, (int)old, 0);
}

int net_listen(int port) { return syscall(SYS_NET_LISTEN, port, 0, 0); }

int net_accept(int listen_fd) { return syscall(SYS_NET_ACCEPT, listen_fd, 0, 0); }

int net_bind(int port) { return syscall(SYS_NET_BIND, port, 0, 0); }

int net_recvfrom(int fd, void *buf, int max, unsigned char src_ip[16], int *src_port)
{
  unsigned a[6] = {(unsigned)fd, (unsigned)buf, (unsigned)max, 0, (unsigned)src_ip, (unsigned)src_port};
  return syscall(SYS_NET_RECV, (int)a, 0, 0);
}

int net_sendto(int fd, const unsigned char ip[16], int port, const void *buf, int len)
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

int win_fullscreen(int id, int on) { return syscall(SYS_WIN_FULLSCREEN, id, on, 0); }

int win_cursor(int id, int shape) { return syscall(SYS_WIN_CURSOR, id, shape, 0); }

/* Phase 1 user-space compositor wrappers (root-only) */
int fb_info(struct fbinfo *out) { return syscall(SYS_FB_INFO, (int)out, 0, 0); }

unsigned *fb_map(void)
{
  int v = syscall(SYS_FB_MAP, 0, 0, 0);
  return v == -1 ? (unsigned *)0 : (unsigned *)v;
}

int fb_present_rects(const struct fbrect *r, int n) { return syscall(SYS_FB_PRESENT, (int)r, n, 0); }

int fb_cursor_set(const unsigned *argb, int w, int h, int hot_x, int hot_y, int scale)
{
  return syscall(SYS_FB_CURSOR, (int)argb, w | h << 8 | hot_x << 16 | hot_y << 24, scale);
}

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

int symbolize(unsigned addr, char *out, int cap) { return syscall(SYS_SYMBOLIZE, (int)addr, (int)out, cap); }

int thread_setname(const char *name) { return syscall(SYS_THREAD_NAME, THREAD_NAME_SET, (int)name, 0); }

int thread_getname(char *out, int cap) { return syscall(SYS_THREAD_NAME, THREAD_NAME_GET, (int)out, cap); }

/* Packed (hour<<16)|(min<<8)|sec, UTC */
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

int chmod(const char *path, unsigned mode) { return syscall(SYS_CHMOD, (int)path, (int)mode, 0); }

int chown(const char *path, int uid, int gid) { return syscall(SYS_CHOWN, (int)path, uid, gid); }

unsigned umask(unsigned mask) { return (unsigned)syscall(SYS_UMASK, (int)mask, 0, 0); }

int path_access(const char *path, int mode) { return syscall(SYS_ACCESS, (int)path, mode, 0); }

int statfs(const char *path, struct statfs_info *out) { return syscall(SYS_STATFS, (int)path, (int)out, 0); }

int spawn(const char *path, const char *arg) { return syscall(SYS_SPAWN, (int)path, (int)arg, 0); }

/* SYS_SPAWN_CAP request; see syscall_exec_spawn_cap in the kernel */
struct spawn_req {
  const char *path;
  char *const *argv;
  int argc;
  int *rfd;
  int *wfd;
  int uid;
  int gid;
  const char *cwd;
  unsigned flags;
  int stdio[3];
  const char *env;
  unsigned env_len;
};

int spawn_ex(const struct spawn_opts *o)
{
  struct spawn_req req = {o->path,
                          o->argv,
                          o->argc,
                          o->rfd,
                          o->wfd,
                          o->flags & SPAWN_CREDS ? o->uid : -1,
                          o->flags & SPAWN_CREDS ? o->gid : -1,
                          o->cwd,
                          o->flags & (SPAWN_PTY | SPAWN_STDIO),
                          {o->stdio[0], o->stdio[1], o->stdio[2]},
                          0,
                          0};
  if (o->wfd)
    *o->wfd = -1;
  if (o->env) { /* a block of NAME=VALUE strings ended by an empty string */
    unsigned n = 0;
    while (o->env[n] || o->env[n + 1])
      n++;
    req.env = o->env;
    req.env_len = n ? n + 1 : 0;
    req.flags |= SPAWN_ENV;
  }
  return syscall(SYS_SPAWN_CAP, (int)&req, 0, 0);
}

/* spawn_cap with a full argv; the kernel uses `path` for the image */
int spawn_cap(const char *path, char *const argv[], int argc, int *rfd)
{
  struct spawn_opts o = {.path = path, .argv = argv, .argc = argc, .rfd = rfd};
  return spawn_ex(&o);
}

/* Like spawn_cap, plus a stdin pipe whose write end is stored in *wfd */
int spawn_cap_io(const char *path, char *const argv[], int argc, int *rfd, int *wfd)
{
  struct spawn_opts o = {.path = path, .argv = argv, .argc = argc, .rfd = rfd, .wfd = wfd};
  return spawn_ex(&o);
}

/* environment */

static int env_call(unsigned cmd, const char *name, const char *value, char *buf, unsigned buf_len, unsigned index)
{
  struct env_req q = {cmd,
                      (unsigned)name,
                      name ? (unsigned)strlen(name) : 0,
                      (unsigned)value,
                      value ? (unsigned)strlen(value) : 0,
                      (unsigned)buf,
                      buf_len,
                      index};
  return syscall(SYS_ENV, (int)&q, 0, 0);
}

/* The value stays valid until the next getenv call */
char *getenv(const char *name)
{
  static char *value;
  int len = env_call(ENV_GET, name, 0, 0, 0, 0);
  if (len < 0)
    return 0;
  char *grown = realloc(value, (unsigned)len + 1);
  if (!grown)
    return 0;
  value = grown;
  env_call(ENV_GET, name, 0, value, (unsigned)len + 1, 0);
  return value;
}

int setenv(const char *name, const char *value, int overwrite)
{
  return env_call(ENV_SET, name, value, 0, 0, overwrite != 0);
}

int unsetenv(const char *name) { return env_call(ENV_UNSET, name, 0, 0, 0, 0); }

int clearenv(void) { return env_call(ENV_CLEAR, 0, 0, 0, 0, 0); }

/* Entry `index` ("NAME=VALUE") into buf; its full length, or -1 past the end */
int env_entry(unsigned index, char *buf, unsigned buf_len) { return env_call(ENV_ENTRY, 0, 0, buf, buf_len, index); }

/* Append NAME=VALUE to a block for the env argument of the spawn calls; *block is malloc'd, 0 to start */
int env_block_add(char **block, const char *name, const char *value)
{
  unsigned used = 0;
  if (*block)
    while ((*block)[used])
      used += (unsigned)strlen(*block + used) + 1;
  unsigned add = (unsigned)strlen(name) + 1 + (unsigned)strlen(value) + 1;
  char *grown = realloc(*block, used + add + 1);
  if (!grown)
    return -1;
  strcpy(grown + used, name);
  strcat(grown + used, "=");
  strcat(grown + used, value);
  grown[used + add] = '\0';
  *block = grown;
  return 0;
}

int isatty(int fd) { return syscall(SYS_TTY, fd, TTY_ISATTY, 0) == 1; }

int tty_size(int fd, int *cols, int *rows)
{
  int v = syscall(SYS_TTY, fd, TTY_GETSIZE, 0);
  if (v < 0)
    return -1;
  *cols = (int)((unsigned)v >> 16);
  *rows = v & 0xFFFF;
  return 0;
}

int tty_set_size(int master, int cols, int rows) { return syscall(SYS_TTY, master, TTY_SETSIZE, cols << 16 | rows); }

int tty_set_fg(int fd, int pid) { return syscall(SYS_TTY, fd, TTY_SETFG, pid); }

int tty_fg(int fd) { return syscall(SYS_TTY, fd, TTY_GETFG, 0); }

/* Non-blocking read: >0 bytes, 0 if it would block, -1 at EOF */
int read_nb(int fd, void *buf, int len) { return syscall(SYS_READ_NB, fd, (int)buf, len); }

int chdir(const char *path) { return syscall(SYS_CHDIR, (int)path, 0, 0); }

int kill(int pid, int sig) { return syscall(SYS_KILL, pid, sig, 0); }

/* action: 0 = shut down, 1 = reboot, 2 = suspend (returns on resume), 3 = 1 if suspend is supported */
int power(int action) { return syscall(SYS_POWER, action, 0, 0); }

int getpid(void) { return syscall(SYS_GETPID, 0, 0, 0); }

/* sigprocmask with set==NULL only queries the mask via *old */
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
int install_disk(int index, struct install_disk *out) { return syscall(SYS_INSTALL, INSTALL_LIST, index, (int)out); }
int install_start(const char *dev) { return syscall(SYS_INSTALL, INSTALL_START, (int)dev, 0); }
int install_step(struct install_progress *out) { return syscall(SYS_INSTALL, INSTALL_STEP, (int)out, 0); }

int audio_open(void) { return syscall(SYS_AUDIO_OPEN, 0, 0, 0); }

int audio_write(const void *pcm, int len) { return syscall(SYS_AUDIO_WRITE, (int)pcm, len, 0); }

int audio_close(void) { return syscall(SYS_AUDIO_CLOSE, 0, 0, 0); }

void msleep(unsigned ms) { syscall(SYS_SLEEP, (int)ms, 0, 0); }

/* __thread storage. user.ld lays out the .tdata image and .tbss; each thread gets a block holding both,
 * then a thread pointer that stores its own address (%gs:0, as the i386 ABI wants). The kernel makes %gs
 * a segment based at that pointer, so a variable lives at a fixed negative offset from it. */
extern char __tdata_start[], __tdata_end[], __tls_main[], __tls_memsz[], __tls_align[];

static unsigned tls_data_size(void)
{
  unsigned align = (unsigned)__tls_align;
  return ((unsigned)__tls_memsz + align - 1) & ~(align - 1);
}

/* Fill the block at `block` (16-aligned, tls_data_size() + 16 bytes) and return its thread pointer. */
static void *tls_fill(char *block)
{
  unsigned image = (unsigned)(__tdata_end - __tdata_start);
  void **tp = (void **)(block + tls_data_size());
  memcpy(block, __tdata_start, image);
  memset(block + image, 0, tls_data_size() - image);
  *tp = tp;
  return tp;
}

/* Called by _start for the first thread, before main. */
void tls_init(void)
{
  if ((unsigned)__tls_memsz)
    syscall(SYS_SET_TLS, (int)tls_fill(__tls_main), 0, 0);
}

/* The new thread's TLS block is carved from the top of its stack; with an argument, the entry then sees an
 * ordinary cdecl frame (a null return address, then `arg`). */
static int thread_spawn(void *fn, void *stack_top, int has_arg, void *arg)
{
  void *tp = 0;
  if ((unsigned)__tls_memsz) {
    char *block = (char *)(((unsigned)stack_top - tls_data_size() - 16) & ~15u);
    tp = tls_fill(block);
    stack_top = block;
  }
  if (has_arg) {
    void **sp = (void **)((char *)stack_top - 20); /* esp + 4 is 16-aligned at entry */
    sp[0] = 0;
    sp[1] = arg;
    stack_top = sp;
  }
  return syscall(SYS_THREAD, (int)fn, (int)stack_top, (int)tp);
}

int thread_create(void (*fn)(void), void *stack_top) { return thread_spawn((void *)fn, stack_top, 0, 0); }

int thread_create_arg(void (*fn)(void *), void *arg, void *stack_top)
{
  return thread_spawn((void *)fn, stack_top, 1, arg);
}

/* Stack with an unmapped guard page below, so overflow faults */
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

/* identity / hostname */
/* Per-process credentials */
int getuid(void) { return syscall(SYS_GETUID, 0, 0, 0); }
int setuid(int uid) { return uid < 0 ? -1 : syscall(SYS_SETUID, uid, 0, 0); }
int setgid(int gid) { return gid < 0 ? -1 : syscall(SYS_SETGID, gid, 0, 0); }

/* Copy the ':'-separated field #idx (0-based) of the line [s,end) into out */
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

/* "uid gid name home" of the caller from /proc/user (readable even without /etc/passwd) */
static int proc_user(unsigned *uid, unsigned *gid, char *name, int nmax, char *home, int hmax)
{
  char b[256];
  int n = readfile("/proc/user", b, (int)sizeof b - 1);
  if (n <= 0)
    return 0;
  b[n] = '\0';
  const char *p = b;
  unsigned id[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    for (; *p >= '0' && *p <= '9'; p++)
      id[k] = id[k] * 10 + (unsigned)(*p - '0');
    while (*p == ' ')
      p++;
  }
  int i = 0;
  while (*p && *p != ' ' && *p != '\n' && i < nmax - 1)
    name[i++] = *p++;
  name[i] = '\0';
  while (*p == ' ')
    p++;
  i = 0;
  while (*p && *p != '\n' && i < hmax - 1)
    home[i++] = *p++;
  home[i] = '\0';
  *uid = id[0];
  *gid = id[1];
  return 1;
}

int getgid(void)
{
  unsigned uid, gid;
  char name[2], home[2];
  return proc_user(&uid, &gid, name, sizeof name, home, sizeof home) ? (int)gid : -1;
}

/* Resolve the calling process's login from its uid via /etc/passwd (name:pwhash:uid:gid:home) */
int user_ident(char *name, int nmax, char *home, int hmax)
{
  static char pw[2048];
  unsigned pu, pg;
  char pname[64], phome[256];
  if (proc_user(&pu, &pg, pname, sizeof pname, phome, sizeof phome)) {
    if (name)
      strlcpy(name, pname, nmax);
    if (home)
      strlcpy(home, phome, hmax);
    return 1;
  }
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

/* The system hostname from /etc/hostname (default "owos" when unset) */
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

/* Persist a new system hostname to /etc/hostname */
int sethostname(const char *name) { return writefile("/etc/hostname", name, (int)strlen(name)) < 0 ? -1 : 0; }

/* POSIX uname(2) */
int uname(struct utsname *out)
{
  if (syscall(SYS_UNAME, (int)out, 0, 0) != 0)
    return -1;
  gethostname(out->nodename, (int)sizeof out->nodename);
  return 0;
}

/* IPC: local ports */
/* Named local port: as_server=1 listens (then port_accept), 0 connects */
int port_open(const char *name, int as_server) { return syscall(SYS_PORT_OPEN, (int)name, as_server, 0); }

int port_accept(int listen_fd) { return syscall(SYS_PORT_ACCEPT, listen_fd, 0, 0); }

/* malloc / free */
/* K&R first-fit allocator (§8.7) over SYS_SBRK, locked for threads */

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
static Header *freep; /* roving free-list pointer */

/* Exact-size LIFO bins for small blocks (<= SBIN_MAX units): O(1) free/malloc */
#define SBIN_MAX 1024u
static Header *sbins[SBIN_MAX + 1];

static mutex_t heap_lock;
#define hlock() mutex_lock(&heap_lock)
#define hunlock() mutex_unlock(&heap_lock)

/* Crash recovery only: drop the heap lock a longjmp'd-over frame may have held */
void heap_lock_reset(void) { mutex_reset(&heap_lock); }

#define NALLOC 2048u /* min header-units to request at once */

/* free() body without the lock (so morecore can reuse it under the held lock) */
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
      break;                         /* freed block at the list's edge */
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

/* Grow the heap by at least `nu` header units via sbrk and fold it into the free list */
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
  if (prevp == 0) { /* first call: one empty block */
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for (Header *p = prevp->s.ptr;; prevp = p, p = p->s.ptr) {
    if (p->s.size >= nunits) { /* big enough */
      if (p->s.size == nunits) {
        prevp->s.ptr = p->s.ptr; /* exact fit: unlink */
      } else {
        p->s.size -= nunits; /* split: carve the tail */
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      hunlock();
      return (void *)(p + 1);
    }
    if (p == freep) { /* wrapped the list: grow the heap */
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
  if (size && total / size != nmemb) /* overflow */
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
  /* Old payload = (block units - 1) header-sized cells */
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

/* mem*: shared by every program and by libs like minimp3 and the HTTP/2 HPACK tables */
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

char *strchr(const char *s, int c)
{
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return 0;
  }
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

char *strdup(const char *s)
{
  if (!s)
    return 0;
  int n = strlen(s);
  char *d = malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}

void strcat(char *d, const char *s)
{
  while (*d)
    d++;
  while ((*d++ = *s++))
    ;
}

/* UTF-8 codec */

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

/* printf family */

/* A minimal freestanding printf, shared by snprintf and printf */
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
  } else { /* " -7": space padding, then sign */
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
    int prec = -1;
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9') {
          prec = prec * 10 + (*fmt - '0');
          fmt++;
        }
      }
    }
    switch (*fmt) {
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      while (*s && prec--)
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

/* snprintf sink: write into a buffer */
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

/* printf sink: accumulate into a buffer and write(1) in chunks */
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
    if (c == '\b') { /* backspace */
      if (len > 0) {
        len--;
        write(1, "\b \b", 3);
      }
      continue;
    }
    if (len < max - 1) {
      buf[len++] = c;
      write(1, &c, 1); /* echo */
    }
  }
  buf[len] = '\0';
  return len;
}

/* randomness */
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

/* getrandom: CSPRNG bytes from the kernel entropy pool */
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
  /* No RDRAND, or it stalled mid-buffer: fill the rest from the kernel entropy pool */
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

void opt_print_usage(const char *prog, const char *args, const struct opt *tbl, int n)
{
  int len = opt_usage(0, 0, prog, args, tbl, n);
  char *buf = malloc((unsigned)len + 1);
  if (!buf)
    return;
  opt_usage(buf, len + 1, prog, args, tbl, n);
  puts(buf);
  free(buf);
}

void opt_print_help(const char *prog, const char *args, const char *summary, const struct opt *tbl, int n)
{
  int len = opt_help(0, 0, prog, args, summary, tbl, n);
  char *buf = malloc((unsigned)len + 1);
  if (!buf)
    return;
  opt_help(buf, len + 1, prog, args, summary, tbl, n);
  puts(buf);
  free(buf);
}
