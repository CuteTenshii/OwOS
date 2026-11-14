/* win.c: the app end of the window protocol (wmproto.h). File descriptors are per thread, so each thread talks to the
 * compositor over its own connection; the window table and the queued events are shared under one lock, and a
 * window's events arrive on the connection of the thread that created it. */
#include "mutex.h"
#include "owos.h"
#include "wmproto.h"
#include <stdbool.h>
#include <stdint.h>

#define EVQ 64

struct queued {
  struct win_event ev;
  int handle; /* WEV_RESIZE: the buffer to draw into from then on */
};

struct lwin {
  bool used;
  int id, conn;
  int handle, w, h; /* the buffer mapped at this window's slot, and the size the app draws at */
  bool ack_due;     /* the compositor learns of an adopted resize just before the next present */
  int parent;
  int head, tail;
  struct queued q[EVQ];
};

/* Indexed by shm slot, so the lowest free one (the first window: WIN_FB_ADDR) is reused first */
static struct lwin wins[SHM_LOW_SLOTS];
static mutex_t lock = MUTEX_INIT;
static int next_conn = 1;

static __thread int conn_fd = -1;
static __thread int conn_id;
static __thread char rbuf[2 * WM_MSG_MAX];
static __thread int rlen;

static sys_word slot_addr(const struct lwin *w) { return SHM_LOW_ADDR + (sys_word)(w - wins) * SHM_SLOT_SIZE; }

static struct lwin *find(int id)
{
  if (id < 0)
    return 0;
  for (int i = 0; i < SHM_LOW_SLOTS; i++)
    if (wins[i].used && wins[i].id == id)
      return &wins[i];
  return 0;
}

static void push(struct lwin *w, unsigned type, int a, int b, int handle)
{
  if (type == WEV_MOVE && w->head != w->tail) { /* only the latest hover position matters */
    struct queued *last = &w->q[(w->head + EVQ - 1) % EVQ];
    if (last->ev.type == WEV_MOVE) {
      last->ev.a = a;
      last->ev.b = b;
      return;
    }
  }
  int next = (w->head + 1) % EVQ;
  if (next == w->tail) {
    if (type != WEV_RESIZE)
      return;                      /* full: drop, the app isn't polling */
    w->tail = (w->tail + 1) % EVQ; /* a lost resize would leave the app drawing at a stale size */
  }
  w->q[w->head] = (struct queued){{type, a, b}, handle};
  w->head = next;
}

static void disconnect(void)
{
  for (int i = 0; i < SHM_LOW_SLOTS; i++)
    if (wins[i].used && wins[i].conn == conn_id)
      push(&wins[i], WEV_CLOSE, 0, 0, 0);
  close(conn_fd);
  conn_fd = -1;
  rlen = 0;
}

static bool wm_connect(void)
{
  if (conn_fd >= 0)
    return true;
  int fd = port_open(WM_PORT, 0);
  if (fd < 0)
    return false;
  struct port_peer peer;
  if (port_peer(fd, &peer) != 0 || peer.uid != 0) { /* keystrokes and pixels only go to a root compositor */
    close(fd);
    return false;
  }
  conn_fd = fd;
  conn_id = next_conn++;
  rlen = 0;
  return true;
}

static bool send_msg(const void *m, int len)
{
  if (!wm_connect())
    return false;
  if (write(conn_fd, m, len) != len) {
    disconnect();
    return false;
  }
  return true;
}

static bool send_req(int type, int id, int a, int b, int c, int d)
{
  struct wm_msg m = {(uint16_t)type, (uint16_t)sizeof m, id, a, b, c, d};
  return send_msg(&m, (int)sizeof m);
}

static bool send_title(int type, int id, int a, int b, const char *title)
{
  struct wm_msg_title t;
  memset(&t, 0, sizeof t);
  t.m = (struct wm_msg){(uint16_t)type, (uint16_t)sizeof t, id, a, b, 0, 0};
  for (int i = 0; title && title[i] && i < WM_TITLE_LEN - 1; i++)
    t.title[i] = title[i];
  return send_msg(&t, (int)sizeof t);
}

