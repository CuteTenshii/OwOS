/* uwin.c: kernel windowing broker (see uwin.h).
 * Sole kernel owner of per-window state; the compositor owns layout/z/focus.
 * Client buffers are private contiguous PMM runs, not kernel heap. */
#include "uwin.h"
#include "creg.h"
#include "fb.h"
#include "heap.h"
#include "irqflags.h"
#include "loader.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include <stddef.h>
#include <stdint.h>

/* Frame chrome insets; must match the compositor's WM_TITLE_H / WM_BORDER. */
#define WM_TITLE_H 22
#define WM_BORDER 2
#define WM_BAR_H 28
#define WM_MIN_W 140
#define WM_MIN_H 120

/* uwins[] grows on the heap; a window id is its slot index (slots never compact). */
#define EVQ 64

/* Fill for not-yet-painted client pixels; matches the compositor's frame. */
#define UWIN_BG 0x10141a

struct uwin {
  int used;
  uint32_t owner;
  uint32_t *owner_dir; /* page dir, to unmap the buffer on exit */
  uint32_t buf_phys;   /* contiguous client buffer */
  uint32_t buf_bytes;
  uint32_t user_vaddr; /* buffer mapping in the owner */
  int parent;          /* transient parent id, or -1 */
  int npages;
  int w, h;           /* client size in pixels */
  int fx, fy, fw, fh; /* on-screen frame geometry */
  uint32_t flags;     /* UWIN_F_* */
  int cursor;         /* WIN_CURSOR_* */
  char title[64];
  uint32_t present_seq;
  bool dirty_full;
  int dx0, dy0, dx1, dy1; /* client damage since last uwin_enum */
  bool compositor_pinned; /* uwin_reap must not free buf_phys while set */
  bool free_pending;      /* detached, buffer still owed */
  volatile int head, tail;
  struct win_event ev[EVQ];
  uint32_t last_poll; /* timer_ticks() of last poll (hang detection) */
};

static struct uwin **uwins;
static int uwins_cap;

/* Pid of the compositor owning the display (set on first SYS_FB_MAP). */
static uint32_t compositor_pid;

void uwin_compositor_register(uint32_t pid)
{
  if (!compositor_pid)
    compositor_pid = pid;
}

bool uwin_compositor_active(void) { return compositor_pid != 0; }

/* Grow uwins[] to `need` slots (~2x); 0 on heap exhaustion. cli around the swap for IRQ readers. */
static bool uwins_reserve(int need)
{
  if (need <= uwins_cap)
    return true;
  int nc = uwins_cap ? uwins_cap * 2 : 16;
  while (nc < need)
    nc *= 2;
  struct uwin **nu = kmalloc((size_t)nc * sizeof *nu);
  if (!nu)
    return false;
  for (int i = 0; i < uwins_cap; i++)
    nu[i] = uwins[i];
  for (int i = uwins_cap; i < nc; i++)
    nu[i] = 0;
  uintptr_t fl = irq_save();
  struct uwin **ou = uwins;
  uwins = nu;
  uwins_cap = nc;
  if (fl & 0x200)
    __asm__ volatile("sti");
  if (ou)
    kfree(ou);
  return true;
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
    return; /* full: drop (rare; the app polls promptly) */
  u->ev[u->head].type = type;
  u->ev[u->head].a = a;
  u->ev[u->head].b = b;
  u->head = next;
}

/* Free slots in the event ring (paste flow control). */
int uwin_key_space(int id)
{
  if (id < 0 || id >= uwins_cap || !uwins[id])
    return 1 << 30;
  struct uwin *u = uwins[id];
  if (!u->used)
    return 1 << 30;
  int used = (u->head - u->tail + EVQ) % EVQ;
  return (EVQ - 1) - used;
}

/* Largest client buffer: the whole framebuffer. */
static uint32_t win_cap_bytes(void)
{
  uint32_t px = fb_width() * fb_height();
  uint32_t cap = px ? px * 4u : WIN_MAX_BYTES;
  /* A larger buffer would overrun the compositor's per-window map slot. */
  if (cap > WIN_MAP_SLOT_BYTES)
    cap = WIN_MAP_SLOT_BYTES;
  return cap;
}

