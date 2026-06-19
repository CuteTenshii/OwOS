/* uwin.c — windowing layer. See uwin.h.
 *
 * Each user window owns its client buffer: a private, contiguous run of physical
 * RAM from pmm_alloc_contig (above the 16 MiB identity map), NOT the shared
 * kernel heap — so one window (or a big image) can't starve the others, and a
 * buffer can be as large as the whole screen. The run is identity-mapped into
 * the kernel master directory (paging_kmap) so the WM thread (kernel CR3) blits
 * it via its physical address, and mapped into the owning process at WIN_FB_VADDR
 * (its own high page tables) so the owner draws into it. On window/process teardown
 * the user mapping and its page tables are released BEFORE the dir is freed, the
 * kernel mapping is dropped, and the frames are returned to the PMM.
 */
#include <stdint.h>
#include <stddef.h>
#include "uwin.h"
#include "wm.h"
#include "fb.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "sched.h"
#include "timer.h"

/* No fixed cap on concurrent app windows: the uwins[] slot table grows on the
 * kernel heap (each uwin kmalloc'd, stable so win->state can hold it), so the
 * only ceiling is memory. The window handle returned to an app is its slot index,
 * which stays valid because slots are never compacted (freed slots go NULL and
 * are reused). */
#define EVQ     64          /* per-window event ring                          */

/* Fill colour for client pixels the app hasn't drawn yet: a fresh window before
 * its first paint, and the area newly exposed by a resize before the app
 * repaints. Matched to the WM window frame (wm.c draw_window) so an unpainted
 * region reads as part of the window's dark chrome rather than a jarring grey
 * rectangle flashing through. */
#define UWIN_BG 0x10141a

struct uwin {
	int            used;
	uint32_t       owner;       /* owning task id                            */
	uint32_t      *owner_dir;   /* its page dir (to unmap the buffer on exit) */
	struct window *win;         /* the WM window                             */
	uint32_t       buf_phys;    /* contiguous client buffer (>16 MiB phys)    */
	uint32_t       buf_bytes;   /* its size, for free/kunmap                  */
	int            npages;
	int            w, h;        /* client size in pixels                     */
	uint32_t       present_seq; /* bumped by uwin_present -> dirty()          */
	int            free_pending; /* detached; WM thread still owes its buffer */
	volatile int   head, tail;  /* event ring indices                        */
	struct win_event ev[EVQ];
	uint32_t       last_poll;   /* timer_ticks() of the owner's last poll —
	                               its event-loop heartbeat (hang detection)  */
	uint32_t       last_click;  /* timer_ticks() of the previous click       */
	int            last_cx, last_cy; /* its client-relative position         */
};

/* Double-click: a second click within this many ticks (TIMER_HZ = 250, so
 * 100 ticks = 400 ms) and within DBLCLICK_SLOP px of the first. */
#define DBLCLICK_TICKS 100
#define DBLCLICK_SLOP  8

static struct uwin **uwins;    /* grown array of kmalloc'd uwins (stable addrs) */
static int           uwins_cap;

/* Ensure uwins[] can index `need` slots, growing ~2x. New pointer slots are
 * NULL. Returns 0 on heap exhaustion (uwin_create then fails -> the app's
 * win_create returns -1). cli around the swap so the WM thread (uwin_reap) never
 * reads a half-swapped table. */
static int uwins_reserve(int need)
{
	if (need <= uwins_cap)
		return 1;
	int nc = uwins_cap ? uwins_cap * 2 : 16;
	while (nc < need)
		nc *= 2;
	struct uwin **nu = kmalloc((size_t)nc * sizeof *nu);
	if (!nu)
		return 0;
	for (int i = 0; i < uwins_cap; i++) nu[i] = uwins[i];
	for (int i = uwins_cap; i < nc; i++) nu[i] = 0;
	uint32_t fl;
	__asm__ volatile ("pushf; pop %0; cli" : "=r"(fl) :: "memory");
	struct uwin **ou = uwins;
	uwins = nu; uwins_cap = nc;
	if (fl & 0x200) __asm__ volatile ("sti");
	if (ou) kfree(ou);
	return 1;
}

static inline void tlb_flush(void)
{
	uint32_t c;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(c) :: "memory");
	__asm__ volatile ("mov %0, %%cr3" :: "r"(c) : "memory");
}

