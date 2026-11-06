/* owos.h: the OwOS user-space library (minimal libc over int 0x80) */
#ifndef OWOS_USER_H
#define OWOS_USER_H

#include "ascii.h"
#include "timeval64.h"
#include <stdarg.h>

/* The null pointer constant */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Build version: the Makefile passes -DOWOS_VERSION="<git describe>" */
#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

/* Thin syscall wrappers */
int write(int fd, const void *buf, int len); /* bytes written */
int read(int fd, void *buf, int len);        /* bytes read (0 = EOF) */
int pipe(int fds[2]);                        /* fds[0]=read end, fds[1]=write */
int close(int fd);
void exit(int code) __attribute__((noreturn));
int fork(void);                                            /* 0 in child, child pid in parent */
int execp(const char *name);                               /* replace this image; -1 on error */
int execp1(const char *name, const char *arg);             /* + one argv[1] string */
int waitpid(int pid);                                      /* block for a child; its status */
#define WNOHANG 1                                          /* waitpid flag (see waitpid_nb) */
int waitpid_nb(int pid, int *status);                      /* non-blocking reap: 1 reaped/0 none */
unsigned uptime(void);                                     /* seconds since boot */
unsigned msec(void);                                       /* milliseconds since boot */
unsigned rtc_epoch(void);                                  /* Unix epoch seconds (UTC) */
long long now_ms(void);                                    /* wall-clock ms since the epoch */
int readfile(const char *name, char *buf, int max);        /* bytes, or -1 */
int writefile(const char *name, const void *buf, int len); /* 0, or -1 */
/* Partial read: up to `len` bytes at byte `off` */
int readfile_at(const char *name, unsigned off, void *buf, unsigned len);
/* Streaming write; one stream at a time */
int stream_begin(const char *name);
int stream_write(const void *buf, int len);
int stream_end(void);

/* Process exit status for "interrupted by Ctrl+C" (128 + SIGINT, as on Unix) */
#define EXIT_SIGINT 130

/* net_* return: a cooperative Ctrl+C arrived mid-call; tear down and exit */
#define NET_EINTR (-4)

/* net_connect: `host` is a dotted-quad (name resolution is user/lib/resolve), `proto` is SK_* */
#define SK_TCP 0
#define SK_UDP 2
#define SK_ICMP 3
#define SK_RAWETH 4 /* raw L2: send/recv full Ethernet frames (root-only) */
int net_connect(const char *host, int port, int proto);
int net_send(int h, const void *buf, int len);
int net_recv(int h, void *buf, int max);
/* Non-blocking recv: >0 bytes, 0 would-block, -1 EOF/error, -4 interrupted */
#define MSG_DONTWAIT 1 /* internal recv flag (net_recv_nb); the wrappers set it */
int net_recv_nb(int h, void *buf, int max);
int net_close(int h);
/* ALPN protocol a TLS conn negotiated: ALPN_H2, ALPN_HTTP11, or ALPN_NONE for a plain connection */
#define ALPN_NONE 0
#define ALPN_HTTP11 1
#define ALPN_H2 2
/* Outgoing IP TTL for a UDP socket (the IP_MULTICAST_TTL analog); mDNS/SSDP need 255 */
#define NET_OPT_TTL 1 /* net_set_ttl option id (the only socket option today) */
int net_set_ttl(int h, int ttl);

/* Server sockets */
int net_listen(int port);
int net_accept(int listen_fd);
int net_bind(int port);
int net_recvfrom(int fd, void *buf, int max, unsigned char src_ip[4], int *src_port);
int net_sendto(int fd, const unsigned char ip[4], int port, const void *buf, int len);

/* Raw Ethernet socket (full L2 frames) + IPv4 config, for /bin/dhcpcd */
int net_raw_open(void);
int ifconfig(const unsigned char ip[4], const unsigned char mask[4], const unsigned char gw[4],
             const unsigned char dns[4]);

/* Set the wall clock from a Unix timestamp (UTC seconds) */
int setrtc(unsigned secs);

/* Microsecond wall clock (UTC). settime steps it (root); adjtime slews by delta at <= 500 ppm (root),
 * returns the unapplied remainder in *old; either pointer may be NULL. 0 on success, -1 on error. */
