/* uwin.c — the kernel windowing broker (see uwin.h).
 *
 * With the window manager moved to user space (/bin/compositor), this file is the
 * SOLE kernel owner of per-window state: each window's client pixel buffer, its
 * on-screen frame geometry, its focus flag, and its input event ring. The
 * compositor owns all layout/z-order/chrome decisions and drives them back in
 * through the SYS_WIN_* primitives; the kernel just records what it's told and
 * reports it.
 *
 * Each user window owns its client buffer: a private, contiguous run of physical
 * RAM from pmm_alloc_contig (above the 16 MiB identity map), NOT the shared
 * kernel heap — so one window (or a big image) can't starve the others, and a
 * buffer can be as large as the whole screen. The run is identity-mapped into the
 * kernel master directory (paging_kmap) and mapped into the owning process at
 * WIN_FB_VADDR (its own high page tables) so the owner draws into it; the
 * compositor maps it a second time (SYS_WIN_MAP) to blit it zero-copy. On
 * window/process teardown the user mapping and its page tables are released BEFORE
 * the dir is freed, the kernel mapping is dropped, and the frames are returned to
 * the PMM.
 */
#include <stdint.h>
#include <stddef.h>
#include "uwin.h"
#include "fb.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "creg.h"
#include "irqflags.h"
#include "sched.h"
#include "timer.h"
#include "loader.h"

/* Window frame chrome sizes. The kernel records geometry in FRAME coordinates (a
 * window's on-screen box including title bar + border); the client buffer is that
 * box inset by these. They must match the compositor's own WM_TITLE_H / WM_BORDER
 * (user/apps/gui/compositor.c), since the compositor draws the chrome and does the
 * screen->client coordinate mapping — the kernel only needs the inset to size the
 * client buffer from a frame geometry set via SYS_WIN_SET_GEOM. */
#define WM_TITLE_H 22
#define WM_BORDER  2
#define WM_BAR_H   28
#define WM_MIN_W   140
#define WM_MIN_H   120

/* No fixed cap on concurrent app windows: the uwins[] slot table grows on the
 * kernel heap (each uwin kmalloc'd, stable so the id can index it), so the only
 * ceiling is memory. The window handle returned to an app is its slot index,
 * which stays valid because slots are never compacted (freed slots go NULL and
 * are reused). */
#define EVQ     64          /* per-window event ring                          */

/* Fill colour for client pixels the app hasn't drawn yet: a fresh window before
 * its first paint, and the area newly exposed by a resize before the app
 * repaints. Matched to the compositor's window frame so an unpainted region reads
 * as part of the window's dark chrome rather than a jarring grey rectangle. */
#define UWIN_BG 0x10141a

struct uwin {
	int            used;
	uint32_t       owner;       /* owning task id                            */
	uint32_t      *owner_dir;   /* its page dir (to unmap the buffer on exit) */
	uint32_t       buf_phys;    /* contiguous client buffer (>16 MiB phys)    */
	uint32_t       buf_bytes;   /* its size, for free/kunmap                  */
	int            npages;
	int            w, h;        /* client size in pixels                     */
	int            fx, fy, fw, fh; /* on-screen FRAME geometry (compositor-set) */
	uint32_t       flags;       /* UWIN_F_* (visible/minimized/maximized)     */
	int            z;           /* monotonic focus stamp: higher = focused later */
	int            cursor;      /* app-requested client-area cursor (enum wm_cursor) */
	char           title[64];
	uint32_t       present_seq; /* bumped by uwin_present                      */
	int            compositor_pinned; /* the compositor SYS_WIN_MAP'd this buffer;
	                               uwin_reap must not free buf_phys until it unmaps */
	int            free_pending; /* detached; buffer still owed (pinned or deferred) */
	volatile int   head, tail;  /* event ring indices                        */
	struct win_event ev[EVQ];
	uint32_t       last_poll;   /* timer_ticks() of the owner's last poll —
	                               its event-loop heartbeat (hang detection)  */
};

