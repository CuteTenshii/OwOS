/* owos.h — the OwOS user-space library (minimal libc over int 0x80). */
#ifndef OWOS_USER_H
#define OWOS_USER_H

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

/* Networking: a tiny socket-ish API. net_connect resolves `host`, opens TCP,
 * and (if tls) runs the TLS handshake; it returns a handle >= 0, or a negative
 * error (-1 no NIC/slot, -2 resolve failed, -3 connect failed). net_recv
 * blocks and returns >0 bytes, 0 at EOF, -1 on error. */
int      net_connect(const char *host, int port, int tls);
int      net_send(int h, const void *buf, int len);
int      net_recv(int h, void *buf, int max);
int      net_close(int h);

/* Conveniences built on the above. */
void puts(const char *s);                     /* write a NUL-terminated string */
void print_uint(unsigned v);                  /* write a decimal number        */
int  readline(char *buf, int max);            /* echoed line input, returns len*/

/* ----------------------------------------------------------- windowing API */
/*
 * A program creates one window, gets a shared client pixel buffer mapped at
 * WIN_FB_ADDR (XRGB8888, row-major, pitch = the width passed to win_create),
 * draws into it, calls win_present() to show it, and reads input events.
 * win_wait() sleeps roughly one frame so the event loop doesn't busy-spin.
 */
#define WIN_FB_ADDR  0xC0000000u

#define WEV_KEY    1    /* a = key char (control codes < 0x20 for editing keys) */
#define WEV_MOUSE  2    /* a, b = client-relative x, y of a left click          */
#define WEV_CLOSE  3    /* the close box was clicked                            */
#define WEV_RESIZE 4    /* a, b = new client w, h (only if win_resizable on)    */
#define WEV_DBLCLICK 5  /* a, b = client x, y of a double click (after WEV_MOUSE)*/
#define WEV_RCLICK   6  /* a, b = client x, y of a right click                   */
#define WEV_SCROLL   7  /* a = mouse-wheel delta (+ up / - down)                 */

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

/* Directory enumeration: fill *out with the idx-th child of `path`
 * (NULL/""/"/" = root). Returns 1 if it exists, 0 past the end, -1 on error. */
struct dirent_u { char name[20]; int is_dir; unsigned size; };
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

/* Run a kernel-shell builtin (journal, neofetch, lspci, date, ...) and capture
 * its text output into out[max]. Returns bytes written, or -1 if not a builtin.
 * Lets the ring-3 terminal reuse every kernel command. */
int shell_cmd(const char *cmd, char *out, int max);

/* Mounted removable volumes (USB sticks, etc.). Fill *out with the idx-th
 * volume: its label, mount path (e.g. "/media"), backing kind ("USB"/"SATA"/...),
 * and removable flag. Returns 1 if it exists, 0 past the end, -1 on error. */
struct volinfo { char label[24]; char path[24]; char kind[8]; int removable; };
int volumes(int idx, struct volinfo *out);

/* Unmount/eject the removable volume at /media (write-through, so always safe). The
 * argument is reserved for a future multi-volume API; pass the mount path or
 * NULL. Returns 0 on success, -1 if nothing was mounted. */
int eject(const char *path);

/* Editing key codes delivered via WEV_KEY (mirror kernel/keyboard.h). */
#define KEY_UP     0x01
#define KEY_DOWN   0x02
#define KEY_LEFT   0x03
#define KEY_RIGHT  0x04
#define KEY_HOME   0x05
#define KEY_END    0x06
#define KEY_DELETE 0x07
#define KEY_F5     0x0E   /* refresh/reload (the app decides what that means)  */
#define KEY_NEW    0x0F   /* Ctrl+N  \                                         */
#define KEY_OPEN   0x10   /* Ctrl+O   > menu accelerators (see menu_shortcut)  */
#define KEY_SAVE   0x13   /* Ctrl+S  /                                         */

#endif /* OWOS_USER_H */