static void dispatch(const struct wm_msg *m)
{
  struct lwin *w = find(m->id);
  if (!w || w->conn != conn_id)
    return;
  if (m->type == WM_EVENT)
    push(w, (unsigned)m->a, m->b, m->c, 0);
  else if (m->type == WM_RESIZE)
    push(w, WEV_RESIZE, m->b, m->c, m->a);
}

/* Consume the complete messages in rbuf; stops at, and copies out, the first one of type `want` */
static bool take(int want, struct wm_msg *reply)
{
  while (rlen >= (int)sizeof(struct wm_msg)) {
    struct wm_msg m;
    memcpy(&m, rbuf, sizeof m);
    if (m.len != sizeof m) {
      disconnect();
      return false;
    }
    rlen -= m.len;
    memmove(rbuf, rbuf + m.len, (unsigned)rlen);
    if (want && m.type == want) {
      *reply = m;
      return true;
    }
    dispatch(&m);
  }
  return false;
}

static void pump(void)
{
  while (conn_fd >= 0) {
    int n = read_nb(conn_fd, rbuf + rlen, (int)sizeof rbuf - rlen);
    if (n == 0)
      return;
    if (n < 0) {
      disconnect();
      return;
    }
    rlen += n;
    take(0, 0);
  }
}

/* Block for the compositor's answer to the request just sent, queueing events that arrive first */
static bool wait_reply(int want, struct wm_msg *reply)
{
  while (conn_fd >= 0) {
    if (take(want, reply))
      return true;
    if (conn_fd < 0)
      return false;
    int n = read(conn_fd, rbuf + rlen, (int)sizeof rbuf - rlen);
    if (n == 0)
      disconnect();
    if (n <= 0)
      return false;
    rlen += n;
  }
  return false;
}

int win_create(int w, int h, const char *title)
{
  if (w < 16 || h < 16 || w > 4096 || h > 4096)
    return -1;
  mutex_lock(&lock);
  int id = -1;
  struct lwin *lw = 0;
  for (int i = 0; i < SHM_LOW_SLOTS && !lw; i++)
    if (!wins[i].used)
      lw = &wins[i];
  struct wm_msg r;
  if (lw && send_title(WM_CREATE, -1, w, h, title ? title : "app") && wait_reply(WM_CREATED, &r) && r.id >= 0) {
    if (shm_map(r.a, slot_addr(lw)) == 0) {
      *lw = (struct lwin){0};
      lw->used = true;
      lw->id = r.id;
      lw->conn = conn_id;
      lw->handle = r.a;
      lw->w = r.b;
      lw->h = r.c;
      lw->parent = -1;
      id = r.id;
    } else {
      send_req(WM_CLOSE, r.id, 0, 0, 0, 0);
    }
  }
  mutex_unlock(&lock);
  return id;
}

static int present(int id, int x, int y, int w, int h)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  int r = -1;
  if (lw && (!lw->ack_due || send_req(WM_ACK, id, lw->handle, lw->w, lw->h, 0))) {
    lw->ack_due = false;
    r = send_req(WM_PRESENT, id, x, y, w, h) ? 0 : -1;
  }
  mutex_unlock(&lock);
  return r;
}

int win_present(int id) { return present(id, 0, 0, 0, 0); }

int win_present_rect(int id, int x, int y, int w, int h)
{
  if (x < 0 || y < 0 || w <= 0 || h <= 0)
    return win_present(id);
  return present(id, x, y, w, h);
}

/* Switch to the buffer a resize offered; false (event dropped) if it can't be mapped, so the app keeps its old size */
static bool adopt(struct lwin *lw, const struct queued *q)
{
  if (q->handle != lw->handle) {
    if (shm_map(q->handle, slot_addr(lw)) != 0)
      return false;
    lw->handle = q->handle;
  }
  lw->w = q->ev.a;
  lw->h = q->ev.b;
  lw->ack_due = true;
  return true;
}