/* Per-owner buffer slots span [WIN_FB_VADDR, COMPOSITOR_FB_VADDR), capping windows per process. */
#define WIN_FB_SLOTS ((int)((COMPOSITOR_FB_VADDR - WIN_FB_VADDR) / WIN_MAP_SLOT_BYTES))

/* Lowest free buffer slot in `dir` (shared by threads), or -1. */
static int alloc_dir_slot(const uint32_t *dir)
{
  uint32_t used = 0;
  for (int i = 0; i < uwins_cap; i++) {
    struct uwin *u = uwins[i];
    if (!u || !u->used || u->owner_dir != dir)
      continue;
    int s = (int)((u->user_vaddr - WIN_FB_VADDR) / WIN_MAP_SLOT_BYTES);
    if (s >= 0 && s < WIN_FB_SLOTS)
      used |= 1u << s;
  }
  for (int s = 0; s < WIN_FB_SLOTS; s++)
    if (!(used & (1u << s)))
      return s;
  return -1;
}

/* Detach the user mapping; defer freeing to uwin_reap() while compositor_pinned. */
static void uwin_free(struct uwin *u)
{
  if (!u->used || u->free_pending)
    return;
  /* Detach before the dir is torn down so paging_free_process_dir skips it. */
  if (u->owner_dir)
    paging_unmap_user_at(u->owner_dir, u->user_vaddr, u->buf_bytes);
  u->owner_dir = 0;
  u->free_pending = true;
  /* If the compositor isn't holding this buffer, reclaim it now. */
  if (!u->compositor_pinned && u->buf_phys) {
    paging_kunmap(u->buf_phys, u->buf_bytes);
    pmm_free_contig(u->buf_phys, u->buf_bytes);
    u->used = 0;
    u->buf_phys = 0;
    u->free_pending = false;
  }
}

/* Reclaim buffers of detached windows once the compositor has unmapped them. */
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
    u->free_pending = false;
  }
}

int uwin_create(int w, int h, const char *title)
{
  if (w < 16 || h < 16 || w > 4096 || h > 4096)
    return -1;
  uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
  if (bytes == 0 || bytes > win_cap_bytes())
    return -1;

  int slot = -1;
  for (int i = 0; i < uwins_cap; i++)
    if (!uwins[i] || !uwins[i]->used) {
      slot = i;
      break;
    }
  if (slot < 0) {
    slot = uwins_cap;
    if (!uwins_reserve(slot + 1))
      return -1;
  }
  if (!uwins[slot]) {
    uwins[slot] = kmalloc(sizeof(struct uwin));
    if (!uwins[slot])
      return -1;
    uint8_t *z = (uint8_t *)uwins[slot];
    for (unsigned k = 0; k < sizeof(struct uwin); k++)
      z[k] = 0;
  }
  struct uwin *u = uwins[slot];

  /* Client buffer: private contiguous PMM run, identity-mapped for the compositor. Reserve a slot first. */
  uint32_t owner = sched_current_id();
  uint32_t *dir = sched_current_dir();
  int fbslot = alloc_dir_slot(dir);
  if (fbslot < 0)
    return -1;
  uint32_t user_vaddr = WIN_FB_VADDR + (uint32_t)fbslot * WIN_MAP_SLOT_BYTES;

  int npages = (int)((bytes + 4095u) / 4096u);
  uint32_t buf_bytes = (uint32_t)npages * 4096u;
  uint32_t phys = pmm_alloc_contig(buf_bytes);
  if (!phys)
    return -1;
  if (paging_kmap(phys, buf_bytes) < 0) {
    pmm_free_contig(phys, buf_bytes);
    return -1;
  }

  /* Map into the owner and clear via that mapping (the kernel identity map is not visible in its CR3). */
  if (paging_map_user_at(dir, user_vaddr, phys, npages) < 0) {
    paging_kunmap(phys, buf_bytes);
    pmm_free_contig(phys, buf_bytes);
    return -1;
  }
  tlb_flush();
  for (uint32_t i = 0; i < (uint32_t)npages * 1024u; i++)
    ((uint32_t *)(uintptr_t)user_vaddr)[i] = UWIN_BG;

  /* Cascade the initial frame so it opens on-screen. */
  int win_w = w + 2 * WM_BORDER;
  int win_h = h + WM_TITLE_H + WM_BORDER;
  int scale = fb_ui_scale_active();
  int sw = (int)fb_width() / scale, sh = (int)fb_height() / scale;
  int step = 26, margin = 40;
  int rng_x = sw - win_w - margin;
  int rng_y = sh - WM_BAR_H - win_h - margin;
  if (rng_x < step)
    rng_x = step;
  if (rng_y < step)
    rng_y = step;
  int wx = margin + (slot * step) % rng_x;
  int wy = margin + (slot * step) % rng_y;
  if (wx + win_w > sw)
    wx = sw > win_w ? sw - win_w : 0;
  if (wy + win_h > sh - WM_BAR_H)
    wy = (sh - WM_BAR_H) > win_h ? sh - WM_BAR_H - win_h : 0;

  u->used = 1;
  u->owner = owner;
  u->owner_dir = dir;
  u->buf_phys = phys;
  u->buf_bytes = buf_bytes;
  u->user_vaddr = user_vaddr;
  u->parent = -1;
  u->npages = npages;
  u->w = w;
  u->h = h;
  u->fx = wx;
  u->fy = wy;
  u->fw = win_w;
  u->fh = win_h;
  u->flags = UWIN_F_VISIBLE | UWIN_F_RESIZABLE;
  u->cursor = 0;
  u->compositor_pinned = false;
  u->free_pending = false;
  int j = 0;
  for (const char *s = title ? title : "app"; *s && j < (int)sizeof u->title - 1; s++)
    u->title[j++] = *s;
  u->title[j] = '\0';
  u->present_seq = 1;
  u->dirty_full = true;
  u->dx0 = u->dy0 = u->dx1 = u->dy1 = 0;
  u->head = u->tail = 0;
  u->last_poll = timer_ticks();
  return slot;
}

