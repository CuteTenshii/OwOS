/* owos.h — the OwOS user-space library (minimal libc over int 0x80). */
#ifndef OWOS_USER_H
#define OWOS_USER_H

#include <stdarg.h>

/* The null pointer constant. Guarded so a TU that also pulls in <stddef.h>
 * (its definition is token-identical) doesn't warn on redefinition. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Build version: the Makefile passes -DOWOS_VERSION="<git describe>". This is the
 * fallback for an editor/standalone compile with no -D. */
#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

/* Thin syscall wrappers. */
int      write(int fd, const void *buf, int len);   /* bytes written          */
int      read(int fd, void *buf, int len);          /* bytes read (0 = EOF)   */
int      pipe(int fds[2]);                    /* fds[0]=read end, fds[1]=write  */
int      close(int fd);
void     exit(int code) __attribute__((noreturn));
int      fork(void);                          /* 0 in child, child pid in parent */
int      execp(const char *name);             /* replace this image; -1 on error */
int      execp1(const char *name, const char *arg); /* + one argv[1] string      */
int      waitpid(int pid);                     /* block for a child; its status  */
unsigned uptime(void);                        /* seconds since boot            */
unsigned msec(void);                          /* milliseconds since boot       */
unsigned rtc_epoch(void);                     /* Unix epoch seconds (UTC)      */
long long now_ms(void);                       /* wall-clock ms since the epoch */
int      readfile(const char *name, char *buf, int max);  /* bytes, or -1     */
int      writefile(const char *name, const void *buf, int len); /* 0, or -1    */
/* Partial read: up to `len` bytes at byte `off`. Returns the count (0 = EOF,
 * may be short at end of file) or -1. Lets a program stream a file bigger than
 * its address window. */
int      readfile_at(const char *name, unsigned off, void *buf, unsigned len);
/* Streaming write, for content too large to hold in one buffer (e.g. a browser
 * download). stream_begin opens `name` (overwriting any existing file);
 * stream_write appends a chunk; stream_end commits and returns the total bytes
 * written, or -1. Only one stream may be open at a time. begin/write return
 * 0/-1. A failed write auto-closes the stream, so stream_end then returns -1
 * (nothing is committed and the reserved space is reused). */
int      stream_begin(const char *name);
int      stream_write(const void *buf, int len);
int      stream_end(void);

/* Process exit status for "interrupted by Ctrl+C" (128 + SIGINT, as on Unix). A
 * program returns this from main when a cooperative interrupt aborted it. */
#define EXIT_SIGINT 130

/* net_* error/status codes. Negative returns from net_connect/net_recv/net_send;
 * NET_EINTR means a cooperative Ctrl+C arrived mid-call (the program should tear
 * down and exit). Check it with `r == NET_EINTR`. */
#define NET_EINTR (-4)

/* Networking: a small Berkeley-ish socket API. net_connect resolves `host` and
 * opens a transport selected by `proto` (SK_*): TCP, TLS (handshake, SNI=host),
 * a UDP datagram socket to host:port, or a raw ICMP socket to host (the program
 * builds/parses the ICMP message itself). Returns a handle >= 0, or a negative
 * error (-1 no NIC/slot, -2 resolve failed, -3 connect failed, -4 interrupted).
 * net_send writes (a datagram, for UDP/ICMP). net_recv blocks: TCP/TLS -> >0
 * bytes / 0 EOF; UDP/ICMP -> >0 datagram bytes / 0 timeout; both -1 error, -4
 * interrupted. ping/dig/resolution are code over these sockets. */
#define SK_TCP    0
#define SK_TLS    1
#define SK_UDP    2
#define SK_ICMP   3
#define SK_TLS_H2 4   /* TLS that offers ALPN "h2" (check the result with net_alpn) */
int      net_connect(const char *host, int port, int proto);
int      net_send(int h, const void *buf, int len);
int      net_recv(int h, void *buf, int max);
/* Non-blocking recv: >0 bytes, 0 would-block (no data yet, still open), -1
 * EOF/error, -4 interrupted. Lets an interactive client poll the socket
 * alongside stdin without the blocking recv's idle timeout closing the link. */
int      net_recv_nb(int h, void *buf, int max);
int      net_close(int h);
/* For a TLS socket, the ALPN protocol the server negotiated (we always offer
 * "h2","http/1.1"): ALPN_H2, ALPN_HTTP11 (or no ALPN), or ALPN_NONE for a
 * non-TLS socket. Lets a client pick HTTP/2 vs HTTP/1.1. */
#define ALPN_NONE   0
#define ALPN_HTTP11 1
#define ALPN_H2     2
int      net_alpn(int h);

