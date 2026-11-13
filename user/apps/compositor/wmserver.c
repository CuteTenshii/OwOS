/* wmserver.c: the compositor end of the window protocol (wmproto.h). It owns every window's state; a window's pixels
 * live in an shm buffer created here, mapped at the window's slot above SHM_HIGH_ADDR and granted to the client's pid.
 * Requests are checked against the pid the kernel reports for the channel, never against anything the client says. */
#include "wmserver.h"
#include "owos.h"
#include "wmproto.h"
#include <stdbool.h>
#include <stdint.h>

#define WMS_CLIENTS 64
#define WMS_OUTQ 128
#define WIN_SIDE_MIN 16
#define WIN_SIDE_MAX 4096
#define UWIN_BG 0x10141a /* matches the frame fill in draw.c */

_Static_assert(WM_LIST_MAX <= SHM_HIGH_SLOTS, "every window id needs a map slot");

struct wclient {
  bool used;
  int fd, pid;
  char in[2 * WM_MSG_MAX];
  int inlen;
  struct wm_msg out[WMS_OUTQ];
  int ohead, otail;
};

struct wwin {
  bool used;
  int client, pid;
  int x, y, w, h; /* frame */
  unsigned flags, seq;
  bool dirty_full;
  int dx0, dy0, dx1, dy1; /* client damage since the last enum */
  char title[WM_TITLE_LEN];
  int parent, cursor;
  int handle, buf_w, buf_h; /* the buffer shown and the size the client draws it at */
  unsigned cap;
  int next_handle; /* larger buffer offered by a resize the client hasn't adopted yet, 0 if none */
  unsigned next_cap;
  int want_w, want_h; /* the client size last asked for */
  bool resize_unsent;
};

static struct wclient clients[WMS_CLIENTS];
static struct wwin wins[WM_LIST_MAX];
static int listen_fd = -1;

static unsigned map_addr(int id) { return SHM_HIGH_ADDR + (unsigned)id * SHM_SLOT_SIZE; }

static unsigned *map_of(int id) { return (unsigned *)(uintptr_t)map_addr(id); }

/* Largest client buffer: the whole framebuffer, within one map slot */
static unsigned buf_limit(void)
{
  unsigned cap = (unsigned)hw_w * (unsigned)hw_h * 4u;
  return cap && cap < SHM_SLOT_SIZE ? cap : SHM_SLOT_SIZE;
}

static unsigned page_round(unsigned bytes) { return (bytes + 4095u) & ~4095u; }

static struct wm_msg msg(int type, int id, int a, int b, int c)
{
  struct wm_msg m = {(uint16_t)type, (uint16_t)sizeof m, id, a, b, c, 0};
  return m;
}

static struct wwin *live(int id)
{
  if (id < 0 || id >= WM_LIST_MAX || !wins[id].used)
    return 0;
  return &wins[id];
}

static struct wwin *owned(int ci, int id)
{
  struct wwin *w = live(id);
  return w && w->pid == clients[ci].pid ? w : 0;
}

static void touch_full(struct wwin *w)
{
  w->seq++;
  w->dirty_full = true;
}

static bool queue(int ci, const struct wm_msg *m)
{
  struct wclient *c = &clients[ci];
  int next = (c->ohead + 1) % WMS_OUTQ;
  if (next == c->otail)
    return false;
  c->out[c->ohead] = *m;
  c->ohead = next;
  return true;
}

static void flush(int ci)
{
  struct wclient *c = &clients[ci];
  while (c->otail != c->ohead) {
    if (write_nb(c->fd, &c->out[c->otail], (int)sizeof c->out[0]) <= 0)
      return; /* the client is behind; its EOF, if that is why, arrives through read */
    c->otail = (c->otail + 1) % WMS_OUTQ;
  }
  for (int id = 0; id < WM_LIST_MAX; id++) {
    struct wwin *w = &wins[id];
    if (!w->used || w->client != ci || !w->resize_unsent)
      continue;
    struct wm_msg m = msg(WM_RESIZE, id, w->next_handle ? w->next_handle : w->handle, w->want_w, w->want_h);
    if (write_nb(c->fd, &m, (int)sizeof m) <= 0)
      return;
    w->resize_unsent = false;
  }
}