/* Validate an id and confirm the caller owns it. */
static struct uwin *owned(int id)
{
  if (id < 0 || id >= uwins_cap || !uwins[id])
    return 0;
  struct uwin *u = uwins[id];
  if (!u->used || u->free_pending || u->owner != sched_current_id())
    return 0;
  return u;
}

/* The frame needs a full repaint (chrome, title, resize). */
static void touch_full(struct uwin *u)
{
  uintptr_t fl = irq_save();
  u->present_seq++;
  u->dirty_full = true;
  irq_restore(fl);
}

/* w <= 0 or h <= 0 means the whole client area. */
int uwin_present(int id, int x, int y, int w, int h)
{
  struct uwin *u = owned(id);
  if (!u)
    return -1;
  int x1 = x + w, y1 = y + h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x1 > u->w)
    x1 = u->w;
  if (y1 > u->h)
    y1 = u->h;
  uintptr_t fl = irq_save();
  u->present_seq++;
  if (w <= 0 || h <= 0) {
    u->dirty_full = true;
  } else if (x1 > x && y1 > y) {
    if (u->dx1 <= u->dx0 || u->dy1 <= u->dy0) {
      u->dx0 = x;
      u->dy0 = y;
      u->dx1 = x1;
      u->dy1 = y1;
    } else {
      if (x < u->dx0)
        u->dx0 = x;
      if (y < u->dy0)
        u->dy0 = y;
      if (x1 > u->dx1)
        u->dx1 = x1;
      if (y1 > u->dy1)
        u->dy1 = y1;
    }
  }
  irq_restore(fl);
  return 0;
}

int uwin_poll(int id, struct win_event *out)
{
  struct uwin *u = owned(id);
  if (!u)
    return -1;
  u->last_poll = timer_ticks();
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
  touch_full(u);
  return 0;
}

int uwin_set_resizable(int id, int on)
{
  struct uwin *u = owned(id);
  if (!u)
    return -1;
  if (on)
    u->flags |= UWIN_F_RESIZABLE;
  else
    u->flags &= ~UWIN_F_RESIZABLE;
  return 0;
}