/* Freestanding string helpers — shared so each program doesn't re-roll its own.
 * Standard libc names, matching the kernel's kstring.h convention. */
int  strlen(const char *s);
int  strcmp(const char *a, const char *b);    /* <0 / 0 / >0                    */
void strcpy(char *d, const char *s);          /* caller sizes d                 */
void strcat(char *d, const char *s);          /* caller sizes d                 */
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Case-insensitive ASCII helpers — shared so each program (browser, curl, the
 * URL parser, …) doesn't re-roll its own. `lower` folds one byte; the _ci
 * variants compare/scan with that folding. Inline because `lower` is called
 * per-character in hot lexers. Non-ASCII bytes pass through unchanged. */
static inline char lower(char c)
{ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int starts_ci(const char *s, const char *pre)
{ while (*pre) { if (lower(*s) != lower(*pre)) return 0; s++; pre++; } return 1; }
static inline int streq_ci(const char *a, const char *b)
{ while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) == lower(*b); }
static inline int contains_ci(const char *hay, const char *needle)
{ for (; *hay; hay++) if (starts_ci(hay, needle)) return 1; return 0; }

/* UTF-8 codec — shared so the whole system handles multibyte / non-Latin text
 * the same way. utf8_seqlen: bytes a lead byte announces (1 for bad/continuation).
 * utf8_decode: read one codepoint from [s,end) into *cp, returning bytes consumed
 * (U+FFFD + 1 byte on a malformed/truncated sequence). utf8_encode: write cp as
 * 1..4 bytes into `out` (must hold >=4), returning the count. */
int utf8_seqlen(unsigned char lead);
int utf8_decode(const char *s, const char *end, unsigned *cp);
int utf8_encode(unsigned cp, char *out);

/* mem* — the freestanding libc primitives gcc also lowers struct copies and
 * array inits to at -O0. Provided once in the shared lib so every program (and
 * any lib needing them, e.g. minimp3, the HTTP/2 HPACK tables) shares one copy.
 * __SIZE_TYPE__ is gcc's builtin for size_t — no header needed. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
void *memmove(void *d, const void *s, __SIZE_TYPE__ n);

/* Conveniences built on the above. */
void puts(const char *s);                     /* write a NUL-terminated string */
void print_uint(unsigned v);                  /* write a decimal number        */
int  readline(char *buf, int max);            /* echoed line input, returns len*/

/* Formatted output. Conversions: %s (NULL -> "(null)"), %c, %d/%i, %u, %x, %X,
 * %p, %%; with an optional '0' flag and a decimal field width (e.g. %08x, %4u).
 * No floats, no length modifiers — everything is 32-bit. printf writes to fd 1;
 * snprintf always NUL-terminates when size > 0 and returns the length that would
 * have been written (C99), so truncation is detectable. */
int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned size, const char *fmt, ...);
int vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap);

/* ----------------------------------------------------------- windowing API */
/*
 * A program creates one window, gets a shared client pixel buffer mapped at
 * WIN_FB_ADDR (XRGB8888, row-major, pitch = the width passed to win_create),
 * draws into it, calls win_present() to show it, and reads input events.
 * win_wait() sleeps roughly one frame so the event loop doesn't busy-spin.
 */
#define WIN_FB_ADDR  0xC0000000u

#define WEV_KEY    1    /* a = key char (control codes < 0x20 for editing keys),
                         * b = KMOD_* modifier flags held when it was pressed    */
#define WEV_MOUSE  2    /* a, b = client-relative x, y of a left click          */
#define WEV_CLOSE  3    /* the close box was clicked                            */
#define WEV_RESIZE 4    /* a, b = new client w, h (only if win_resizable on)    */
#define WEV_DBLCLICK 5  /* a, b = client x, y of a double click (after WEV_MOUSE)*/
#define WEV_RCLICK   6  /* a, b = client x, y of a right click                   */
#define WEV_SCROLL   7  /* a = mouse-wheel delta (+ up / - down)                 */
#define WEV_DRAG     8  /* a, b = client x, y while the left button is held and
                         * moving (motion after a WEV_MOUSE press; ends silently
                         * on release — the next WEV_MOUSE starts a fresh drag)  */
#define WEV_MOVE     9  /* a, b = client x, y as the pointer moves with NO button
                         * held; a = -1 when the pointer leaves the window. Fast
                         * motion coalesces in the queue (only the latest is kept) */

struct win_event { unsigned type; int a, b; };

int  win_create(int w, int h, const char *title);  /* -> window id, or -1      */
int  win_present(int id);                           /* flush the buffer to screen */
int  win_poll(int id, struct win_event *ev);        /* 1 = got event, 0 = none  */
void win_wait(int id);                              /* sleep ~a frame for events */
int  win_close(int id);