static void destroy(int id)
{
  struct wwin *w = &wins[id];
  if (w->handle) {
    shm_unmap(map_addr(id));
    shm_release(w->handle);
  }
  if (w->next_handle)
    shm_release(w->next_handle);
  w->used = false;
  for (int i = 0; i < WM_LIST_MAX; i++) /* a reused id must not inherit someone else's transients */
    if (wins[i].used && wins[i].parent == id) {
      wins[i].parent = -1;
      wins[i].flags &= ~UWIN_F_MODAL;
      touch_full(&wins[i]);
    }
}

static void drop_client(int ci)
{
  for (int id = 0; id < WM_LIST_MAX; id++)
    if (wins[id].used && wins[id].client == ci)
      destroy(id);
  close(clients[ci].fd);
  clients[ci].used = false;
}

/* Cascade a new frame so it opens on-screen */
static void place(struct wwin *w, int id, int cw, int ch)
{
  int win_w = cw + 2 * WM_BORDER, win_h = ch + WM_TITLE_H + WM_BORDER;
  int step = 26, margin = 40;
  int rng_x = screen_w - win_w - margin;
  int rng_y = screen_h - WM_BAR_H - win_h - margin;
  if (rng_x < step)
    rng_x = step;
  if (rng_y < step)
    rng_y = step;
  int wx = margin + (id * step) % rng_x;
  int wy = margin + (id * step) % rng_y;
  if (wx + win_w > screen_w)
    wx = screen_w > win_w ? screen_w - win_w : 0;
  if (wy + win_h > screen_h - WM_BAR_H)
    wy = screen_h - WM_BAR_H > win_h ? screen_h - WM_BAR_H - win_h : 0;
  w->x = wx;
  w->y = wy;
  w->w = win_w;
  w->h = win_h;
}

static void set_title(struct wwin *w, const char *title)
{
  int j = 0;
  for (; j < WM_TITLE_LEN - 1 && title[j]; j++)
    w->title[j] = title[j];
  w->title[j] = '\0';
}

static void on_create(int ci, int cw, int ch, const char *title)
{
  struct wm_msg reply = msg(WM_CREATED, -1, 0, 0, 0);
  int id = -1;
  for (int i = 0; i < WM_LIST_MAX && id < 0; i++)
    if (!wins[i].used)
      id = i;
  bool sized = cw >= WIN_SIDE_MIN && ch >= WIN_SIDE_MIN && cw <= WIN_SIDE_MAX && ch <= WIN_SIDE_MAX;
  unsigned bytes = sized ? (unsigned)cw * (unsigned)ch * 4u : 0;
  if (id >= 0 && sized && bytes <= buf_limit()) {
    int h = shm_create(bytes);
    if (h > 0 && shm_grant(h, clients[ci].pid) == 0 && shm_map(h, map_addr(id)) == 0) {
      unsigned *px = map_of(id);
      for (unsigned i = 0; i < bytes / 4u; i++)
        px[i] = UWIN_BG;
      struct wwin *w = &wins[id];
      *w = (struct wwin){0};
      w->used = true;
      w->client = ci;
      w->pid = clients[ci].pid;
      place(w, id, cw, ch);
      w->flags = UWIN_F_VISIBLE | UWIN_F_RESIZABLE;
      set_title(w, title[0] ? title : "app");
      w->parent = -1;
      w->handle = h;
      w->cap = page_round(bytes);
      w->buf_w = w->want_w = cw;
      w->buf_h = w->want_h = ch;
      w->seq = 1;
      w->dirty_full = true;
      reply = msg(WM_CREATED, id, h, cw, ch);
    } else if (h > 0) {
      shm_release(h);
    }
  }
  queue(ci, &reply);
}

