/* uwin.c — ring-3 windowing layer. See uwin.h.
 *
 * Each user window owns its client buffer: a private, contiguous run of physical
 * RAM from pmm_alloc_contig (above the 16 MiB identity map), NOT the shared
 * kernel heap — so one window (or a big image) can't starve the others, and a
 * buffer can be as large as the whole screen. The run is identity-mapped into
 * the kernel master directory (paging_kmap) so the WM thread (kernel CR3) blits
 * it via its physical address, and mapped into the owning process at WIN_FB_VADDR
 * (its own high page tables) so ring-3 draws into it. On window/process teardown
 * the user mapping and its page tables are released BEFORE the dir is freed, the
 * kernel mapping is dropped, and the frames are returned to the PMM.
 */
#include <stdint.h>
#include "uwin.h"
#include "wm.h"
#include "fb.h"
#include "pmm.h"
#include "paging.h"
#include "sched.h"
#include "timer.h"

#define MAXUWIN 32    /* concurrent ring-3 app windows (buffers come from pmm) */
#define EVQ     32          /* per-window event ring                          */

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
	volatile int   head, tail;  /* event ring indices                        */
	struct win_event ev[EVQ];
	uint32_t       last_click;  /* timer_ticks() of the previous click       */
	int            last_cx, last_cy; /* its client-relative position         */
};

/* Double-click: a second click within this many ticks (TIMER_HZ = 250, so
 * 100 ticks = 400 ms) and within DBLCLICK_SLOP px of the first. */
#define DBLCLICK_TICKS 100
#define DBLCLICK_SLOP  8

static struct uwin uwins[MAXUWIN];

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

static void uw_rclick(struct window *w, int mx, int my)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	int cx = mx - (w->x + WM_BORDER);
	int cy = my - (w->y + WM_TITLE_H);
	evq_push(u, WEV_RCLICK, cx, cy);
}

static void uw_key(struct window *w, char c)
{
	struct uwin *u = w->state;
	if (u)
		evq_push(u, WEV_KEY, (unsigned char)c, 0);
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
 * blit it, and mapped into the owner at WIN_FB_VADDR so ring 3 can draw it. The
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

static void uwin_free(struct uwin *u)
{
	if (!u->used)
		return;
	/* Detach the buffer from the owner's window (frees its private page tables)
	 * before the dir is torn down, so paging_free_process_dir never touches it. */
	if (u->owner_dir)
		paging_unmap_user_at(u->owner_dir, WIN_FB_VADDR, u->buf_bytes);
	if (u->win)
		wm_destroy_window(u->win);
	if (u->buf_phys) {
		paging_kunmap(u->buf_phys, u->buf_bytes);
		pmm_free_contig(u->buf_phys, u->buf_bytes);
	}
	u->used = 0;
	u->win = 0;
	u->buf_phys = 0;
	u->owner_dir = 0;
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
	for (int i = 0; i < MAXUWIN; i++)
		if (!uwins[i].used) { slot = i; break; }
	if (slot < 0)
		return -1;
	struct uwin *u = &uwins[slot];

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
	struct window *win = wm_create_window(150 + slot * 26, 90 + slot * 26,
	                                      win_w, win_h, title ? title : "app", 1);
	if (!win) {
		paging_kunmap(phys, buf_bytes);
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}
	win->resizable = 1;            /* resizable by default; an app may opt out */
	win->on_resize = uw_on_resize;
	win->draw = uw_draw;
	win->click = uw_click;
	win->rclick = uw_rclick;
	win->key = uw_key;
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
	u->last_click = 0;             /* fresh slot: no prior click to pair with */
	return slot;
}

/* Validate an id and confirm the caller owns it. */
static struct uwin *owned(int id)
{
	if (id < 0 || id >= MAXUWIN)
		return 0;
	struct uwin *u = &uwins[id];
	if (!u->used || u->owner != sched_current_id())
		return 0;
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
	for (int i = 0; i < MAXUWIN; i++)
		if (uwins[i].used && uwins[i].owner == owner_id)
			uwin_free(&uwins[i]);
}