static inline unsigned *win_fb(void) { return (unsigned *)WIN_FB_ADDR; }

int win_settitle(int id, const char *title);   /* set the window's title bar */
int win_resizable(int id, int on);   /* opt the window in/out of user resizing */

/* Cursor shape shown while the pointer is over this window's client area (the
 * WM still overrides it with the resize arrow over a grip / hand over the close
 * box). Mirrors enum wm_cursor. Set WIN_CURSOR_BUSY around a slow operation
 * (loading, decoding) and WIN_CURSOR_ARROW again when done. */
#define WIN_CURSOR_ARROW  0
#define WIN_CURSOR_HAND   1    /* pointing hand — over clickable items */
#define WIN_CURSOR_TEXT   2    /* I-beam — over editable text          */
#define WIN_CURSOR_BUSY   3    /* spinner — a slow operation is running */
#define WIN_CURSOR_RESIZE_NWSE 4 /* ↖↘ diagonal resize (corner)              */
#define WIN_CURSOR_RESIZE_NESW 5 /* ↗↙ diagonal resize (corner)        */
#define WIN_CURSOR_RESIZE_NS   6 /* ↕ vertical resize (top/bottom edge) */
#define WIN_CURSOR_RESIZE_EW   7 /* ↔ horizontal resize (left/right)    */
int win_cursor(int id, int shape);

/* Wall-clock time, packed as (hour<<16)|(min<<8)|sec. */
unsigned now_hms(void);

/* Sleep for `ms` milliseconds (yields the CPU to other tasks meanwhile). */
void msleep(unsigned ms);

/* Threads. thread_create spawns `fn` on a caller-provided stack, sharing this
 * process's address space; it returns a tid. The thread function must not
 * return (loop, then call exit()); join it with thread_join (== waitpid) before
 * the process exits, so the shared address space outlives every thread. */
int thread_create(void (*fn)(void), void *stack_top);
static inline int thread_join(int tid) { return waitpid(tid); }

/* User-space heap. sbrk() moves the process break by `inc` bytes (negative to
 * shrink) and returns the PREVIOUS break, or (void *)-1 on failure. malloc/
 * calloc/realloc/free are a K&R-style allocator over it, and are safe to call
 * concurrently from the threads of one process (a lock serialises the free list;
 * the heap itself is shared, since threads share the address space). */
void *sbrk(int inc);
void *malloc(unsigned size);
void *calloc(unsigned nmemb, unsigned size);
void *realloc(void *ptr, unsigned size);
void  free(void *ptr);

/* Fill `buf` with `len` cryptographically-strong random bytes (RDRAND, the CPU
 * hardware RNG; a clock/TSC mix only on CPUs lacking it). Shared by crypto. */
void  rng_bytes(void *buf, int len);

/* Directory enumeration: fill *out with the idx-th child of `path`
 * (NULL/""/"/" = root). Returns 1 if it exists, 0 past the end, -1 on error. */
struct dirent_u { char name[64]; int is_dir; unsigned size; };  /* name: owofs FS_NAME_LEN */
int readdir(const char *path, int idx, struct dirent_u *out);

/* Filesystem management (paths join onto the process cwd; the shell passes
 * absolute paths). All return 0 on success, -1 on failure. */
struct statinfo { int is_dir; unsigned size; };
int mkdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);                       /* delete a file          */
int rename(const char *old, const char *neu);
int stat(const char *path, struct statinfo *out);   /* -1 if path absent      */

/* Launch a program (resolved /bin path or absolute) as a detached process,
 * with an optional single argument. Returns the child pid, or -1. */
int spawn(const char *path, const char *arg);

/* Spawn a /bin child with its stdout captured into a pipe: returns the pid and
 * stores the read-end fd in *rfd. Drain *rfd with read_nb until EOF (-1) to
 * render the child's output inline -- this is how the terminal runs CLI tools
 * (neofetch, curl, ...) without a kernel-shell bridge. The child inherits the
 * caller's cwd. */
int spawn_cap(const char *path, char *const argv[], int argc, int *rfd);
/* Like spawn_cap, but also gives the child a stdin pipe: *wfd is the write end
 * (write keystrokes into it; the child reads fd 0). For interactive children
 * (ssh) the terminal forwards its own keystrokes through *wfd. */
int spawn_cap_io(const char *path, char *const argv[], int argc, int *rfd, int *wfd);
int read_nb(int fd, void *buf, int len);  /* >0 bytes / 0 would-block / -1 EOF */
int chdir(const char *path);              /* set this process's cwd; 0/-1      */
int kill(int pid);                        /* terminate a child of this process */
int power(int action);                    /* 0 = shut down, 1 = reboot         */

