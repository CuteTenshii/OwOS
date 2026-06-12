/* uwin.h — the ring-3 windowing layer (user-window server).
 *
 * Lets a user process own a desktop window: it creates one, gets a shared
 * client-area pixel buffer mapped into its address space (at WIN_FB_VADDR),
 * draws into it, presents frames, and reads input events. The kernel side
 * keeps a struct window in the WM whose draw() blits the user's buffer and
 * whose click/key callbacks enqueue events for the process to poll.
 *
 * Reached from ring 3 through the SYS_WIN_* syscalls (see syscall.c); the
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

struct win_event {
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

/* Called from sched_exit(): free every window owned by the exiting task. */
void uwin_task_exiting(uint32_t owner_id);

#endif /* OWOS_UWIN_H */