static void on_present(struct wwin *w, int x, int y, int pw, int ph)
{
  w->seq++;
  if (pw <= 0 || ph <= 0) {
    w->dirty_full = true;
    return;
  }
  long long x1 = (long long)x + pw, y1 = (long long)y + ph;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int xe = x1 > w->buf_w ? w->buf_w : (int)x1;
  int ye = y1 > w->buf_h ? w->buf_h : (int)y1;
  if (xe <= x0 || ye <= y0)
    return;
  if (w->dx1 <= w->dx0 || w->dy1 <= w->dy0) {
    w->dx0 = x0;
    w->dy0 = y0;
    w->dx1 = xe;
    w->dy1 = ye;
    return;
  }
  if (x0 < w->dx0)
    w->dx0 = x0;
  if (y0 < w->dy0)
    w->dy0 = y0;
  if (xe > w->dx1)
    w->dx1 = xe;
  if (ye > w->dy1)
    w->dy1 = ye;
}

/* The client now draws into `handle` at bw x bh: show that buffer from here on */
static void on_ack(struct wwin *w, int id, int handle, int bw, int bh)
{
  if (bw < WIN_SIDE_MIN || bh < WIN_SIDE_MIN || bw > WIN_SIDE_MAX || bh > WIN_SIDE_MAX)
    return;
  unsigned need = (unsigned)bw * (unsigned)bh * 4u;
  if (w->next_handle && handle == w->next_handle) {
    if (need > w->next_cap || shm_map(handle, map_addr(id)) != 0)
      return;
    shm_release(w->handle); /* the map above replaced our view of it; the client has already moved off it */
    w->handle = handle;
    w->cap = w->next_cap;
    w->next_handle = 0;
  } else if (handle != w->handle || need > w->cap) {
    return; /* an offer since superseded: a later WM_RESIZE is on its way */
  }
  w->buf_w = bw;
  w->buf_h = bh;
  touch_full(w);
}

/* True when following parents up from `from` reaches `target`; a chain longer than the table counts as a cycle */
static bool reaches(int from, int target)
{
  for (int a = from, guard = 0; a >= 0; guard++) {
    if (a == target || guard >= WM_LIST_MAX)
      return true;
    struct wwin *c = live(a);
    if (!c)
      return false;
    a = c->parent;
  }
  return false;
}

static void on_parent(struct wwin *w, int id, int parent, int modal)
{
  if (parent < 0) {
    w->parent = -1;
    w->flags &= ~UWIN_F_MODAL;
    touch_full(w);
    return;
  }
  struct wwin *p = live(parent);
  if (!p || parent == id || p->pid != w->pid || reaches(parent, id))
    return;
  w->parent = parent;
  if (modal)
    w->flags |= UWIN_F_MODAL;
  else
    w->flags &= ~UWIN_F_MODAL;
  touch_full(w);
}

static void set_flag(struct wwin *w, unsigned flag, int on)
{
  if (on)
    w->flags |= flag;
  else
    w->flags &= ~flag;
}