/* Identity: numeric ids from the kernel; name/home resolved from /etc/passwd. */
int getuid(void);
int getgid(void);
int user_ident(char *name, int nmax, char *home, int hmax); /* 1 if found       */
int gethostname(char *buf, int max);      /* /etc/hostname (default "owos")     */
int sethostname(const char *name);        /* persist /etc/hostname; 0/-1        */

/* Named local IPC ports (the unix-socket analog). Server: port_open(name,1) ->
 * listen fd, then port_accept(listen) -> a connection fd per client. Client:
 * port_open(name,0) -> a connection fd. Connections are bidirectional: read(),
 * write() and close() work on the fd. */
int port_open(const char *name, int as_server);
int port_accept(int listen_fd);

int intr_take(void);   /* 1 (and clear) if Ctrl+C arrived; for CPU-bound loops */

/* Audio PCM sink (streaming): open a stream, push 48 kHz / 16-bit / signed /
 * stereo frames (4 bytes each), then close. audio_write blocks (back-pressure)
 * until the device has room, so it self-paces to real time without buffering the
 * whole file; it returns the bytes accepted, -1 on error, or -2 if Ctrl+C was
 * pressed. Only one stream exists system-wide. */
int audio_open(void);
int audio_write(const void *pcm, int len);
int audio_close(void);

/* Mounted removable volumes (USB sticks, etc.). Fill *out with the idx-th
 * volume: its label, mount path (e.g. "/media"), backing kind ("USB"/"SATA"/...),
 * and removable flag. Returns 1 if it exists, 0 past the end, -1 on error. */
struct volinfo { char dev[16]; char kind[8]; char label[24]; char point[40];
                 int mounted; };  /* mounted: 1 -> `point` is its mount path     */
int volumes(int idx, struct volinfo *out);

/* Mount a present removable device `dev` ("usb0"/"cd0"/...) at mount point
 * `point` (must be "/media/<name>"). fs.c probes the device's filesystem.
 * Returns 0, or -1 (no device, no recognised fs, point taken, table full). */
int mount(const char *dev, const char *point);

/* Unmount/eject the removable volume at /media (write-through, so always safe). The
 * argument is reserved for a future multi-volume API; pass the mount path or
 * NULL. Returns 0 on success, -1 if nothing was mounted. */
int umount(const char *point);


/* Editing key codes delivered via WEV_KEY (mirror kernel/keyboard.h). */
#define KEY_INTR   0x18   /* Ctrl+C: interrupt the running command / cancel line */
#define KEY_UP     0x01
#define KEY_DOWN   0x02
#define KEY_LEFT   0x03
#define KEY_RIGHT  0x04
#define KEY_HOME   0x05
#define KEY_END    0x06
#define KEY_DELETE 0x07
#define KEY_F5     0x0E   /* refresh/reload (the app decides what that means)  */
#define KEY_F12    0x1D   /* F12: the app decides (the browser opens devtools) */
#define KEY_SUPER  0x11   /* Windows key: WM-consumed (Start menu), never reaches apps;
                             defined only to keep 0x11 reserved and this table in sync */
#define KEY_NEW    0x0F   /* Ctrl+N  \                                         */
#define KEY_OPEN   0x10   /* Ctrl+O   > menu accelerators (see menu_shortcut)  */
#define KEY_SAVE   0x13   /* Ctrl+S  /                                         */
#define KEY_SELALL 0x12   /* Ctrl+A: select all (editor)                       */
#define KEY_CUT    0x14   /* Ctrl+X: cut the selection (editor)                */
/* Shift+navigation: extend a selection toward the new caret position. */
#define KEY_SLEFT  0x15
#define KEY_SRIGHT 0x16
#define KEY_SUP    0x17
#define KEY_SDOWN  0x19
#define KEY_SHOME  0x1A
#define KEY_SEND   0x1C
/* Ctrl+C reaches a focused app as KEY_INTR (0x18); the editor uses it as Copy. */

/* Modifier flags reported with every key event in win_event.b (WEV_KEY). An app
 * reads these to act on chords itself — e.g. the browser treats Alt+Left as
 * "back" — instead of the keyboard minting a dedicated code per chord. Keep in
 * sync with the KMOD_* in drivers/input/keyboard.h. */
#define KMOD_SHIFT 0x01
#define KMOD_CTRL  0x02
#define KMOD_ALT   0x04
#define KMOD_SUPER 0x08
#define KMOD_ALTGR 0x10

#endif /* OWOS_USER_H */