static void evq_push(struct uwin *u, uint32_t type, int32_t a, int32_t b)
{
	int next = (u->head + 1) % EVQ;
	if (next == u->tail)
		return;                 /* full: drop (rare; the app polls promptly) */
	u->ev[u->head].type = type;
	u->ev[u->head].a = a;
	u->ev[u->head].b = b;
	u->head = next;
}

/* Coalescing push for high-rate pointer motion: if the most recently queued
 * event is also a WEV_MOVE, overwrite it in place instead of appending, so a
 * burst of hover motion can never fill the ring and crowd out clicks or keys. */
static void evq_push_move(struct uwin *u, int32_t a, int32_t b)
{
	if (u->head != u->tail) {
		int last = (u->head - 1 + EVQ) % EVQ;
		if (u->ev[last].type == WEV_MOVE) {
			u->ev[last].a = a;
			u->ev[last].b = b;
			return;
		}
	}
	evq_push(u, WEV_MOVE, a, b);
}

/* ------------------------------------------------------------ WM callbacks */

static void uw_draw(struct window *w, int cx, int cy, int cw, int ch,
                    int focused)
{
	(void)focused;
	struct uwin *u = w->state;
	if (!u)
		return;
	int bw = u->w < cw ? u->w : cw;
	int bh = u->h < ch ? u->h : ch;
	fb_blit(cx, cy, bw, bh, (const uint32_t *)u->buf_phys, u->w);
}

static void uw_click(struct window *w, int mx, int my)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	int cx = mx - (w->x + WM_BORDER);
	int cy = my - (w->y + WM_TITLE_H);

	/* Every click is a WEV_MOUSE (apps keep using it for select/navigate). A
	 * quick second click near the first additionally raises WEV_DBLCLICK, so
	 * apps get double-click without each re-implementing the timing. */
	evq_push(u, WEV_MOUSE, cx, cy);

	uint32_t now = timer_ticks();
	int dx = cx - u->last_cx, dy = cy - u->last_cy;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	if (u->last_click && now - u->last_click <= DBLCLICK_TICKS &&
	    dx <= DBLCLICK_SLOP && dy <= DBLCLICK_SLOP) {
		evq_push(u, WEV_DBLCLICK, cx, cy);
		u->last_click = 0;          /* a 3rd quick click won't re-trigger    */
	} else {
		u->last_click = now;
		u->last_cx = cx;
		u->last_cy = cy;
	}
}

/* The left button is held and moving after a press in our client area. The WM
 * captures the drag (it set app_dragging because we expose this callback) and
 * calls us on every motion until release; we forward it as WEV_DRAG in client
 * coords so an app can drive a draggable element (e.g. a resizable
 * sidebar's divider). There is no release event: motion simply stops, and the
 * next WEV_MOUSE press begins a fresh drag. */
static void uw_drag(struct window *w, int mx, int my)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	int cx = mx - (w->x + WM_BORDER);
	int cy = my - (w->y + WM_TITLE_H);
	evq_push(u, WEV_DRAG, cx, cy);
}

/* The pointer moved over our client area with no button held (or left it, which
 * the WM signals with mx<0). Forwarded as WEV_MOVE so an app can react to hover
 * — e.g. show a hovered link's target in a status bar. Consecutive moves
 * coalesce in the ring (evq_push_move) so fast motion can't outrun the drain. */
static void uw_hover(struct window *w, int mx, int my)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	if (mx < 0)
		evq_push_move(u, -1, -1);                       /* pointer left */
	else
		evq_push_move(u, mx - (w->x + WM_BORDER), my - (w->y + WM_TITLE_H));
}

static void uw_rclick(struct window *w, int mx, int my)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	int cx = mx - (w->x + WM_BORDER);
	int cy = my - (w->y + WM_TITLE_H);
	evq_push(u, WEV_RCLICK, cx, cy);
}

static void uw_key(struct window *w, char c, unsigned mods)
{
	struct uwin *u = w->state;
	if (u)
		evq_push(u, WEV_KEY, (unsigned char)c, (int)mods);   /* b = KMOD_* flags */
}

/* Free slots in this window's input event ring. Used for paste flow control: a
 * clipboard paste must not push faster than the app drains, or events
 * past the ring's depth are dropped (a long URL pasted only up to ~EVQ chars).
 * Returns a large number for any window that isn't one of ours (non-uwin key
 * sinks have no such ring and are never throttled). */
int uwin_key_space(struct window *w)
{
	if (w->key != uw_key)
		return 1 << 30;
	struct uwin *u = w->state;
	if (!u)
		return 1 << 30;
	int used = (u->head - u->tail + EVQ) % EVQ;
	return (EVQ - 1) - used;       /* one slot is always left empty (full test) */
}