/* One complete message from client `ci`; false on a protocol violation */
static bool handle(int ci, const struct wm_msg *m, const char *title)
{
  if (m->type == WM_CREATE) {
    on_create(ci, m->a, m->b, title);
    return true;
  }
  if (m->type == WM_OWNS) {
    int owns = 0;
    for (int i = 0; i < WM_LIST_MAX && !owns; i++)
      owns = wins[i].used && wins[i].pid == m->a;
    struct wm_msg r = msg(WM_OWNS_REPLY, -1, m->a, owns, 0);
    queue(ci, &r);
    return true;
  }
  struct wwin *w = owned(ci, m->id);
  if (!w)
    return m->type >= WM_PRESENT && m->type <= WM_ACK; /* a request for a window already gone is harmless */
  switch (m->type) {
  case WM_PRESENT:
    on_present(w, m->a, m->b, m->c, m->d);
    break;
  case WM_CLOSE:
    destroy(m->id);
    break;
  case WM_TITLE:
    set_title(w, title);
    touch_full(w);
    break;
  case WM_RESIZABLE:
    set_flag(w, UWIN_F_RESIZABLE, m->a);
    break;
  case WM_FULLSCREEN:
    set_flag(w, UWIN_F_FULLSCREEN, m->a);
    w->seq++;
    break;
  case WM_CURSOR:
    if (m->a >= 0 && m->a <= WIN_CURSOR_PROGRESS)
      w->cursor = m->a;
    break;
  case WM_PARENT:
    on_parent(w, m->id, m->a, m->b);
    break;
  case WM_ACK:
    on_ack(w, m->id, m->a, m->b, m->c);
    break;
  default:
    return false;
  }
  return true;
}

static int msg_len(int type) { return type == WM_CREATE || type == WM_TITLE ? WM_MSG_MAX : (int)sizeof(struct wm_msg); }

static bool parse(int ci)
{
  struct wclient *c = &clients[ci];
  while (c->inlen >= (int)sizeof(struct wm_msg)) {
    struct wm_msg m;
    memcpy(&m, c->in, sizeof m);
    if (m.len != msg_len(m.type))
      return false;
    if (c->inlen < m.len)
      break;
    char title[WM_TITLE_LEN] = "";
    if (m.len == WM_MSG_MAX) {
      memcpy(title, c->in + sizeof m, WM_TITLE_LEN);
      title[WM_TITLE_LEN - 1] = '\0';
    }
    c->inlen -= m.len;
    memmove(c->in, c->in + m.len, (unsigned)c->inlen);
    if (!handle(ci, &m, title))
      return false;
  }
  return true;
}

static void read_client(int ci)
{
  struct wclient *c = &clients[ci];
  for (;;) {
    int n = read_nb(c->fd, c->in + c->inlen, (int)sizeof c->in - c->inlen);
    if (n == 0)
      return;
    if (n < 0) {
      drop_client(ci);
      return;
    }
    c->inlen += n;
    if (!parse(ci)) {
      drop_client(ci);
      return;
    }
  }
}

static void accept_clients(void)
{
  if (listen_fd < 0 && (listen_fd = port_open(WM_PORT, 1)) < 0)
    return;
  for (;;) {
    int fd = port_accept_nb(listen_fd);
    if (fd < 0)
      return;
    int ci = -1;
    for (int i = 0; i < WMS_CLIENTS && ci < 0; i++)
      if (!clients[i].used)
        ci = i;
    struct port_peer peer;
    if (ci < 0 || port_peer(fd, &peer) != 0) {
      close(fd);
      continue;
    }
    struct wclient *c = &clients[ci];
    c->used = true;
    c->fd = fd;
    c->pid = peer.pid;
    c->inlen = 0;
    c->ohead = c->otail = 0;
  }
}

void wms_poll(void)
{
  accept_clients();
  for (int ci = 0; ci < WMS_CLIENTS; ci++)
    if (clients[ci].used)
      read_client(ci);
  wms_flush();
}

void wms_flush(void)
{
  for (int ci = 0; ci < WMS_CLIENTS; ci++)
    if (clients[ci].used)
      flush(ci);
}