static struct uwin **uwins;    /* grown array of kmalloc'd uwins (stable addrs) */
static int           uwins_cap;
static int           z_next = 1;   /* next focus stamp                         */
static int           focused_id = -1;  /* the window SYS_WIN_SET_FOCUS last set */

/* ------------------------------------------------ compositor liveness state.
 * A single "compositor active" flag with the pid that owns the display. Set by
 * the first SYS_FB_MAP (uwin_compositor_register); cleared when that task exits
 * (uwin_task_exiting). kernel_main uses it to decide whether to drain the
 * physical keyboard into the recovery console: while a compositor is alive it
 * owns input (via SYS_INPUT_POLL), so the kernel must NOT also drain it. */
static uint32_t compositor_pid;

void uwin_compositor_register(uint32_t pid)
{
	if (!compositor_pid)
		compositor_pid = pid;
}

int uwin_compositor_active(void)
{
	return compositor_pid != 0;
}

/* Ensure uwins[] can index `need` slots, growing ~2x. New pointer slots are
 * NULL. Returns 0 on heap exhaustion. cli around the swap so an interrupt-context
 * reader never sees a half-swapped table. */
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
	uintptr_t fl = irq_save();
	struct uwin **ou = uwins;
	uwins = nu; uwins_cap = nc;
	if (fl & 0x200) __asm__ volatile ("sti");
	if (ou) kfree(ou);
	return 1;
}

static inline void tlb_flush(void)
{
	/* Reloading CR3 with its current value flushes all non-global TLB entries. */
	write_cr3(read_cr3());
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

/* Free slots in this window's input event ring. Used for paste flow control: a
 * clipboard paste must not push faster than the app drains, or events past the
 * ring's depth are dropped. */
int uwin_key_space(int id)
{
	if (id < 0 || id >= uwins_cap || !uwins[id])
		return 1 << 30;
	struct uwin *u = uwins[id];
	if (!u->used)
		return 1 << 30;
	int used = (u->head - u->tail + EVQ) % EVQ;
	return (EVQ - 1) - used;       /* one slot is always left empty (full test) */
}

/* Largest client buffer a window may have: the whole framebuffer (a window can
 * never usefully exceed the screen). */
static uint32_t win_cap_bytes(void)
{
	uint32_t px = fb_width() * fb_height();
	return px ? px * 4u : WIN_MAX_BYTES;
}

/* ------------------------------------------------------------- teardown */

/* Detach a window's user mapping and hand the buffer to the deferred reaper.
 * Runs in the exiting/killing task's context (sched_exit/sched_kill) or a close
 * syscall. The buffer isn't freed here if the compositor still has it mapped
 * (compositor_pinned): freeing pinned frames would fault the compositor's next
 * blit. uwin_reap() releases them once the compositor SYS_WIN_UNMAPs. */
static void uwin_free(struct uwin *u)
{
	if (!u->used || u->free_pending)
		return;
	/* Detach the buffer from the owner's window (frees its private page tables)
	 * before the dir is torn down, so paging_free_process_dir never touches it. */
	if (u->owner_dir)
		paging_unmap_user_at(u->owner_dir, WIN_FB_VADDR, u->buf_bytes);
	u->owner_dir = 0;
	u->free_pending = 1;
	if (focused_id >= 0 && focused_id < uwins_cap && uwins[focused_id] == u)
		focused_id = -1;
	/* If the compositor isn't holding this buffer, reclaim it now. */
	if (!u->compositor_pinned && u->buf_phys) {
		paging_kunmap(u->buf_phys, u->buf_bytes);
		pmm_free_contig(u->buf_phys, u->buf_bytes);
		u->used = 0;
		u->buf_phys = 0;
		u->free_pending = 0;
	}
}

/* Reclaim client buffers of windows detached while still compositor-pinned, once
 * the compositor has SYS_WIN_UNMAPped them (compositor_pinned cleared). Called
 * from uwin_unmap after a pin is dropped. */
void uwin_reap(void)
{
	for (int i = 0; i < uwins_cap; i++) {
		struct uwin *u = uwins[i];
		if (!u || !u->used || !u->free_pending || u->compositor_pinned)
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
	 * space, off the shared kernel heap), identity-mapped for the compositor. */
	int npages = (int)((bytes + 4095u) / 4096u);
	uint32_t buf_bytes = (uint32_t)npages * 4096u;
	uint32_t phys = pmm_alloc_contig(buf_bytes);
	if (!phys)
		return -1;
	if (paging_kmap(phys, buf_bytes) < 0) {
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}

	/* Map the buffer into the calling process at WIN_FB_VADDR, then clear it
	 * through that user mapping (this runs in the owner's CR3, where the buffer's
	 * kernel identity mapping isn't visible). */
	uint32_t *dir = sched_current_dir();
	if (paging_map_user_at(dir, WIN_FB_VADDR, phys, npages) < 0) {
		paging_kunmap(phys, buf_bytes);
		pmm_free_contig(phys, buf_bytes);
		return -1;
	}
	tlb_flush();
	for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
		((uint32_t *)WIN_FB_VADDR)[i] = UWIN_BG;

	/* Cascade the initial frame diagonally within the screen so a window never
	 * opens off-screen; the compositor may move it later via SYS_WIN_SET_GEOM. */
	int win_w = w + 2 * WM_BORDER;
	int win_h = h + WM_TITLE_H + WM_BORDER;
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

	u->used = 1;
	u->owner = sched_current_id();
	u->owner_dir = dir;
	u->buf_phys = phys;
	u->buf_bytes = buf_bytes;
	u->npages = npages;
	u->w = w;
	u->h = h;
	u->fx = wx; u->fy = wy; u->fw = win_w; u->fh = win_h;
	u->flags = UWIN_F_VISIBLE;
	u->cursor = 0;
	u->compositor_pinned = 0;
	u->free_pending = 0;
	int j = 0;
	for (const char *s = title ? title : "app"; *s && j < (int)sizeof u->title - 1; s++)
		u->title[j++] = *s;
	u->title[j] = '\0';
	u->present_seq = 1;
	u->head = u->tail = 0;
	u->last_poll = timer_ticks();  /* seed the heartbeat */
	u->z = z_next++;               /* newest window starts on top / focused */
	focused_id = slot;
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
	int j = 0;
	for (const char *s = title ? title : ""; *s && j < (int)sizeof u->title - 1; s++)
		u->title[j++] = *s;
	u->title[j] = '\0';
	u->present_seq++;          /* force a repaint so the new title shows */
	return 0;
}

int uwin_set_resizable(int id, int on)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	if (on) u->flags |=  UWIN_F_RESIZABLE;
	else    u->flags &= ~UWIN_F_RESIZABLE;
	return 0;
}

