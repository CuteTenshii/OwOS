/* uwin.h: kernel windowing broker; user processes own windows, the compositor owns chrome/focus/z-order. */
#ifndef OWOS_UWIN_H
#define OWOS_UWIN_H

#include <stdbool.h>
#include <stdint.h>

/* Base user address of window client buffers (slot*WIN_MAP_SLOT_BYTES apart). */
#define WIN_FB_VADDR 0xC0000000u
#define WIN_MAX_BYTES 0x000E0000u

/* Input event types delivered to the owner; must match user/lib/owos.h. */
#define WEV_KEY 1   /* a: key char */
#define WEV_MOUSE 2 /* a,b: client x,y */
#define WEV_CLOSE 3
#define WEV_RESIZE 4   /* a,b: new client w,h */
#define WEV_DBLCLICK 5 /* a,b: client x,y */
#define WEV_RCLICK 6   /* a,b: client x,y */
#define WEV_SCROLL 7   /* a: wheel delta (+up) */
#define WEV_DRAG 8     /* a,b: client x,y, button held */
#define WEV_MOVE 9     /* a,b: client x,y, no button */

struct win_event {
  uint32_t type;
  int32_t a, b;
};

/* Window records for the compositor; must match user/lib/owos.h. */

/* One live window, as reported by uwin_enum. */
struct winrec {
  int id;
  int x, y, w, h;       /* frame geometry */
  uint32_t flags;       /* UWIN_F_* */
  uint32_t present_seq; /* bumped per present (dirty tag) */
  int dirty_x, dirty_y; /* client damage since last enum; */
  int dirty_w, dirty_h; /* dirty_w < 0: whole frame, w == 0: none */
  int owner_pid;
  int z;            /* unused */
  int buf_w, buf_h; /* client buffer size (stride = buf_w) */
  char title[64];
  int parent; /* transient parent id, or -1 */
  int cursor; /* client-area cursor shape, WIN_CURSOR_* */
};

/* Highest client cursor shape; mirrors WIN_CURSOR_* in user/lib/owos.h */
#define UWIN_CURSOR_LAST 14

#define UWIN_F_VISIBLE 0x01
#define UWIN_F_FOCUSED 0x02
#define UWIN_F_MINIMIZED 0x04
#define UWIN_F_MAXIMIZED 0x08
#define UWIN_F_RESIZABLE 0x10
#define UWIN_F_FULLSCREEN 0x40 /* client asked to cover the screen with no chrome */
#define UWIN_F_MODAL                                                                                                   \
  0x20 /* a transient child that blocks its parent family                                                              \
        */

/* A routed client-relative input event; matches user/lib/owos.h. */
struct winevent {
  uint32_t type;
  int32_t a, b;
};

/* Syscall backends (run in the caller's context). */
int uwin_create(int w, int h, const char *title);     /* -> id or -1 */
int uwin_present(int id, int x, int y, int w, int h); /* damage rect; w/h <= 0 = all */
int uwin_poll(int id, struct win_event *out);         /* 1 event / 0 none / -1 */
int uwin_close(int id);
int uwin_set_title(int id, const char *title);
int uwin_set_resizable(int id, int on);
int uwin_set_fullscreen(int id, int on);
int uwin_set_cursor(int id, int cursor); /* WIN_CURSOR_*, 0..UWIN_CURSOR_LAST */

/* Link `id` to a transient `parent` (modal blocks the family); parent < 0 clears. 0 / -1. */
int uwin_set_parent(int id, int parent, int modal);

/* User address of window `id`'s buffer in the caller, or 0 if not owned. */
uint32_t uwin_fb_vaddr(int id);

/* Free slots in a window's event ring (1<<30 for a bad id); for paste flow control. */
int uwin_key_space(int id);

/* Detach windows of the exiting task; clears the compositor-active flag if it was the compositor. */
void uwin_task_exiting(uint32_t owner_id);

/* 1 if task `pid` owns a live window, else 0. */
bool uwin_pid_has_window(uint32_t pid);

/* Reclaim buffers of windows closed while the compositor still had them mapped. */
void uwin_reap(void);

/* uwin_compositor_register() marks the compositor on first SYS_FB_MAP; _active() is 1 while it runs. */
void uwin_compositor_register(uint32_t pid);
bool uwin_compositor_active(void);

/* Compositor brokering backends (SYS_WIN_ENUM/MAP/UNMAP/SET_GEOM/DELIVER); caller root-gates. */

/* Fill up to `max` winrecs in slot order; returns the count written. */
int uwin_enum(struct winrec *out, int max);

/* Map window `id`'s buffer at `at` (npages), pinned against close; fills bw/bh/stride. 0 / -1. */
int uwin_map(int id, uint32_t at, int *bw, int *bh, int *stride);

/* Drop the compositor's pin on `id` and unmap it from `dir` at `at`. */
int uwin_unmap(int id, uint32_t *dir, uint32_t at);

/* Set a window's frame geometry. */
int uwin_set_geom(int id, int x, int y, int w, int h);

/* Push a client-relative input event into window `id`'s ring. */
int uwin_deliver(int id, const struct winevent *ev);

#endif /* OWOS_UWIN_H */