int gettime(struct timeval64 *tv);
int settime(const struct timeval64 *tv);
int adjtime(const struct timeval64 *delta, struct timeval64 *old);

/* Freestanding string helpers, shared so each program doesn't re-roll its own */
int strlen(const char *s);
int strcmp(const char *a, const char *b); /* <0 / 0 / >0 */
void strcpy(char *d, const char *s);      /* caller sizes d */
void strcat(char *d, const char *s);      /* caller sizes d */
/* BSD strlcpy: truncates, always NUL-terminates */
void strlcpy(char *dst, const char *src, int size);
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* UTF-8 codec */
int utf8_seqlen(unsigned char lead);
int utf8_decode(const char *s, const char *end, unsigned *cp);
int utf8_encode(unsigned cp, char *out);

/* mem*: gcc also lowers struct copies and array inits to these at -O0 */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);
void *memmove(void *d, const void *s, __SIZE_TYPE__ n);

/* Conveniences built on the above */
void puts(const char *s);         /* write a NUL-terminated string */
void print_uint(unsigned v);      /* write a decimal number */
int readline(char *buf, int max); /* echoed line input, returns len */

/* Supports %s %c %d %i %u %x %X %p %% with '0' flag and width; no floats or length modifiers */
int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned size, const char *fmt, ...);
int vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap);

/* windowing API */
/* Client buffer is mapped at WIN_FB_ADDR (XRGB8888, pitch = width passed to win_create) */
#define WIN_FB_ADDR 0xC0000000u

#define WEV_KEY                                                                                                        \
  1                    /* a = key char (control codes < 0x20 for editing keys),                                        \
                        * b = KMOD_* modifier flags held when it was pressed    */
#define WEV_MOUSE 2    /* a, b = client-relative x, y of a left click */
#define WEV_CLOSE 3    /* the close box was clicked */
#define WEV_RESIZE 4   /* a, b = new client w, h (only if win_resizable on) */
#define WEV_DBLCLICK 5 /* a, b = client x, y of a double click (after WEV_MOUSE) */
#define WEV_RCLICK 6   /* a, b = client x, y of a right click */
#define WEV_SCROLL 7   /* a = mouse-wheel delta (+ up / - down) */
#define WEV_DRAG                                                                                                       \
  8 /* a, b = client x, y while the left button is held and                                                            \
     * moving (motion after a WEV_MOUSE press; ends silently                                                           \
     * on release, the next WEV_MOUSE starts a fresh drag)  */
#define WEV_MOVE                                                                                                       \
  9 /* a, b = client x, y as the pointer moves with NO button                                                          \
     * held; a = -1 when the pointer leaves the window. Fast                                                           \
     * motion coalesces in the queue (only the latest is kept) */

struct win_event {
  unsigned type;
  int a, b;
};

int win_create(int w, int h, const char *title);          /* -> window id, or -1 */
int win_present(int id);                                  /* flush the buffer to screen */
int win_present_rect(int id, int x, int y, int w, int h); /* flush only the client rect that changed */
int win_poll(int id, struct win_event *ev);               /* 1 = got event, 0 = none */
void win_wait(int id);                                    /* sleep ~a frame for events */
int win_close(int id);
/* 1 if process `pid` owns at least one window, else 0 */
int win_owned(int pid);

static inline unsigned *win_fb(void) { return (unsigned *)WIN_FB_ADDR; }

int win_settitle(int id, const char *title); /* set the window's title bar */
int win_resizable(int id, int on);           /* opt the window in/out of user resizing */

/* Make `id` a transient child of `parent` (stacked above, hidden from taskbar) */
int win_set_parent(int id, int parent, int modal);

/* Buffer of an owned window; 0 if not owned */
unsigned *win_fb_of(int id);