int uwin_set_cursor(int id, int cursor)
{
	struct uwin *u = owned(id);
	if (!u)
		return -1;
	u->cursor = cursor;        /* reported to the compositor via enum (advisory) */
	return 0;
}

void uwin_task_exiting(uint32_t owner_id)
{
	if (owner_id == compositor_pid)
		compositor_pid = 0;    /* compositor died: kernel_main reverts to shell */
	for (int i = 0; i < uwins_cap; i++)
		if (uwins[i] && uwins[i]->used && uwins[i]->owner == owner_id)
			uwin_free(uwins[i]);
}

/* --------------------------------------------- compositor brokering backends */

/* A window is enumerable if it's a live, mapped uwin (not detached-pending). */
static int enumerable(struct uwin *u)
{
	return u && u->used && !u->free_pending && u->buf_phys;
}

int uwin_enum(struct winrec *out, int max)
{
	if (!out || max <= 0)
		return 0;
	int n = 0;
	for (int i = 0; i < uwins_cap && n < max; i++) {
		struct uwin *u = uwins[i];
		if (!enumerable(u))
			continue;
		struct winrec *r = &out[n];
		r->id  = i;
		r->x = u->fx; r->y = u->fy; r->w = u->fw; r->h = u->fh;
		r->flags = u->flags;
		if (i == focused_id)
			r->flags |= UWIN_F_FOCUSED;
		r->present_seq = u->present_seq;
		r->owner_pid = (int)u->owner;
		r->z = u->z;
		r->buf_w = u->w; r->buf_h = u->h;
		int k = 0;
		for (; k < (int)sizeof r->title - 1 && u->title[k]; k++)
			r->title[k] = u->title[k];
		r->title[k] = '\0';
		n++;
	}
	return n;
}