int win_poll(int id, struct win_event *ev)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  int r = -1;
  if (lw) {
    pump();
    r = 0;
    while (ev && lw->tail != lw->head) {
      struct queued q = lw->q[lw->tail];
      lw->tail = (lw->tail + 1) % EVQ;
      if (q.ev.type == WEV_RESIZE && !adopt(lw, &q))
        continue;
      *ev = q.ev;
      r = 1;
      break;
    }
  }
  mutex_unlock(&lock);
  return r;
}

void win_wait(int id)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  bool ready = false;
  if (lw) {
    pump();
    ready = lw->head != lw->tail;
  }
  mutex_unlock(&lock);
  if (!ready)
    msleep(15);
}

int win_close(int id)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  int r = -1;
  if (lw) {
    send_req(WM_CLOSE, id, 0, 0, 0, 0);
    shm_unmap(slot_addr(lw));
    lw->used = false;
    for (int i = 0; i < SHM_LOW_SLOTS; i++)
      if (wins[i].used && wins[i].parent == id)
        wins[i].parent = -1;
    r = 0;
  }
  mutex_unlock(&lock);
  return r;
}

int win_owned(int pid)
{
  mutex_lock(&lock);
  bool had = conn_fd >= 0;
  int owns = 0;
  struct wm_msg r;
  if (send_req(WM_OWNS, -1, pid, 0, 0, 0) && wait_reply(WM_OWNS_REPLY, &r))
    owns = r.b != 0;
  if (!had && conn_fd >= 0) { /* a one-off query: don't keep a connection with no windows on it */
    close(conn_fd);
    conn_fd = -1;
    rlen = 0;
  }
  mutex_unlock(&lock);
  return owns;
}

/* True when following parents up from `from` reaches `target` */
static bool reaches(int from, int target)
{
  for (int a = from, guard = 0; a >= 0; guard++) {
    if (a == target || guard >= SHM_LOW_SLOTS)
      return true;
    struct lwin *c = find(a);
    if (!c)
      return false;
    a = c->parent;
  }
  return false;
}

int win_set_parent(int id, int parent, int modal)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  int r = -1;
  if (lw && parent != id && (parent < 0 || (find(parent) && !reaches(parent, id))) &&
      send_req(WM_PARENT, id, parent < 0 ? -1 : parent, modal ? 1 : 0, 0, 0)) {
    lw->parent = parent < 0 ? -1 : parent;
    r = 0;
  }
  mutex_unlock(&lock);
  return r;
}

unsigned *win_fb_of(int id)
{
  mutex_lock(&lock);
  struct lwin *lw = find(id);
  unsigned *fb = lw ? (unsigned *)(uintptr_t)slot_addr(lw) : 0;
  mutex_unlock(&lock);
  return fb;
}

static int set_attr(int type, int id, int value)
{
  mutex_lock(&lock);
  int r = find(id) && send_req(type, id, value, 0, 0, 0) ? 0 : -1;
  mutex_unlock(&lock);
  return r;
}

int win_settitle(int id, const char *title)
{
  mutex_lock(&lock);
  int r = find(id) && send_title(WM_TITLE, id, 0, 0, title ? title : "") ? 0 : -1;
  mutex_unlock(&lock);
  return r;
}

int win_resizable(int id, int on) { return set_attr(WM_RESIZABLE, id, on ? 1 : 0); }

int win_fullscreen(int id, int on) { return set_attr(WM_FULLSCREEN, id, on ? 1 : 0); }

int win_cursor(int id, int shape)
{
  if (shape < 0 || shape > WIN_CURSOR_PROGRESS)
    return -1;
  return set_attr(WM_CURSOR, id, shape);
}