/* A window is "not responding" when events are queued for its owner but the
 * owner hasn't polled within this window — i.e. its event loop has stalled. The
 * WM shows "(Not Responding)" and turns the close box into a force-kill. Chosen
 * over a bare last-poll age so a healthy-but-idle app (which still polls every
 * frame, draining nothing) is never flagged, and a hung one is flagged only once
 * there is actually input it's failing to service. */
#define UWIN_HANG_TICKS (TIMER_HZ * 3)         /* 3 s of unserviced input */

int uwin_not_responding(struct window *w)
{
	if (!w || w->key != uw_key)            /* not a GUI-app window */
		return 0;
	struct uwin *u = w->state;
	if (!u || !u->used || u->free_pending)
		return 0;
	if (u->head == u->tail)                /* ring empty: nothing to ignore */
		return 0;
	return (timer_ticks() - u->last_poll) > UWIN_HANG_TICKS;
}

uint32_t uwin_owner_pid(struct window *w)
{
	if (!w || w->key != uw_key)
		return 0;
	struct uwin *u = w->state;
	if (!u || !u->used || u->free_pending)
		return 0;
	return u->owner;
}

/* Mouse wheel over the window: the WM already routes wheel deltas to the
 * focused window's scroll callback; forward them as an event so an app
 * (the terminal) can scroll its own content. `dz` is +up / -down. */
static void uw_scroll(struct window *w, int mx, int my, int dz)
{
	struct uwin *u = w->state;
	(void)mx; (void)my;
	if (u)
		evq_push(u, WEV_SCROLL, dz, 0);
}

static uint32_t uw_dirty(struct window *w)
{
	struct uwin *u = w->state;
	return u ? u->present_seq : 0;
}

static void uw_on_close(struct window *w)
{
	struct uwin *u = w->state;
	if (u)
		evq_push(u, WEV_CLOSE, 0, 0);   /* let the app exit cleanly */
}

/* Largest client buffer a window may have: the whole framebuffer (a window can
 * never usefully exceed the screen, and the WM clamps it there). */
static uint32_t win_cap_bytes(void)
{
	uint32_t px = fb_width() * fb_height();
	return px ? px * 4u : WIN_MAX_BYTES;
}

/* A resize finished: grow/shrink the per-window client buffer. The buffer is a
 * private, contiguous run of physical RAM (its own space — not the shared kernel
 * heap), identity-mapped into the kernel master dir so the WM compositor can
 * blit it, and mapped into the owner at WIN_FB_VADDR so the owner can draw it. The
 * owner's TLB picks up the new mapping on its next schedule (CR3 reload). Runs
 * in the WM thread (CR3 = page_directory), so it touches the buffer through its
 * identity (physical) address, which the kernel mapping makes readable. */
static void uw_on_resize(struct window *w, int cw, int ch)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	if (cw < 16) cw = 16;
	if (ch < 16) ch = 16;
	uint32_t maxpix = win_cap_bytes() / 4;     /* cap to a full-screen buffer   */
	if ((uint32_t)cw * (uint32_t)ch > maxpix) {
		ch = (int)(maxpix / (uint32_t)cw);
		if (ch < 16) ch = 16;
		w->h = ch + WM_TITLE_H + WM_BORDER;    /* snap the frame to the cap     */
	}

	int npages = (int)(((uint32_t)cw * ch * 4u + 4095u) / 4096u);
	uint32_t nbytes = (uint32_t)npages * 4096u;
	uint32_t nphys = pmm_alloc_contig(nbytes);
	if (!nphys || paging_kmap(nphys, nbytes) < 0) {   /* keep the old buffer     */
		if (nphys) pmm_free_contig(nphys, nbytes);
		w->w = u->w + 2 * WM_BORDER;
		w->h = u->h + WM_TITLE_H + WM_BORDER;
		return;
	}
	for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
		((uint32_t *)nphys)[i] = UWIN_BG;

	/* Carry the old pixels into the new buffer (top-left aligned) so the window
	 * keeps showing its content until the app repaints — a slow, busy, or
	 * resize-unaware app must not leave a blank gray rectangle. */
	{
		uint32_t *dst = (uint32_t *)nphys;
		const uint32_t *src = (const uint32_t *)u->buf_phys;
		int copy_w = u->w < cw ? u->w : cw;
		int copy_h = u->h < ch ? u->h : ch;
		for (int y = 0; y < copy_h; y++)
			for (int x = 0; x < copy_w; x++)
				dst[y * cw + x] = src[y * u->w + x];
	}

	paging_unmap_user_at(u->owner_dir, WIN_FB_VADDR, u->buf_bytes);
	paging_map_user_at(u->owner_dir, WIN_FB_VADDR, nphys, npages);
	paging_kunmap(u->buf_phys, u->buf_bytes);
	pmm_free_contig(u->buf_phys, u->buf_bytes);

	u->buf_phys = nphys;
	u->buf_bytes = nbytes;
	u->npages = npages;
	u->w = cw;
	u->h = ch;
	u->present_seq++;
	evq_push(u, WEV_RESIZE, cw, ch);
}