int uwin_set_fullscreen(int id, int on)
{
  struct uwin *u = owned(id);
  if (!u)
    return -1;
  uintptr_t fl = irq_save();
  if (on)
    u->flags |= UWIN_F_FULLSCREEN;
  else
    u->flags &= ~UWIN_F_FULLSCREEN;
  u->present_seq++;
  irq_restore(fl);
  return 0;
}

int uwin_set_cursor(int id, int cursor)
{
  struct uwin *u = owned(id);
  if (!u || cursor < 0 || cursor > UWIN_CURSOR_LAST)
    return -1;
  u->cursor = cursor;
  return 0;
}

int uwin_set_parent(int id, int parent, int modal)
{
  struct uwin *u = owned(id);
  if (!u || parent == id)
    return -1;
  if (parent < 0) {
    u->parent = -1;
    u->flags &= ~UWIN_F_MODAL;
    touch_full(u);
    return 0;
  }
  /* Parent must be a live window of the same owner. */
  if (parent >= uwins_cap || !uwins[parent])
    return -1;
  struct uwin *p = uwins[parent];
  if (!p->used || p->free_pending || p->owner != u->owner)
    return -1;
  /* Reject cycles: the parent's ancestry must not reach `id`. */
  int a = parent;
  for (int guard = 0; a >= 0 && guard < WIN_FB_SLOTS * 4; guard++) {
    if (a == id)
      return -1;
    struct uwin *c = (a < uwins_cap) ? uwins[a] : 0;
    if (!c || !c->used)
      break;
    a = c->parent;
  }
  u->parent = parent;
  if (modal)
    u->flags |= UWIN_F_MODAL;
  else
    u->flags &= ~UWIN_F_MODAL;
  touch_full(u);
  return 0;
}

uint32_t uwin_fb_vaddr(int id)
{
  struct uwin *u = owned(id);
  return u ? u->user_vaddr : 0;
}

void uwin_task_exiting(uint32_t owner_id)
{
  if (owner_id == compositor_pid)
    compositor_pid = 0;
  for (int i = 0; i < uwins_cap; i++)
    if (uwins[i] && uwins[i]->used && uwins[i]->owner == owner_id)
      uwin_free(uwins[i]);
}

/* Does task `pid` own a live window? Tells windowed apps from CLIs. */
bool uwin_pid_has_window(uint32_t pid)
{
  for (int i = 0; i < uwins_cap; i++)
    if (uwins[i] && uwins[i]->used && uwins[i]->owner == pid)
      return true;
  return false;
}

/* Enumerable: live, mapped, not detached-pending. */
static bool enumerable(struct uwin *u) { return u && u->used && !u->free_pending && u->buf_phys; }

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
    r->id = i;
    r->x = u->fx;
    r->y = u->fy;
    r->w = u->fw;
    r->h = u->fh;
    r->flags = u->flags;
    uintptr_t fl = irq_save();
    r->present_seq = u->present_seq;
    r->dirty_x = u->dx0;
    r->dirty_y = u->dy0;
    r->dirty_w = u->dirty_full ? -1 : u->dx1 - u->dx0;
    r->dirty_h = u->dirty_full ? -1 : u->dy1 - u->dy0;
    u->dirty_full = false;
    u->dx1 = u->dx0;
    u->dy1 = u->dy0;
    irq_restore(fl);
    r->owner_pid = (int)u->owner;
    r->z = 0;
    r->parent = u->parent;
    r->cursor = u->cursor;
    r->buf_w = u->w;
    r->buf_h = u->h;
    int k = 0;
    for (; k < (int)sizeof r->title - 1 && u->title[k]; k++)
      r->title[k] = u->title[k];
    r->title[k] = '\0';
    n++;
  }
  return n;
}

/* Resolve an id to a live uwin regardless of caller; NULL if invalid. */
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
  /* The buffer must fit one map slot. */
  if ((uint32_t)u->npages > WIN_MAP_SLOT_BYTES / 4096u)
    return -1;
  uint32_t *dir = sched_current_dir();
  /* Re-map in place; re-pinning just refreshes. */
  if (paging_map_user_at(dir, at, u->buf_phys, u->npages) < 0)
    return -1;
  u->compositor_pinned = true;
  tlb_flush();
  if (bw)
    *bw = u->w;
  if (bh)
    *bh = u->h;
  if (stride)
    *stride = u->w;
  return 0;
}