/* Cursor over the client area; mirrors enum wm_cursor */
#define WIN_CURSOR_ARROW 0
#define WIN_CURSOR_HAND 1        /* pointing hand, over clickable items */
#define WIN_CURSOR_TEXT 2        /* I-beam, over editable text */
#define WIN_CURSOR_BUSY 3        /* spinner, a slow operation is running */
#define WIN_CURSOR_RESIZE_NWSE 4 /* ↖↘ diagonal resize (corner) */
#define WIN_CURSOR_RESIZE_NESW 5 /* ↗↙ diagonal resize (corner) */
#define WIN_CURSOR_RESIZE_NS 6   /* ↕ vertical resize (top/bottom edge) */
#define WIN_CURSOR_RESIZE_EW 7   /* ↔ horizontal resize (left/right) */
int win_cursor(int id, int shape);

/* Compositor API (root-only, -1 if not uid 0) */
#define COMPOSITOR_FB_ADDR 0xD0000000u
#define WIN_MAP_BASE_ADDR 0xE0000000u
#define WIN_MAP_SLOT 0x00800000u /* 8 MiB per window slot */
static inline unsigned *win_map_ptr(int id) { return (unsigned *)(WIN_MAP_BASE_ADDR + (unsigned)id * WIN_MAP_SLOT); }

struct fbinfo {
  int w, h, pitch, bpp;
}; /* pitch = pixels per row */
int fb_info(struct fbinfo *out); /* 0/-1 */
unsigned *fb_map(void);          /* the back buffer (COMPOSITOR_FB_ADDR), or 0 */
struct fbrect {
  int x, y, w, h;
};
int fb_present_rects(const struct fbrect *r, int n); /* flush up to 32 rects to VRAM, one drain; 0/-1 */

/* Raw input for the compositor: current pointer state + drained key events */
#define INPUT_KEYS 32
struct inputstate {
  int mx, my, buttons, wheel; /* buttons: bit0 L, bit1 R, bit2 M */
  int nkeys;
  struct {
    int c, mods;
  } keys[INPUT_KEYS];
};
int input_poll(struct inputstate *out); /* 0/-1 */
int input_set_bounds(int w, int h);     /* clamp box after a mode change; 0/-1 */

/* Window brokering */
struct winrec {
  int id, x, y, w, h;
  unsigned flags; /* UWIN_F_* */
  unsigned present_seq;
  int dirty_x, dirty_y, dirty_w, dirty_h; /* client damage since last enum; w < 0 = whole frame */
  int owner_pid, z;
  int buf_w, buf_h; /* client buffer size (stride = buf_w) */
  char title[64];
  int parent; /* transient parent window id, or -1 */
};
#define UWIN_F_VISIBLE 0x01
#define UWIN_F_FOCUSED 0x02
#define UWIN_F_MINIMIZED 0x04
#define UWIN_F_MAXIMIZED 0x08
#define UWIN_F_RESIZABLE 0x10
#define UWIN_F_MODAL 0x20 /* transient child that blocks its parent family */
struct winmap {
  unsigned vaddr;
  int w, h, stride;
};
struct winevent {
  unsigned type;
  int a, b;
}; /* type = WEV_*, client-relative */

int win_enum(struct winrec *out, int max);            /* count / -1 */
int win_map(int id, struct winmap *m);                /* 0/-1 */
int win_unmap(int id);                                /* 0/-1 */
int win_set_geom(int id, int x, int y, int w, int h); /* 0/-1 */
int win_deliver(int id, const struct winevent *ev);   /* 0/-1 */
/* Free slots in a window's key/input event ring (a huge number if the window doesn't queue) */
int win_key_space(int id);

/* Pending crash reports; crash_poll consumes up to `max` (root) */
#define CRASH_TRACE_MAX 12
struct crash_rec {
  char name[24];
  unsigned vector, err, eip, cr2;
  unsigned eax, ebx, ecx, edx, esi, edi, ebp, esp;
  unsigned trace[CRASH_TRACE_MAX];
  int ntrace;
};
int crash_poll(struct crash_rec *out, int max); /* count / -1 */

/* Wall-clock time, packed as (hour<<16)|(min<<8)|sec */
unsigned now_hms(void);

/* Sleep for `ms` milliseconds (yields the CPU to other tasks meanwhile) */
void msleep(unsigned ms);

/* Threads share the address space */
int thread_create(void (*fn)(void), void *stack_top);
void *thread_stack_alloc(int bytes); /* guarded thread stack (unmapped guard page below); 0 on fail */
void thread_stack_free(void *top);
static inline int thread_join(int tid) { return waitpid(tid); }