/* Resolve an id to a live uwin regardless of caller (the compositor isn't the
 * owner). NULL if the id is out of range, free, or detached-pending. */
static struct uwin *broker_win(int id)
{
	if (id < 0 || id >= uwins_cap || !uwins[id])
		return 0;
	struct uwin *u = uwins[id];
	if (!u->used || u->free_pending || !u->buf_phys)
		return 0;
	return u;
}

int uwin_map(int id, uint32_t at, int *bw, int *bh, int *stride)
{
	struct uwin *u = broker_win(id);
	if (!u || !u->buf_phys)
		return -1;
	uint32_t *dir = sched_current_dir();
	/* Re-map in place (paging_map_user_at overwrites the PTEs), so re-mapping an
	 * already-pinned window just refreshes it. */
	if (paging_map_user_at(dir, at, u->buf_phys, u->npages) < 0)
		return -1;
	u->compositor_pinned = 1;
	tlb_flush();
	if (bw)     *bw     = u->w;
	if (bh)     *bh     = u->h;
	if (stride) *stride = u->w;   /* client buffer is tightly packed (pitch = w) */
	return 0;
}

int uwin_unmap(int id, uint32_t *dir, uint32_t at)
{
	if (id < 0 || id >= uwins_cap || !uwins[id])
		return -1;
	struct uwin *u = uwins[id];
	if (dir)
		paging_unmap_user_at(dir, at, u->buf_bytes ? u->buf_bytes : WIN_MAP_SLOT_BYTES);
	u->compositor_pinned = 0;      /* the buffer may now be reclaimed if pending */
	tlb_flush();
	uwin_reap();                   /* release a closed-but-pinned buffer now      */
	return 0;
}

/* Grow/shrink the client buffer to fit a new client size (frame minus chrome).
 * The buffer is a private, contiguous run of physical RAM, identity-mapped for
 * the compositor and mapped into the owner at WIN_FB_VADDR. Never shrinks the
 * allocation (a smaller buffer mapped out from under an owner mid-paint would
 * race into an out-of-bounds write); only grows and reallocates. Returns 0 if the
 * logical size was updated, and pushes a WEV_RESIZE so the owner repaints. */