/* ------------------------------------------------------------- teardown */

/* Detach a window, but DON'T free its client buffer here. uwin_free runs in the
 * exiting/killing task's context (sched_exit/sched_kill) or a close syscall --
 * never the WM thread. The WM blits buf_phys in fb_blit with IF=1, so a timer
 * tick can suspend it mid-blit with buf_phys in hand; kunmap+free here would pull
 * the pages out from under that in-progress blit -> a not-present supervisor read
 * in the wm task at buf_phys (~3 GiB on a big-RAM guest: the real-hardware page
 * fault). So detach now and hand the buffer to the WM thread, which reclaims it
 * from uwin_reap() where it's serialized with compose()/uw_draw. */
static void uwin_free(struct uwin *u)
{
	if (!u->used || u->free_pending)
		return;
	/* Detach the buffer from the owner's window (frees its private page tables)
	 * before the dir is torn down, so paging_free_process_dir never touches it. */
	if (u->owner_dir)
		paging_unmap_user_at(u->owner_dir, WIN_FB_VADDR, u->buf_bytes);
	if (u->win)
		wm_destroy_window(u->win);   /* WM stops dispatching uw_draw for it     */
	u->win = 0;
	u->owner_dir = 0;
	u->free_pending = 1;             /* WM reclaims buf_phys in uwin_reap()      */
}

/* Reclaim the client buffers of windows torn down from another context. Runs in
 * the WM thread (the compositor loop calls it before compose()), so freeing
 * buf_phys is serialized with uw_draw/fb_blit -- the pages are released only when
 * the WM is provably not mid-blit. */
void uwin_reap(void)
{
	for (int i = 0; i < uwins_cap; i++) {
		struct uwin *u = uwins[i];
		if (!u || !u->used || !u->free_pending)
			continue;
		if (u->buf_phys) {
			paging_kunmap(u->buf_phys, u->buf_bytes);
			pmm_free_contig(u->buf_phys, u->buf_bytes);
		}
		u->used = 0;
		u->buf_phys = 0;
		u->free_pending = 0;
	}
}

/* -------------------------------------------------------------- syscalls */

