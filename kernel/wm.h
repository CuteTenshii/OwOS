/* wm.h — window manager / compositor, and the desktop-app interface.
 *
 * The compositor (wm.c) is app-agnostic: it owns the screen, the window pool,
 * z-order, dragging, the cursor and taskbar, and the damage-tracked redraw
 * loop. It knows nothing about what any window *contains*. An app is just a
 * window with three callbacks:
 *
 *   draw(w, cx,cy,cw,ch, focused)  paint the client area (already clipped &
 *                                  inset below the title bar / inside borders)
 *   click(w, mx,my)                a left-press landed in the client area
 *   dirty(w)                       return a value that changes whenever the
 *                                  client needs repainting; the compositor
 *                                  diffs it each frame and damages the window
 *
 * Any of the three may be NULL. To add an app: write a new kernel/app_*.c that
 * calls wm_create_window() and fills in its callbacks, then add one line to
 * apps.c (see apps.h). wm.c itself never needs to change.
 */
#ifndef OWOS_WM_H
#define OWOS_WM_H

#include <stdint.h>

#define WM_TITLE_H   22      /* title-bar height                               */
#define WM_BORDER    2       /* frame border width                            */
#define WM_BAR_H     28      /* taskbar height                                 */
#define WM_TITLE_MAX 64      /* max window title length (incl. NUL)            */
#define WM_GRIP      14      /* bottom-right resize-grip size                  */
#define WM_MIN_W     140     /* smallest a window may be dragged to            */
#define WM_MIN_H     90

/* Cursor shapes. The compositor picks one per frame from context (a hand over
 * clickable chrome, the diagonal arrow over a resize grip, the spinner while a
 * window is still catching up after a live-resize). An app can also request a
 * shape for its own client area via w->cursor / wm_set_cursor (e.g. an I-beam
 * over editable text). WM_CURSOR_ARROW (0) is the default. */
enum wm_cursor {
	WM_CURSOR_ARROW = 0,    /* the plain pointer                              */
	WM_CURSOR_HAND,         /* pointing hand — over clickable things          */
	WM_CURSOR_TEXT,         /* I-beam — over editable / selectable text       */
	WM_CURSOR_BUSY,         /* animated spinner — "working / loading"         */
	WM_CURSOR_RESIZE,       /* diagonal double-arrow — over a resize grip     */
	WM_CURSOR_COUNT
};

struct window {
	int  x, y, w, h;
	char title[WM_TITLE_MAX];
	int  closable;
	int  resizable;          /* 1 by default; an app may clear it after create */
	int  visible;
	int  dead;               /* destroyed; slot reusable by wm_create_window    */
	int  minimized;          /* hidden to the taskbar (visible=0, still listed) */
	int  maximized;          /* filling the screen above the taskbar            */
	int  saved_x, saved_y, saved_w, saved_h;  /* frame geometry before maximize */

	/* App interface (any may be NULL). `cx,cy,cw,ch` is the client rect. */
	void     (*draw)(struct window *w, int cx, int cy, int cw, int ch,
	                 int focused);
	void     (*click)(struct window *w, int mx, int my);
	/* A right-press landed in the client area (mx,my). Apps use it to open a
	 * context menu at the cursor. NULL = the right button is ignored. */
	void     (*rclick)(struct window *w, int mx, int my);
	uint32_t (*dirty)(struct window *w);
	void     (*key)(struct window *w, char c);  /* a key while focused         */
	void     (*on_close)(struct window *w);     /* close box clicked (else hide) */
	void     (*on_resize)(struct window *w, int cw, int ch); /* resize finished  */
	/* The left button is held and moving after a press in this window's client
	 * area (mx,my track the cursor). Lets an app implement a draggable element
	 * such as a scrollbar thumb; pairs with click() for the initial press. */
	void     (*drag)(struct window *w, int mx, int my);
	/* The scroll wheel moved while the cursor was over this window. `dz` is the
	 * notch delta (positive = wheel away from the user / scroll up). */
	void     (*scroll)(struct window *w, int mx, int my, int dz);

	/* Preferred cursor shape (enum wm_cursor) while the pointer is over this
	 * window's client area; 0 (WM_CURSOR_ARROW) = the default pointer. Resize
	 * grips and the close box always win over this. */
	int  cursor;

	void *state;             /* per-window app data (opaque to the WM)         */
};

/* Bring up the compositor (no-op if there is no framebuffer). Creates the
 * desktop apps via apps_start() and spawns the "wm" thread. */
void wm_init(void);

/* Register a window and return it so the caller can fill in draw/click/dirty/
 * state. The newest window starts focused and on top. Returns NULL if the
 * window pool is full. Call only from app init (before/at wm_init time).
 *
 * Isolation invariant: an app only ever receives a handle to its OWN window —
 * the pointer returned here, and the `w` passed to its own callbacks. There is
 * deliberately no API to enumerate windows or look one up by index/title, so an
 * app cannot reach (or mutate the title/geometry/state of) another app's
 * window. Each app's per-window data lives behind `w->state` and in its own
 * file-static variables, private to that module. (Kernel apps still share one
 * address space, so this is an API-level guarantee, not a memory-level one;
 * enforcing it against a hostile app is the job of the future ring-3 windowing
 * syscall layer's per-process handle table — see the wm-app-interface notes.) */
struct window *wm_create_window(int x, int y, int w, int h,
                                const char *title, int closable);

/* Change a window's title bar at runtime (copied; truncated to WM_TITLE_MAX).
 * The new title shows the next time the window is repainted — pair it with a
 * dirty() bump if nothing else is forcing a redraw that frame. */
void wm_set_title(struct window *w, const char *title);

/* Request the cursor shape (enum wm_cursor) shown while the pointer is over
 * this window's client area. Takes effect on the next frame the pointer is
 * inside; pass WM_CURSOR_ARROW to go back to the default pointer. */
void wm_set_cursor(struct window *w, int cursor);

/* Destroy a window: hide it, clear its callbacks, and mark its pool slot
 * reusable by a later wm_create_window. The struct window* must not be used
 * afterwards. (Used by the ring-3 windowing layer when a GUI process exits.) */
void wm_destroy_window(struct window *w);

int wm_screen_w(void);       /* valid once the framebuffer is up               */
int wm_screen_h(void);

/* Route one typed character to the focused window. Returns 1 if that window
 * consumed it (it has a key callback), 0 otherwise — the caller (kernel_main)
 * then falls back to feeding the shell. Safe to call with no framebuffer. */
int wm_feed_key(char c);

/* 1 if the focused window is a GUI app that consumes keyboard input. */
int wm_focus_takes_keys(void);

#endif