static void uwin_resize_client(struct uwin *u, int id, int cw, int ch)
{
	if (cw < 16) cw = 16;
	if (ch < 16) ch = 16;
	uint32_t maxpix = win_cap_bytes() / 4;
	if ((uint32_t)cw * (uint32_t)ch > maxpix) {
		ch = (int)(maxpix / (uint32_t)cw);
		if (ch < 16) ch = 16;
	}
	if (cw == u->w && ch == u->h)
		return;

	if ((uint32_t)cw * (uint32_t)ch * 4u <= u->buf_bytes) {
		u->w = cw;
		u->h = ch;
		u->present_seq++;
		evq_push(u, WEV_RESIZE, cw, ch);
		return;
	}

	int npages = (int)(((uint32_t)cw * ch * 4u + 4095u) / 4096u);
	uint32_t nbytes = (uint32_t)npages * 4096u;
	uint32_t nphys = pmm_alloc_contig(nbytes);
	if (!nphys || paging_kmap(nphys, nbytes) < 0) {   /* keep the old buffer */
		if (nphys) pmm_free_contig(nphys, nbytes);
		return;
	}
	/* Both buffers live above the 16 MiB identity map and are kmap'd only into
	 * the kernel master directory. This syscall runs under the caller's
	 * (compositor's) CR3, which never inherited those high PDEs — its page dir
	 * was cloned before paging_kmap installed them — so the raw phys addresses
	 * are unmapped here. Reach the frames through the supervisor scratch slots
	 * (present under every CR3), exactly as paging_fork_dir copies frames; IRQs
	 * off around each transient mapping so a preemption can't steal the slot. */
	for (uint32_t pg = 0; pg < (uint32_t)npages; pg++) {
		uintptr_t fl = irq_save();
		uint32_t *v = paging_scratch_map(0, nphys + pg * 4096u);
		for (int k = 0; k < 1024; k++)
			v[k] = UWIN_BG;
		paging_scratch_unmap(0);
		irq_restore(fl);
	}
	/* Carry the old pixels into the new buffer (top-left aligned) so the window
	 * keeps showing its content until the app repaints. One scanline per critical
	 * section bounds the IRQ-off window; slot 0 = source, 1 = dest, re-mapped as a
	 * row crosses a 4 KiB page (1024 px) in either buffer. */
	{
		int copy_w = u->w < cw ? u->w : cw;
		int copy_h = u->h < ch ? u->h : ch;
		uint32_t sphys = u->buf_phys;
		for (int y = 0; y < copy_h; y++) {
			uintptr_t fl = irq_save();
			uint32_t spg = ~0u, dpg = ~0u;
			uint32_t *sv = 0, *dv = 0;
			for (int x = 0; x < copy_w; x++) {
				uint32_t si = (uint32_t)y * (uint32_t)u->w + (uint32_t)x;
				uint32_t di = (uint32_t)y * (uint32_t)cw + (uint32_t)x;
				if ((si >> 10) != spg) { spg = si >> 10; sv = paging_scratch_map(0, sphys + spg * 4096u); }
				if ((di >> 10) != dpg) { dpg = di >> 10; dv = paging_scratch_map(1, nphys + dpg * 4096u); }
				dv[di & 1023] = sv[si & 1023];
			}
			paging_scratch_unmap(1);
			paging_scratch_unmap(0);
			irq_restore(fl);
		}
	}
	/* Remap the owner's WIN_FB_VADDR to the new buffer IN PLACE (overwrite the
	 * PTEs) rather than unmap-then-map: an unmapped gap here would fault a
	 * concurrent paint (the owner draws on its own thread). */
	if (u->owner_dir)
		paging_map_user_at(u->owner_dir, WIN_FB_VADDR, nphys, npages);
	/* If the compositor has this window pinned, remap its slot to the new frames
	 * IN PLACE too: this runs in the compositor's context (SYS_WIN_SET_GEOM), and
	 * it composites the new buffer LATER in the same loop iteration off the pin it
	 * already holds — leaving the pin pointing at the freed old frames would blit
	 * freed memory for one frame. */
	if (u->compositor_pinned) {
		uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
		paging_map_user_at(sched_current_dir(), at, nphys, npages);
	}
	paging_kunmap(u->buf_phys, u->buf_bytes);
	pmm_free_contig(u->buf_phys, u->buf_bytes);

	u->buf_phys = nphys;
	u->buf_bytes = nbytes;
	u->npages = npages;
	u->w = cw;
	u->h = ch;
	u->present_seq++;
	tlb_flush();
	evq_push(u, WEV_RESIZE, cw, ch);
}

int uwin_set_geom(int id, int x, int y, int w, int h)
{
	struct uwin *u = broker_win(id);
	if (!u)
		return -1;
	if (w < WM_MIN_W) w = WM_MIN_W;
	if (h < WM_MIN_H) h = WM_MIN_H;
	u->fx = x; u->fy = y; u->fw = w; u->fh = h;
	uwin_resize_client(u, id, w - 2 * WM_BORDER, h - WM_TITLE_H - WM_BORDER);
	return 0;
}

int uwin_deliver(int id, const struct winevent *ev)
{
	struct uwin *u = broker_win(id);
	if (!u || !ev)
		return -1;
	evq_push(u, ev->type, ev->a, ev->b);
	return 0;
}

int uwin_set_focus(int id)
{
	if (id <= 0) {
		focused_id = -1;           /* "no focus" */
		return 0;
	}
	struct uwin *u = broker_win(id);
	if (!u)
		return -1;
	focused_id = id;
	u->z = z_next++;               /* raise to the top of the stable order */
	return 0;
}
