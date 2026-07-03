/* uwin.h — the kernel windowing broker (user-window server).
 *
 * Lets a user process own a desktop window: it creates one, gets a shared
 * client-area pixel buffer mapped into its address space (at WIN_FB_VADDR),
 * draws into it, presents frames, and reads input events. The kernel records the
 * window's frame geometry, focus flag, and event ring; the user-space compositor
 * (/bin/compositor) draws the chrome, blits the buffer, and routes input back in
 * through the brokering backends below.
 *
 * Reached from user space through the SYS_WIN_* syscalls (see syscall.c); the
 * functions here run in the calling process's context, so user pointers are
 * caller-validated and dereferenced directly.
 */
#ifndef OWOS_UWIN_H
#define OWOS_UWIN_H

#include <stdint.h>

/* Fixed user-virtual address where a window's client buffer is mapped. It sits
 * well above the program image (0x280000), stack (~0x3FF000) and the 16 MiB
 * identity map, in its own page tables — so the buffer can be as large as the
 * whole screen without colliding with the low per-process window. */
#define WIN_FB_VADDR  0xC0000000u
#define WIN_MAX_BYTES 0x000E0000u      /* fallback cap if the screen size is 0 */

/* Input event delivered to the owning process. Must match user/lib/owos.h. */
#define WEV_KEY    1                   /* a:  key char (control codes < 0x20)  */
#define WEV_MOUSE  2                   /* a,b: client-relative x,y (left click) */
#define WEV_CLOSE  3                   /* the close box was clicked            */
#define WEV_RESIZE 4                   /* a,b: new client w,h (app opted in)    */
#define WEV_DBLCLICK 5                 /* a,b: client x,y of a double click     */
#define WEV_RCLICK 6                   /* a,b: client x,y of a right click      */
#define WEV_SCROLL 7                   /* a: mouse-wheel delta (+up / -down)    */
#define WEV_DRAG   8                   /* a,b: client x,y, left button held+moving */
#define WEV_MOVE   9                   /* a,b: client x,y, no button held (a<0=left) */

struct win_event {
	uint32_t type;
	int32_t  a, b;
};

/* ---- window brokering for the user-space compositor ----
 * A root compositor enumerates live windows, maps their client buffers to blit
 * them zero-copy, positions them, and routes hit-tested input back in.
 * struct winrec must match user/lib/owos.h. */

/* One live window, as reported by uwin_enum. */
struct winrec {
	int      id;             /* window id (index into the uwin table)          */
	int      x, y, w, h;     /* frame geometry on screen (top-left + full size) */
	uint32_t flags;          /* UWIN_F_* below                                 */
	uint32_t present_seq;    /* bumped on each present -> the compositor's dirty tag */
	int      owner_pid;      /* owning task id                                 */
	int      z;              /* z-order: 0 = bottom, higher = nearer the top   */
	int      buf_w, buf_h;   /* client buffer size in pixels (stride = buf_w)   */
	char     title[64];      /* window title (NUL-terminated)                  */
};

#define UWIN_F_VISIBLE   0x01
#define UWIN_F_FOCUSED   0x02
#define UWIN_F_MINIMIZED 0x04
#define UWIN_F_MAXIMIZED 0x08
#define UWIN_F_RESIZABLE 0x10

/* One routed input event the compositor delivers into a window's event ring
 * (client-relative). `type` is a WEV_*; a,b carry its payload (see the WEV_*
 * comments). Matches user/lib/owos.h. */
struct winevent {
	uint32_t type;
	int32_t  a, b;
};

/* Syscall backends (all run in the caller's process context). */
int  uwin_create(int w, int h, const char *title);   /* -> window id or -1     */
int  uwin_present(int id);                            /* -> 0 / -1              */
int  uwin_poll(int id, struct win_event *out);        /* 1 event / 0 none / -1  */
int  uwin_close(int id);                              /* -> 0 / -1              */
int  uwin_set_title(int id, const char *title);       /* -> 0 / -1              */
int  uwin_set_resizable(int id, int on);              /* app opts into resizing */
int  uwin_set_cursor(int id, int cursor);             /* enum wm_cursor for the
                                                         window's client area     */

/* Free slots in a window's input event ring (1<<30 for a bad/unknown id). Used
 * for clipboard-paste flow control. */
int  uwin_key_space(int id);

/* Called from sched_exit()/sched_kill(): detach every window owned by the
 * exiting task (and, if that task is the compositor, clear the compositor-active
 * flag so kernel_main reverts to driving the recovery console). */
void uwin_task_exiting(uint32_t owner_id);

/* Called when a pin is dropped (uwin_unmap): reclaim the client buffers of
 * windows that were closed while the compositor still held them mapped. */
void uwin_reap(void);

/* Compositor liveness. uwin_compositor_register(pid) marks `pid` as the running
 * compositor on its first SYS_FB_MAP; uwin_compositor_active() is 1 while a
 * compositor owns the display. kernel_main uses it to decide whether to drain
 * the physical keyboard into the recovery console (it must not while the
 * compositor is draining input via SYS_INPUT_POLL). Cleared by
 * uwin_task_exiting when the compositor task exits. */
void uwin_compositor_register(uint32_t pid);
int  uwin_compositor_active(void);

/* ---- compositor brokering backends (SYS_WIN_ENUM/MAP/UNMAP/SET_GEOM/DELIVER/
 * SET_FOCUS). All run in the caller's (compositor) context; the caller
 * root-gates and bounds-checks. */

/* Fill up to `max` winrecs for live windows, sorted by ascending z (bottom
 * first). Returns the count written (may be < the number of live windows if the
 * table has more than `max`). */
int uwin_enum(struct winrec *out, int max);

/* Map window `id`'s client buffer into the caller at vaddr `at` (npages of it),
 * pinning the frames so a concurrent close can't free them under the compositor
 * (see uwin_reap). Fills bw/bh/stride with the buffer geometry. Returns 0, or
 * -1 (bad id / gone / map failed). Idempotent: re-mapping a pinned window just
 * refreshes the mapping. */
int uwin_map(int id, uint32_t at, int *bw, int *bh, int *stride);

/* Drop the compositor's pin on window `id` and unmap it from `dir` at `at`. Once
 * unpinned, a closed window's frames are reclaimed by the next uwin_reap. */
int uwin_unmap(int id, uint32_t *dir, uint32_t at);

/* Compositor sets a window's on-screen geometry (frame top-left + full size). */
int uwin_set_geom(int id, int x, int y, int w, int h);

/* Compositor pushes a routed, client-relative input event into window `id`'s
 * event ring (it did the hit-testing). */
int uwin_deliver(int id, const struct winevent *ev);

/* Compositor records which window has focus (raise it to the top). id <= 0 =
 * none. */
int uwin_set_focus(int id);

#endif /* OWOS_UWIN_H */
