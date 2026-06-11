/* uwin.c — ring-3 windowing layer. See uwin.h.
 *
 * Each user window owns a contiguous, page-aligned client buffer carved from
 * the kernel heap. The heap is identity-mapped, so the WM thread (kernel CR3)
 * reads the buffer directly to blit it; the same physical pages are also mapped
 * into the owning process's user window at WIN_FB_VADDR so ring-3 can draw into
 * them. On window/process teardown the pages are unmapped from the user dir
 * BEFORE its dir is freed — they are heap frames, not pool frames, so letting
 * paging_free_process_dir pfree() them would corrupt the page-frame pool.
 */
#include <stdint.h>
#include "uwin.h"
#include "wm.h"
#include "fb.h"
#include "heap.h"
#include "paging.h"
#include "sched.h"

#define MAXUWIN 4
#define EVQ     32          /* per-window event ring                          */

struct uwin {
	int            used;
	uint32_t       owner;       /* owning task id                            */
	uint32_t      *owner_dir;   /* its page dir (to unmap the buffer on exit) */
	struct window *win;         /* the WM window                             */
	void          *buf_raw;     /* kmalloc base (for kfree)                   */
	uint32_t       buf_phys;    /* page-aligned buffer start (== kernel addr) */
	int            npages;
	int            w, h;        /* client size in pixels                     */
	uint32_t       present_seq; /* bumped by uwin_present -> dirty()          */
	volatile int   head, tail;  /* event ring indices                        */
	struct win_event ev[EVQ];
};

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
	if (u)
		evq_push(u, WEV_MOUSE, mx - (w->x + WM_BORDER),
		         my - (w->y + WM_TITLE_H));
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

/* A resize finished: reallocate the shared client buffer to the new size,
 * remap it into the owner's window, and hand the app a WEV_RESIZE so it
 * re-lays-out. (Only windows whose app opted in via uwin_set_resizable get
 * here.) The owner's TLB picks up the new mapping on its next schedule (CR3
 * reload), so no cross-address-space flush is needed. */
static void uw_on_resize(struct window *w, int cw, int ch)
{
	struct uwin *u = w->state;
	if (!u)
		return;
	if (cw < 16) cw = 16;
	if (ch < 16) ch = 16;
	uint32_t maxpix = WIN_MAX_BYTES / 4;       /* cap to the buffer's capacity  */
	if ((uint32_t)cw * (uint32_t)ch > maxpix) {
		ch = (int)(maxpix / (uint32_t)cw);
		if (ch < 16) ch = 16;
		w->h = ch + WM_TITLE_H + WM_BORDER;    /* snap the frame to the cap     */
	}

	int npages = (int)(((uint32_t)cw * ch * 4u + 4095u) / 4096u);
	void *raw = kmalloc((uint32_t)(npages + 1) * 4096u);
	if (!raw) {                                /* keep the old buffer/size      */
		w->w = u->w + 2 * WM_BORDER;
		w->h = u->h + WM_TITLE_H + WM_BORDER;
		return;
	}
	uint32_t base = ((uint32_t)raw + 4095u) & ~4095u;
	for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
		((uint32_t *)base)[i] = 0xFF202830;

	for (int p = 0; p < u->npages; p++)
		paging_unmap_user_page(u->owner_dir, WIN_FB_VADDR + (uint32_t)p * 4096);
	for (int p = 0; p < npages; p++)
		paging_map_user_page(u->owner_dir, WIN_FB_VADDR + (uint32_t)p * 4096,
		                     base + (uint32_t)p * 4096);
	kfree(u->buf_raw);
	u->buf_raw = raw;
	u->buf_phys = base;
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
	/* Detach the buffer from the owner's user window first, so the later
	 * paging_free_process_dir() never pfree()s these (heap, not pool) pages. */
	if (u->owner_dir)
		for (int p = 0; p < u->npages; p++)
			paging_unmap_user_page(u->owner_dir,
			                       WIN_FB_VADDR + (uint32_t)p * 4096);
	if (u->win)
		wm_destroy_window(u->win);
	if (u->buf_raw)
		kfree(u->buf_raw);
	u->used = 0;
	u->win = 0;
	u->buf_raw = 0;
	u->owner_dir = 0;
}

/* -------------------------------------------------------------- syscalls */

int uwin_create(int w, int h, const char *title)
{
	if (w < 16 || h < 16 || w > 4096 || h > 4096)
		return -1;
	uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
	if (bytes == 0 || bytes > WIN_MAX_BYTES)
		return -1;

	int slot = -1;
	for (int i = 0; i < MAXUWIN; i++)
		if (!uwins[i].used) { slot = i; break; }
	if (slot < 0)
		return -1;
	struct uwin *u = &uwins[slot];

	int npages = (int)((bytes + 4095u) / 4096u);
	void *raw = kmalloc((uint32_t)(npages + 1) * 4096u);  /* +1 to page-align */
	if (!raw)
		return -1;
	uint32_t base = ((uint32_t)raw + 4095u) & ~4095u;
	for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
		((uint32_t *)base)[i] = 0xFF202830;               /* clear to a bg     */

	/* The WM window framing the client buffer (not resizable: buffer is fixed). */
	int win_w = w + 2 * WM_BORDER;
	int win_h = h + WM_TITLE_H + WM_BORDER;
	struct window *win = wm_create_window(150 + slot * 26, 90 + slot * 26,
	                                      win_w, win_h, title ? title : "app", 1);
	if (!win) {
		kfree(raw);
		return -1;
	}
	win->resizable = 0;
	win->draw = uw_draw;
	win->click = uw_click;
	win->key = uw_key;
	win->dirty = uw_dirty;
	win->on_close = uw_on_close;
	win->state = u;

	/* Map the buffer into the calling process's user window at WIN_FB_VADDR. */
	uint32_t *dir = sched_current_dir();
	for (int p = 0; p < npages; p++)
		paging_map_user_page(dir, WIN_FB_VADDR + (uint32_t)p * 4096,
		                     base + (uint32_t)p * 4096);
	tlb_flush();

	u->used = 1;
	u->owner = sched_current_id();
	u->owner_dir = dir;
	u->win = win;
	u->buf_raw = raw;
	u->buf_phys = base;
	u->npages = npages;
	u->w = w;
	u->h = h;
	u->present_seq = 1;
	u->head = u->tail = 0;
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

void uwin_task_exiting(uint32_t owner_id)
{
	for (int i = 0; i < MAXUWIN; i++)
		if (uwins[i].used && uwins[i].owner == owner_id)
			uwin_free(&uwins[i]);
}