int uwin_create(int w, int h, const char *title)
{
	if (w < 16 || h < 16 || w > 4096 || h > 4096)
		return -1;
	uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
	if (bytes == 0 || bytes > win_cap_bytes())
		return -1;

	int slot = -1;
	for (int i = 0; i < uwins_cap; i++)
		if (!uwins[i] || !uwins[i]->used) { slot = i; break; }
	if (slot < 0) {                       /* table full: grow; new slot at old cap */
		slot = uwins_cap;
		if (!uwins_reserve(slot + 1))
			return -1;
	}
	if (!uwins[slot]) {                   /* fresh slot: allocate + zero the uwin */
		uwins[slot] = kmalloc(sizeof(struct uwin));
		if (!uwins[slot])
			return -1;
		uint8_t *z = (uint8_t *)uwins[slot];
		for (unsigned k = 0; k < sizeof(struct uwin); k++) z[k] = 0;
	}
	struct uwin *u = uwins[slot];

	/* The client buffer is a private, contiguous run of physical RAM (its own
	 * space, off the shared kernel heap), identity-mapped for the WM to blit. */
	int npages = (int)((bytes + 4095u) / 4096u);
	uint32_t buf_bytes = (uint32_t)npages * 4096u;
	uint32_t phys = pmm_alloc_contig(buf_bytes);
	if (!phys)
		return -1;
	if (paging_kmap(phys, buf_bytes) < 0) {
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}

	/* The WM window framing the client buffer. */
	int win_w = w + 2 * WM_BORDER;
	int win_h = h + WM_TITLE_H + WM_BORDER;
	/* Cascade new windows diagonally, but WRAP within the screen and CLAMP so a
	 * window never opens off-screen (slot*26 used to march past the right/bottom
	 * edge once enough windows were open). The usable range keeps the whole frame
	 * on screen and above the taskbar; an oversized window pins to the top-left. */
	int sw = (int)fb_width(), sh = (int)fb_height();
	int step = 26, margin = 40;
	int rng_x = sw - win_w - margin;
	int rng_y = sh - WM_BAR_H - win_h - margin;
	if (rng_x < step) rng_x = step;
	if (rng_y < step) rng_y = step;
	int wx = margin + (slot * step) % rng_x;
	int wy = margin + (slot * step) % rng_y;
	if (wx + win_w > sw) wx = sw > win_w ? sw - win_w : 0;
	if (wy + win_h > sh - WM_BAR_H) wy = (sh - WM_BAR_H) > win_h ? sh - WM_BAR_H - win_h : 0;
	struct window *win = wm_create_window(wx, wy, win_w, win_h,
	                                      title ? title : "app", 1);
	if (!win) {
		paging_kunmap(phys, buf_bytes);
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}
	win->resizable = 1;            /* resizable by default; an app may opt out */
	win->on_resize = uw_on_resize;
	win->draw = uw_draw;
	win->click = uw_click;
	win->drag = uw_drag;
	win->hover = uw_hover;
	win->rclick = uw_rclick;
	win->key = uw_key;
	win->scroll = uw_scroll;
	win->dirty = uw_dirty;
	win->on_close = uw_on_close;
	win->state = u;

	/* Map the buffer into the calling process at WIN_FB_VADDR, then clear it
	 * through that user mapping (this runs in the owner's CR3, where the buffer's
	 * kernel identity mapping isn't visible). */
	uint32_t *dir = sched_current_dir();
	if (paging_map_user_at(dir, WIN_FB_VADDR, phys, npages) < 0) {
		wm_destroy_window(win);
		paging_unmap_user_at(dir, WIN_FB_VADDR, buf_bytes);
		paging_kunmap(phys, buf_bytes);
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}
	tlb_flush();
	for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
		((uint32_t *)WIN_FB_VADDR)[i] = UWIN_BG;          /* clear to a bg     */

	u->used = 1;
	u->owner = sched_current_id();
	u->owner_dir = dir;
	u->win = win;
	u->buf_phys = phys;
	u->buf_bytes = buf_bytes;
	u->npages = npages;
	u->w = w;
	u->h = h;
	u->present_seq = 1;
	u->head = u->tail = 0;
	u->last_poll = timer_ticks();  /* seed the heartbeat so a pre-first-poll
	                                  event can't read as an instant hang     */
	u->last_click = 0;             /* fresh slot: no prior click to pair with */
	return slot;
}

/* Validate an id and confirm the caller owns it. */
static struct uwin *owned(int id)
{
	if (id < 0 || id >= uwins_cap || !uwins[id])
		return 0;
	struct uwin *u = uwins[id];
	if (!u->used || u->free_pending || u->owner != sched_current_id())
		return 0;                  /* detached-but-not-yet-reaped: gone        */
	return u;
}

int uwin_present(int id)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	u->present_seq++;
	return 0;
}

int uwin_poll(int id, struct win_event *out)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	u->last_poll = timer_ticks();  /* heartbeat: the owner's loop is alive */
	if (u->tail == u->head)
		return 0;
	*out = u->ev[u->tail];
	u->tail = (u->tail + 1) % EVQ;
	return 1;
}

int uwin_close(int id)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	uwin_free(u);
	return 0;
}

int uwin_set_title(int id, const char *title)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	wm_set_title(u->win, title);
	u->present_seq++;          /* force a repaint so the new title shows */
	return 0;
}

int uwin_set_resizable(int id, int on)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	u->win->resizable = on ? 1 : 0;
	u->win->on_resize = on ? uw_on_resize : 0;
	return 0;
}

int uwin_set_cursor(int id, int cursor)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	/* wm_set_cursor range-checks the shape; an out-of-range value is ignored. */
	wm_set_cursor(u->win, cursor);
	return 0;
}

void uwin_task_exiting(uint32_t owner_id)
{
	for (int i = 0; i < uwins_cap; i++)
		if (uwins[i] && uwins[i]->used && uwins[i]->owner == owner_id)
			uwin_free(uwins[i]);
}