int uwin_unmap(int id, uint32_t *dir, uint32_t at)
{
  if (id < 0 || id >= uwins_cap || !uwins[id])
    return -1;
  struct uwin *u = uwins[id];
  if (dir)
    paging_unmap_user_at(dir, at, u->buf_bytes ? u->buf_bytes : WIN_MAP_SLOT_BYTES);
  u->compositor_pinned = false;
  tlb_flush();
  uwin_reap();
  return 0;
}

/* Clear [x0,x1) x [y0,y1) of a client buffer to UWIN_BG via the supervisor scratch slot. */
static void uwin_clear_region(uint32_t phys, int stride, int x0, int y0, int x1, int y1)
{
  for (int y = y0; y < y1; y++) {
    uintptr_t fl = irq_save();
    uint32_t pg = ~0u;
    uint32_t *v = 0;
    for (int x = x0; x < x1; x++) {
      uint32_t idx = (uint32_t)y * (uint32_t)stride + (uint32_t)x;
      if ((idx >> 10) != pg) {
        if (v)
          paging_scratch_unmap(0); /* slot 0 is a non-recursive lock */
        pg = idx >> 10;
        v = paging_scratch_map(0, phys + pg * 4096u);
      }
      v[idx & 1023] = UWIN_BG;
    }
    if (v)
      paging_scratch_unmap(0);
    irq_restore(fl);
  }
}

/* Resize the client area; the allocation only grows. Pushes WEV_RESIZE. */
static void uwin_resize_client(struct uwin *u, int id, int cw, int ch)
{
  if (cw < 16)
    cw = 16;
  if (ch < 16)
    ch = 16;
  /* Cap before multiplying so the uint32 size math can't overflow. */
  if (cw > 4096)
    cw = 4096;
  if (ch > 4096)
    ch = 4096;
  uint32_t maxpix = win_cap_bytes() / 4;
  if ((uint32_t)cw * (uint32_t)ch > maxpix) {
    ch = (int)(maxpix / (uint32_t)cw);
    if (ch < 16)
      ch = 16;
  }
  if (cw == u->w && ch == u->h)
    return;

  if ((uint32_t)cw * (uint32_t)ch * 4u <= u->buf_bytes) {
    u->w = cw;
    u->h = ch;
    /* Clear the new logical buffer rather than copy the old image. */
      uwin_clear_region(u->buf_phys, cw, 0, 0, cw, ch);
      touch_full(u);
      evq_push(u, WEV_RESIZE, cw, ch);
    return;
  }

  int npages = (int)(((uint32_t)cw * ch * 4u + 4095u) / 4096u);
  uint32_t nbytes = (uint32_t)npages * 4096u;
  uint32_t nphys = pmm_alloc_contig(nbytes);
  if (!nphys || paging_kmap(nphys, nbytes) < 0) {
    if (nphys)
      pmm_free_contig(nphys, nbytes);
    return;
  }
  /* The compositor's CR3 lacks the kmap PDEs; use scratch slots with IRQs off. */
  for (uint32_t pg = 0; pg < (uint32_t)npages; pg++) {
    uintptr_t fl = irq_save();
    uint32_t *v = paging_scratch_map(0, nphys + pg * 4096u);
    for (int k = 0; k < 1024; k++)
      v[k] = UWIN_BG;
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
  /* No old-image copy. Remap in place (no unmapped gap that would fault a concurrent paint). */
  if (u->owner_dir)
    paging_map_user_at(u->owner_dir, u->user_vaddr, nphys, npages);
  /* Remap the compositor's pin too. */
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
  touch_full(u);
  tlb_flush();
  evq_push(u, WEV_RESIZE, cw, ch);
}

int uwin_set_geom(int id, int x, int y, int w, int h)
{
  struct uwin *u = broker_win(id);
  if (!u)
    return -1;
  if (w < WM_MIN_W)
    w = WM_MIN_W;
  if (h < WM_MIN_H)
    h = WM_MIN_H;
  u->fx = x;
  u->fy = y;
  u->fw = w;
  u->fh = h;
  if (u->flags & UWIN_F_FULLSCREEN)
    uwin_resize_client(u, id, w, h);
  else
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