int win_enum(struct winrec *out, int max)
{
  int n = 0;
  for (int id = 0; id < WM_LIST_MAX && n < max; id++) {
    struct wwin *w = &wins[id];
    if (!w->used || !w->handle)
      continue;
    struct winrec *r = &out[n++];
    r->id = id;
    r->x = w->x;
    r->y = w->y;
    r->w = w->w;
    r->h = w->h;
    r->flags = w->flags;
    r->present_seq = w->seq;
    r->dirty_x = w->dx0;
    r->dirty_y = w->dy0;
    r->dirty_w = w->dirty_full ? -1 : w->dx1 - w->dx0;
    r->dirty_h = w->dirty_full ? -1 : w->dy1 - w->dy0;
    w->dirty_full = false;
    w->dx1 = w->dx0;
    w->dy1 = w->dy0;
    r->owner_pid = w->pid;
    r->z = 0;
    r->buf_w = w->buf_w;
    r->buf_h = w->buf_h;
    memcpy(r->title, w->title, sizeof r->title);
    r->parent = w->parent;
    r->cursor = w->cursor;
  }
  return n;
}

int win_map(int id, struct winmap *m)
{
  struct wwin *w = live(id);
  if (!w || !w->handle)
    return -1;
  m->vaddr = map_addr(id);
  m->w = w->buf_w;
  m->h = w->buf_h;
  m->stride = w->buf_w;
  return 0;
}

/* Ask the client to draw at cw x ch, offering a bigger buffer when the current one can't hold it */
static void resize_client(struct wwin *w, int cw, int ch)
{
  if (cw < WIN_SIDE_MIN)
    cw = WIN_SIDE_MIN;
  if (ch < WIN_SIDE_MIN)
    ch = WIN_SIDE_MIN;
  if (cw > WIN_SIDE_MAX)
    cw = WIN_SIDE_MAX;
  if (ch > WIN_SIDE_MAX)
    ch = WIN_SIDE_MAX;
  unsigned maxpix = buf_limit() / 4u;
  if ((unsigned)cw * (unsigned)ch > maxpix) {
    ch = (int)(maxpix / (unsigned)cw);
    if (ch < WIN_SIDE_MIN)
      ch = WIN_SIDE_MIN;
  }
  if (cw == w->want_w && ch == w->want_h)
    return;
  unsigned need = (unsigned)cw * (unsigned)ch * 4u;
  if (need <= w->cap) {
    if (w->next_handle) {
      shm_release(w->next_handle);
      w->next_handle = 0;
    }
  } else if (!w->next_handle || need > w->next_cap) {
    unsigned bytes = need + need / 4u; /* headroom, so a drag-resize doesn't reallocate every frame */
    if (bytes > buf_limit())
      bytes = buf_limit() > need ? buf_limit() : need;
    int h = shm_create(bytes);
    if (h <= 0)
      return;
    if (shm_grant(h, w->pid) != 0) {
      shm_release(h);
      return;
    }
    if (w->next_handle)
      shm_release(w->next_handle);
    w->next_handle = h;
    w->next_cap = page_round(bytes);
  }
  w->want_w = cw;
  w->want_h = ch;
  w->resize_unsent = true;
}

int win_set_geom(int id, int x, int y, int w, int h)
{
  struct wwin *wn = live(id);
  if (!wn)
    return -1;
  if (w < WM_MIN_W)
    w = WM_MIN_W;
  if (h < WM_MIN_H)
    h = WM_MIN_H;
  wn->x = x;
  wn->y = y;
  wn->w = w;
  wn->h = h;
  if (wn->flags & UWIN_F_FULLSCREEN)
    resize_client(wn, w, h);
  else
    resize_client(wn, w - 2 * WM_BORDER, h - WM_TITLE_H - WM_BORDER);
  return 0;
}

int win_deliver(int id, const struct winevent *ev)
{
  struct wwin *w = live(id);
  if (!w)
    return -1;
  struct wm_msg m = msg(WM_EVENT, id, (int)ev->type, ev->a, ev->b);
  return queue(w->client, &m) ? 0 : -1;
}

int win_key_space(int id)
{
  struct wwin *w = live(id);
  if (!w)
    return 1 << 30;
  struct wclient *c = &clients[w->client];
  int used = (c->ohead - c->otail + WMS_OUTQ) % WMS_OUTQ;
  return WMS_OUTQ - 1 - used;
}
