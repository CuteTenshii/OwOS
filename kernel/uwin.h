/* uwin.h: kernel windowing broker. A user process owns a window with a shared
 * client buffer and event ring; the compositor owns chrome, focus and z-order.
 * Backends run in the calling process's context (SYS_WIN_*). */
#ifndef OWOS_UWIN_H
#define OWOS_UWIN_H

#include <stdbool.h>
#include <stdint.h>

/* Base user address of a window's client buffer. Each live window of an owner
 * takes a slot at WIN_FB_VADDR + slot*WIN_MAP_SLOT_BYTES; the first is slot 0. */
#define WIN_FB_VADDR 0xC0000000u
#define WIN_MAX_BYTES 0x000E0000u /* fallback cap if the screen size is 0 */

/* Input event delivered to the owning process. Must match user/lib/owos.h. */
#define WEV_KEY 1      /* a:  key char (control codes < 0x20)  */
#define WEV_MOUSE 2    /* a,b: client-relative x,y (left click) */
#define WEV_CLOSE 3    /* the close box was clicked            */
#define WEV_RESIZE 4   /* a,b: new client w,h (app opted in)    */
#define WEV_DBLCLICK 5 /* a,b: client x,y of a double click     */
#define WEV_RCLICK 6   /* a,b: client x,y of a right click      */
#define WEV_SCROLL 7   /* a: mouse-wheel delta (+up / -down)    */
#define WEV_DRAG 8     /* a,b: client x,y, left button held+moving */
#define WEV_MOVE 9     /* a,b: client x,y, no button held (a<0=left) */

struct win_event {
  uint32_t type;
  int32_t a, b;
};

/* ---- window brokering for the compositor; struct winrec must match user/lib/owos.h. */

/* One live window, as reported by uwin_enum. */
struct winrec {
  int id;               /* window id (index into the uwin table)          */
  int x, y, w, h;       /* frame geometry on screen (top-left + full size) */
  uint32_t flags;       /* UWIN_F_* below                                 */
  uint32_t present_seq; /* bumped on each present -> the compositor's dirty tag */
  int dirty_x, dirty_y; /* client-relative damage since the previous enum; */
  int dirty_w, dirty_h; /* dirty_w < 0: whole frame, w == 0: none          */
  int owner_pid;        /* owning task id                                 */
  int z;                /* reserved/unused: the compositor tracks z itself */
  int buf_w, buf_h;     /* client buffer size in pixels (stride = buf_w)   */
  char title[64];       /* window title (NUL-terminated)                  */
  int parent;           /* transient parent window id, or -1 (see uwin_set_parent).
                                        A dialog stacks above its parent; UWIN_F_MODAL
                           blocks input to the rest of the parent's window family. */
};

#define UWIN_F_VISIBLE 0x01
#define UWIN_F_FOCUSED 0x02
#define UWIN_F_MINIMIZED 0x04
#define UWIN_F_MAXIMIZED 0x08
#define UWIN_F_RESIZABLE 0x10
#define UWIN_F_MODAL                                                                                                   \
  0x20 /* a transient child that blocks its parent family                                                              \
        */

/* One routed input event the compositor delivers into a window's event ring
 * (client-relative). `type` is a WEV_*; a,b carry its payload (see the WEV_*
 * comments). Matches user/lib/owos.h. */
struct winevent {
  uint32_t type;
  int32_t a, b;
};

/* Syscall backends (all run in the caller's process context). */
int uwin_create(int w, int h, const char *title);     /* -> window id or -1     */
int uwin_present(int id, int x, int y, int w, int h); /* damage rect; w/h <= 0 = all */
int uwin_poll(int id, struct win_event *out);         /* 1 event / 0 none / -1  */
int uwin_close(int id);                               /* -> 0 / -1              */
int uwin_set_title(int id, const char *title);        /* -> 0 / -1              */
int uwin_set_resizable(int id, int on);               /* app opts into resizing */
int uwin_set_cursor(int id, int cursor);              /* enum wm_cursor for the
                                                                      window's client area */

/* Link `id` to a transient `parent` (both caller-owned); modal blocks input to
 * the parent's family. parent < 0 clears. Rejects self-links and cycles. 0 / -1. */
int uwin_set_parent(int id, int parent, int modal);

/* User address of window `id`'s buffer in the caller, or 0 if not owned. */
uint32_t uwin_fb_vaddr(int id);

/* Free slots in a window's input event ring (1<<30 for a bad/unknown id). Used
 * for clipboard-paste flow control. */
int uwin_key_space(int id);

/* Called from sched_exit(): detach every window owned by the exiting task (and,
 * if that task is the compositor, clear the compositor-active flag so
 * kernel_main reverts to driving the recovery console). */
void uwin_task_exiting(uint32_t owner_id);

/* 1 if task `pid` owns at least one live window, else 0 (SYS_WIN_OWNS backend:
 * lets the terminal detach windowed apps without a hardcoded program list). */
bool uwin_pid_has_window(uint32_t pid);

/* Called when a pin is dropped (uwin_unmap): reclaim the client buffers of
 * windows that were closed while the compositor still held them mapped. */
void uwin_reap(void);

/* uwin_compositor_register(pid) marks the compositor on its first SYS_FB_MAP;
 * uwin_compositor_active() is 1 while it runs, so kernel_main leaves keyboard
 * input to it. Cleared by uwin_task_exiting. */
void uwin_compositor_register(uint32_t pid);
bool uwin_compositor_active(void);

/* ---- compositor brokering backends (SYS_WIN_ENUM/MAP/UNMAP/SET_GEOM/DELIVER).
 * All run in the caller's (compositor) context; the caller root-gates and
 * bounds-checks. */

/* Fill up to `max` winrecs for live windows in unspecified (slot) order, the
 * compositor owns z-ordering. Returns the count written (may be < the number of
 * live windows if the table has more than `max`). */
int uwin_enum(struct winrec *out, int max);

/* Map window `id`'s buffer into the caller at `at` (npages), pinning the frames
 * against a concurrent close (see uwin_reap). Fills bw/bh/stride. Idempotent.
 * Returns 0 / -1. */
int uwin_map(int id, uint32_t at, int *bw, int *bh, int *stride);

/* Drop the compositor's pin on window `id` and unmap it from `dir` at `at`.
 * Once unpinned, a closed window's frames are reclaimed by the next uwin_reap.
 */
int uwin_unmap(int id, uint32_t *dir, uint32_t at);

/* Compositor sets a window's on-screen geometry (frame top-left + full size).
 */
int uwin_set_geom(int id, int x, int y, int w, int h);

/* Compositor pushes a routed, client-relative input event into window `id`'s
 * event ring (it did the hit-testing). */
int uwin_deliver(int id, const struct winevent *ev);

#endif /* OWOS_UWIN_H */