/* sbrk returns the PREVIOUS break, or (void *)-1 */
void *sbrk(int inc);
void *malloc(unsigned size);
void *calloc(unsigned nmemb, unsigned size);
void *realloc(void *ptr, unsigned size);
void free(void *ptr);

/* Fill `buf` with `len` cryptographically-strong random bytes: RDRAND when the CPU has it */
void rng_bytes(void *buf, int len);

/* Raw access to the kernel entropy pool (interrupt jitter, SHA-256 ratchet) */
int getrandom(void *buf, int len);

/* Directory enumeration: fill *out with the idx-th child of `path` (NULL/""/"/" = root) */
struct dirent_u {
  char name[64];
  int is_dir;
  unsigned size;
}; /* name: owofs FS_NAME_LEN */
int readdir(const char *path, int idx, struct dirent_u *out);

/* Filesystem management (paths join onto the process cwd; the shell passes absolute paths) */
/* `mtime` is owofs's packed wall-clock stamp (0 = unstamped/unknown) */
struct statinfo {
  int is_dir;
  unsigned size;
  unsigned mtime;
};
int mkdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path); /* delete a file */
int rename(const char *old, const char *neu);
int stat(const char *path, struct statinfo *out); /* -1 if path absent */

/* Filesystem usage of the volume backing `path` (any path selects the one owofs volume today) */
struct statfs_info {
  unsigned long long total_bytes;
  unsigned long long free_bytes;
};
int statfs(const char *path, struct statfs_info *out);

/* Launch a program as a detached process, with an optional single argument */
int spawn(const char *path, const char *arg);

/* Spawn a /bin child with stdout piped to *rfd (drain with read_nb until -1) */
int spawn_cap(const char *path, char *const argv[], int argc, int *rfd);
/* Like spawn_cap, but also gives the child a stdin pipe: *wfd is the write end */
int spawn_cap_io(const char *path, char *const argv[], int argc, int *rfd, int *wfd);
/* Like spawn_cap_io, but also sets the child's uid/gid and cwd */
int spawn_cap_io_as(const char *path, char *const argv[], int argc, int uid, int gid, const char *cwd, int *rfd,
                    int *wfd);
int read_nb(int fd, void *buf, int len); /* >0 bytes / 0 would-block / -1 EOF */
int chdir(const char *path);             /* set this process's cwd; 0/-1 */
int kill(int pid, int sig);              /* post a signal to a task; 0/-1 */
int getpid(void);                        /* this process's pid */
int power(int action);                   /* 0 = shut down, 1 = reboot */

/* Identity: numeric uid from the kernel; name/home resolved from /etc/passwd */
int getuid(void);
int user_ident(char *name, int nmax, char *home, int hmax); /* 1 if found */
int gethostname(char *buf, int max);                        /* /etc/hostname (default "owos") */
int sethostname(const char *name);                          /* persist /etc/hostname; 0/-1 */

/* POSIX uname(2): system identity */
struct utsname {
  char sysname[65];  /* "OwOS" */
  char nodename[65]; /* hostname (/etc/hostname) */
  char release[65];  /* OWOS_VERSION */
  char version[65];  /* build/version string */
  char machine[65];  /* "i386" | "x86_64" */
};
int uname(struct utsname *out); /* 0/-1 */

/* Named local IPC ports */
int port_open(const char *name, int as_server);
int port_accept(int listen_fd);

/* Signals; numbers/flags mirror kernel/signal.h, keep in sync */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t) - 1)

/* sa_flags is accepted but unused: an interrupted blocking syscall returns EINTR */

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define sigmask(n) (1u << (n))

struct sigaction {
  unsigned handler; /* a sighandler_t, or SIG_DFL / SIG_IGN */
  unsigned mask;    /* extra signals blocked while the handler runs */
  unsigned flags;   /* SA_* */
};

sighandler_t signal(int sig, sighandler_t handler);                         /* prev disposition / SIG_ERR */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old); /* 0/-1 */
unsigned sigprocmask(int how, const unsigned *set, unsigned *old);          /* new blocked mask */
int raise(int sig);                                                         /* send `sig` to ourselves */

/* setjmp/longjmp (start.S); longjmp makes setjmp return `val` (1 if 0) */
typedef unsigned jmp_buf[6];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

/* Force-release the malloc lock after longjmp crash recovery */
void heap_lock_reset(void);

/* PCM sink: 48 kHz 16-bit signed stereo */
int audio_open(void);
int audio_write(const void *pcm, int len);
int audio_close(void);

/* Fill *out with the idx-th removable volume */
struct volinfo {
  char dev[16];
  char kind[8];
  char label[24];
  char point[40];
  int mounted;
}; /* mounted: 1 -> `point` is its mount path */
int volumes(int idx, struct volinfo *out);

/* Mount a present removable device `dev` at mount point `point` */
int mount(const char *dev, const char *point);

/* Unmount/eject the removable volume at /media (write-through, so always safe) */
int umount(const char *point);

/* Key codes via WEV_KEY emitted by the keyboard driver; must mirror kernel/keyboard.h */
#define KEY_INTR                                                                                                       \
  0x18 /* Ctrl+C: interrupt the running command / cancel line                                                          \
        */
#define KEY_UP 0x01
#define KEY_DOWN 0x02
#define KEY_LEFT 0x03
#define KEY_RIGHT 0x04
#define KEY_HOME 0x05
#define KEY_END 0x06
#define KEY_DELETE 0x07
#define KEY_F5 0x0E    /* refresh/reload (the app decides what that means) */
#define KEY_F12 0x1D   /* F12: the app decides (the browser opens devtools) */
#define KEY_SUPER 0x11 /* Windows key: WM-consumed (Start menu); apps never see it */
#define KEY_PRTSC                                                                                                      \
  0x80 /* Print Screen: a real key, WM-consumed (compositor captures the                                               \
                       fb); 0x80 because every control code < 0x20 is already                                          \
          taken */

/* Special keys (media, system, launcher): driver-emitted, WM-consumed unless the compositor forwards them. */
#define KEY_MEDIA_PLAY 0x81 /* play/pause  */
#define KEY_MEDIA_STOP 0x82
#define KEY_MEDIA_NEXT 0x83
#define KEY_MEDIA_PREV 0x84
#define KEY_VOL_UP 0x85
#define KEY_VOL_DOWN 0x86
#define KEY_MUTE 0x87
#define KEY_BRIGHT_UP 0x88
#define KEY_BRIGHT_DOWN 0x89
#define KEY_LAUNCH_CALC 0x8A
#define KEY_LAUNCH_BROWSER 0x8B /* WWW home    */
#define KEY_LAUNCH_FILES 0x8C   /* My Computer */
#define KEY_LAUNCH_MAIL 0x8D
#define KEY_SEARCH 0x8E
#define KEY_POWER 0x8F
#define KEY_SLEEP 0x90
#define KEY_WWW_BACK 0x91
#define KEY_WWW_FORWARD 0x92
#define KEY_WWW_REFRESH 0x93
#define KEY_MIC_MUTE 0x94

/* Accelerators apps synthesise via menu_shortcut; never driver-emitted */
#define KEY_NEW 0x0F    /* Ctrl+N \ */
#define KEY_OPEN 0x10   /* Ctrl+O > menu accelerators (see menu_shortcut) */
#define KEY_SAVE 0x13   /* Ctrl+S / */
#define KEY_SELALL 0x12 /* Ctrl+A: select all (editor) */
#define KEY_CUT 0x14    /* Ctrl+X: cut the selection (editor) */
/* Shift+navigation: extend a selection toward the new caret position */
#define KEY_SLEFT 0x15
#define KEY_SRIGHT 0x16
#define KEY_SUP 0x17
#define KEY_SDOWN 0x19
#define KEY_SHOME 0x1A
#define KEY_SEND 0x1C
/* Ctrl+C reaches a focused app as KEY_INTR (0x18); the editor uses it as Copy */

/* Modifier flags in win_event.b (WEV_KEY) */
#define KMOD_SHIFT 0x01
#define KMOD_CTRL 0x02
#define KMOD_ALT 0x04
#define KMOD_SUPER 0x08
#define KMOD_ALTGR 0x10

#endif /* OWOS_USER_H */
