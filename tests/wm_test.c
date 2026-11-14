/* Host test for the window protocol: the app library (user/lib/win/win.c) talking to the compositor's window server
 * (user/apps/compositor/wmserver.c) over a fake kernel. Channels are in-memory queues; shm objects are memfds mapped
 * at the real slot addresses, so the client and the server really share pixels. One process plays both sides, and
 * `cur_pid` says whose syscall is running. Skipped where memfd is missing. */
#define OWOS_SUITE_TEST
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
static int host_memfd(size_t bytes)
{
  int fd = memfd_create("wm-test", 0);
  if (fd >= 0 && ftruncate(fd, (off_t)bytes) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
#else
static int host_memfd(size_t bytes)
{
  (void)bytes;
  return -1;
}
#define MAP_FIXED_NOREPLACE MAP_FIXED /* never reached: no object is ever created */
#endif
static void host_close(int fd) { close(fd); }

#include "test.h"
#include "wm_host.h"
#include "wmproto.h"
#include "wmserver.h"

int screen_w = 1280, screen_h = 720, hw_w = 1280, hw_h = 720;

#define SERVER_PID 1
#define APP_PID 42
#define OTHER_PID 77
#define BG 0x10141a

static int cur_pid = APP_PID;
static int server_uid;

static void serve(void)
{
  int saved = cur_pid;
  cur_pid = SERVER_PID;
  wms_poll();
  cur_pid = saved;
}

void wmt_msleep(unsigned ms) { (void)ms; }

/* channels */

#define CHAN_MAX 8
#define QCAP 1024 /* the kernel pipe size */
#define LISTEN_FD 500
#define CLIENT_FD(i) (1000 + (i))
#define SERVER_FD(i) (2000 + (i))

struct queue {
  unsigned char b[QCAP];
  int n;
};

struct chan {
  bool used, accepted, client_open, server_open;
  bool hangup; /* the app sees the compositor gone, while the server side still holds its end */
  int client_pid, client_uid;
  struct queue c2s, s2c;
};

static struct chan chans[CHAN_MAX];
static bool listening;

static struct chan *chan_of(int fd, bool *server_end)
{
  int i = fd >= SERVER_FD(0) ? fd - SERVER_FD(0) : fd - CLIENT_FD(0);
  if (i < 0 || i >= CHAN_MAX || !chans[i].used)
    return 0;
  *server_end = fd >= SERVER_FD(0);
  return &chans[i];
}

int wmt_port_open(const char *name, int as_server)
{
  if (strcmp(name, WM_PORT) != 0)
    return -1;
  if (as_server) {
    listening = true;
    return LISTEN_FD;
  }
  if (!listening)
    return -1;
  for (int i = 0; i < CHAN_MAX; i++)
    if (!chans[i].used) {
      memset(&chans[i], 0, sizeof chans[i]);
      chans[i].used = chans[i].client_open = chans[i].server_open = true;
      chans[i].client_pid = cur_pid;
      chans[i].client_uid = 0;
      return CLIENT_FD(i);
    }
  return -1;
}

int wmt_port_accept_nb(int listen_fd)
{
  if (listen_fd != LISTEN_FD)
    return -1;
  for (int i = 0; i < CHAN_MAX; i++)
    if (chans[i].used && !chans[i].accepted) {
      chans[i].accepted = true;
      return SERVER_FD(i);
    }
  return -2;
}

int wmt_port_peer(int fd, struct port_peer *out)
{
  bool server_end;
  struct chan *c = chan_of(fd, &server_end);
  if (!c)
    return -1;
  if (server_end)
    *out = (struct port_peer){c->client_pid, c->client_uid, 0};
  else
    *out = (struct port_peer){SERVER_PID, server_uid, 0};
  return 0;
}

int wmt_read_nb(int fd, void *buf, int len)
{
  bool server_end;
  struct chan *c = chan_of(fd, &server_end);
  if (!c || !(server_end ? c->server_open : c->client_open))
    return -1;
  struct queue *q = server_end ? &c->c2s : &c->s2c;
  if (!q->n)
    return (server_end ? c->client_open : c->server_open && !c->hangup) ? 0 : -1;
  int n = len < q->n ? len : q->n;
  memcpy(buf, q->b, (size_t)n);
  memmove(q->b, q->b + n, (size_t)(q->n - n));
  q->n -= n;
  return n;
}

/* Blocking read: the compositor gets to run while the app waits, as it would on another CPU */
int wmt_read(int fd, void *buf, int len)
{
  for (int tries = 0; tries < 8; tries++) {
    int n = wmt_read_nb(fd, buf, len);
    if (n != 0)
      return n < 0 ? 0 : n;
    serve();
  }
  return -1; /* it would block forever */
}

static int put(int fd, const void *buf, int len, bool all_or_nothing)
{
  bool server_end;
  struct chan *c = chan_of(fd, &server_end);
  if (!c || !(server_end ? c->client_open : c->server_open && !c->hangup))
    return -1;
  struct queue *q = server_end ? &c->s2c : &c->c2s;
  if (QCAP - q->n < len) {
    if (all_or_nothing)
      return 0;
    if (!server_end)
      serve(); /* a full pipe blocks the app until the compositor drains it */
    if (QCAP - q->n < len)
      return -1;
  }
  memcpy(q->b + q->n, buf, (size_t)len);
  q->n += len;
  return len;
}

int wmt_write(int fd, const void *buf, int len) { return put(fd, buf, len, false); }

int wmt_write_nb(int fd, const void *buf, int len) { return put(fd, buf, len, true); }

int wmt_close(int fd)
{
  if (fd == LISTEN_FD) {
    listening = false;
    return 0;
  }
  bool server_end;
  struct chan *c = chan_of(fd, &server_end);
  if (!c)
    return -1;
  if (server_end)
    c->server_open = false;
  else
    c->client_open = false;
  if (!c->client_open && !c->server_open)
    c->used = false;
  return 0;
}

/* shared memory */

#define OBJ_MAX 16
#define MAP_MAX 16

struct fobj {
  bool used, owner_ref;
  int owner, fd, nmaps, ngrants;
  int grants[4];
  size_t bytes;
};

struct fmap {
  bool used;
  int obj;
  unsigned at;
};

static struct fobj objs[OBJ_MAX];
static struct fmap fmaps[MAP_MAX];
static int creates;

static struct fobj *obj_of(int h) { return h > 0 && h <= OBJ_MAX && objs[h - 1].used ? &objs[h - 1] : 0; }

static void obj_maybe_free(struct fobj *o)
{
  if (o->owner_ref || o->nmaps > 0)
    return;
  host_close(o->fd);
  o->used = false;
}

static int live_objects(void)
{
  int n = 0;
  for (int i = 0; i < OBJ_MAX; i++)
    n += objs[i].used;
  return n;
}

int wmt_shm_create(unsigned bytes)
{
  for (int i = 0; i < OBJ_MAX; i++)
    if (!objs[i].used) {
      size_t run = (bytes + 4095u) & ~(size_t)4095;
      int fd = host_memfd(run);
      if (fd < 0)
        return -1;
      objs[i] = (struct fobj){true, true, cur_pid, fd, 0, 0, {0}, run};
      creates++;
      return i + 1;
    }
  return -1;
}

int wmt_shm_grant(int h, int pid)
{
  struct fobj *o = obj_of(h);
  if (!o || !o->owner_ref || o->owner != cur_pid || o->ngrants == 4)
    return -1;
  o->grants[o->ngrants++] = pid;
  return 0;
}

static void unmap_slot(struct fmap *m)
{
  munmap((void *)(uintptr_t)m->at, objs[m->obj].bytes);
  m->used = false;
  objs[m->obj].nmaps--;
  obj_maybe_free(&objs[m->obj]);
}

static struct fmap *map_at(unsigned at)
{
  for (int i = 0; i < MAP_MAX; i++)
    if (fmaps[i].used && fmaps[i].at == at)
      return &fmaps[i];
  return 0;
}

int wmt_shm_map(int h, sys_word at)
{
  struct fobj *o = obj_of(h);
  if (!o || at % SHM_SLOT_SIZE)
    return -1;
  bool allowed = o->owner == cur_pid && o->owner_ref;
  for (int g = 0; g < o->ngrants; g++)
    allowed |= o->grants[g] == cur_pid;
  if (!allowed)
    return -1;
  struct fmap *old = map_at(at);
  if (old && old->obj == h - 1)
    return 0;
  if (old)
    unmap_slot(old);
  struct fmap *m = 0;
  for (int i = 0; i < MAP_MAX && !m; i++)
    if (!fmaps[i].used)
      m = &fmaps[i];
  void *want = (void *)(uintptr_t)at;
  if (!m || mmap(want, o->bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, o->fd, 0) != want)
    return -1;
  *m = (struct fmap){true, h - 1, at};
  o->nmaps++;
  return 0;
}

int wmt_shm_unmap(sys_word at)
{
  struct fmap *m = map_at(at);
  if (!m)
    return -1;
  unmap_slot(m);
  return 0;
}

int wmt_shm_release(int h)
{
  struct fobj *o = obj_of(h);
  if (!o || !o->owner_ref || o->owner != cur_pid)
    return -1;
  o->owner_ref = false;
  o->ngrants = 0;
  obj_maybe_free(o);
  return 0;
}

/* helpers */

static int enum_all(struct winrec *out) { return win_enum(out, WM_LIST_MAX); }

/* The compositor resizing a frame, then sending what that produced */
static void set_geom(int id, int cw, int ch)
{
  int saved = cur_pid;
  cur_pid = SERVER_PID;
  win_set_geom(id, 10, 10, cw + 2 * WM_BORDER, ch + WM_TITLE_H + WM_BORDER);
  wms_flush();
  cur_pid = saved;
}

static struct winrec *rec_of(struct winrec *recs, int n, int id)
{
  for (int i = 0; i < n; i++)
    if (recs[i].id == id)
      return &recs[i];
  return 0;
}

/* A connection that speaks the raw protocol as some other process */
static int raw_open(int pid)
{
  int saved = cur_pid;
  cur_pid = pid;
  int fd = wmt_port_open(WM_PORT, 0);
  cur_pid = saved;
  return fd;
}

static void raw_send(int fd, int type, int id, int a, int b)
{
  struct wm_msg m = {(uint16_t)type, (uint16_t)sizeof m, id, a, b, 0, 0};
  wmt_write(fd, &m, (int)sizeof m);
}

static bool raw_recv(int fd, struct wm_msg *m) { return wmt_read_nb(fd, m, (int)sizeof *m) == (int)sizeof *m; }

/* cases */

static void check_root_only(void)
{
  server_uid = 1000;
  serve();
  expect_eq(win_create(100, 100, "x"), -1);
  server_uid = 0;
}

static void check_create_share_close(void)
{
  int id = win_create(200, 100, "Hello");
  expect(id >= 0);
  unsigned *fb = win_fb_of(id);
  expect(fb == (unsigned *)(uintptr_t)SHM_LOW_ADDR);
  if (!fb)
    return;
  expect_eq(fb[0], BG);

  fb[5] = 0xABCDEF;
  struct winmap m;
  expect_eq(win_map(id, &m), 0);
  expect_eq(((unsigned *)(uintptr_t)m.vaddr)[5], 0xABCDEF);

  struct winrec recs[WM_LIST_MAX];
  int n = enum_all(recs);
  expect_eq(n, 1);
  expect_string(recs[0].title, "Hello");
  expect_eq(recs[0].owner_pid, APP_PID);
  expect_eq(recs[0].buf_w, 200);
  expect_eq(recs[0].buf_h, 100);
  expect_eq(recs[0].w, 200 + 2 * WM_BORDER);
  expect_eq(recs[0].h, 100 + WM_TITLE_H + WM_BORDER);
  expect_eq(recs[0].parent, -1);
  expect(win_owned(APP_PID));
  expect(!win_owned(APP_PID + 1));

  int second = win_create(64, 64, 0);
  expect(win_fb_of(second) == (unsigned *)(uintptr_t)(SHM_LOW_ADDR + SHM_SLOT_SIZE));
  win_settitle(id, "Renamed");
  serve();
  n = enum_all(recs);
  expect_eq(n, 2);
  struct winrec *r = rec_of(recs, n, id);
  expect(r && strcmp(r->title, "Renamed") == 0);
  r = rec_of(recs, n, second);
  expect(r && strcmp(r->title, "app") == 0);

  expect_eq(win_close(second), 0);
  expect_eq(win_close(id), 0);
  serve();
  expect_eq(enum_all(recs), 0);
  expect_eq(live_objects(), 0);
  expect(!win_owned(APP_PID));
}

static void check_damage(void)
{
  int id = win_create(200, 100, "d");
  struct winrec recs[WM_LIST_MAX];
  serve();
  enum_all(recs);
  unsigned seq = recs[0].present_seq;

  win_present_rect(id, 10, 20, 30, 40);
  serve();
  enum_all(recs);
  expect_eq(recs[0].dirty_x, 10);
  expect_eq(recs[0].dirty_y, 20);
  expect_eq(recs[0].dirty_w, 30);
  expect_eq(recs[0].dirty_h, 40);
  expect(recs[0].present_seq != seq);
  enum_all(recs);
  expect_eq(recs[0].dirty_w, 0);

  win_present_rect(id, 190, 90, 50, 50); /* clipped to the buffer */
  serve();
  enum_all(recs);
  expect_eq(recs[0].dirty_w, 10);
  expect_eq(recs[0].dirty_h, 10);

  win_present(id);
  serve();
  enum_all(recs);
  expect_eq(recs[0].dirty_w, -1);

  win_close(id);
  serve();
}

static void check_events(void)
{
  int id = win_create(120, 80, "e");
  serve();
  struct win_event ev;
  expect_eq(win_poll(id, &ev), 0);

  int space = win_key_space(id);
  struct winevent key = {WEV_KEY, 'a', 2};
  expect_eq(win_deliver(id, &key), 0);
  expect_eq(win_key_space(id), space - 1);
  wms_flush();
  expect_eq(win_key_space(id), space);
  expect_eq(win_poll(id, &ev), 1);
  expect_eq(ev.type, WEV_KEY);
  expect_eq(ev.a, 'a');
  expect_eq(ev.b, 2);

  for (int i = 1; i <= 3; i++) {
    struct winevent mv = {WEV_MOVE, i, i * 10};
    win_deliver(id, &mv);
  }
  wms_flush();
  expect_eq(win_poll(id, &ev), 1);
  expect_eq(ev.type, WEV_MOVE);
  expect_eq(ev.a, 3);
  expect_eq(ev.b, 30);
  expect_eq(win_poll(id, &ev), 0);

  expect_eq(win_poll(id + 1, &ev), -1);
  win_close(id);
  serve();
}

static void check_resize(void)
{
  int id = win_create(200, 100, "r");
  serve();
  int made = creates;
  struct winrec recs[WM_LIST_MAX];

  set_geom(id, 400, 300);
  enum_all(recs);
  expect_eq(recs[0].w, 400 + 2 * WM_BORDER);
  expect_eq(recs[0].buf_w, 200); /* still the old buffer: the app hasn't drawn the new size yet */
  expect_eq(creates, made + 1);

  struct win_event ev;
  expect_eq(win_poll(id, &ev), 1);
  expect_eq(ev.type, WEV_RESIZE);
  expect_eq(ev.a, 400);
  expect_eq(ev.b, 300);
  unsigned *fb = win_fb_of(id);
  expect(fb == (unsigned *)(uintptr_t)SHM_LOW_ADDR);
  fb[299 * 400 + 399] = 0x123456;
  serve();
  enum_all(recs);
  expect_eq(recs[0].buf_w, 200);

  win_present(id);
  serve();
  enum_all(recs);
  expect_eq(recs[0].buf_w, 400);
  expect_eq(recs[0].buf_h, 300);
  struct winmap m;
  win_map(id, &m);
  expect_eq(m.stride, 400);
  expect_eq(((unsigned *)(uintptr_t)m.vaddr)[299 * 400 + 399], 0x123456);
  expect_eq(live_objects(), 1); /* the old buffer went once both sides let go */

  set_geom(id, 200, 100);
  expect_eq(win_poll(id, &ev), 1);
  expect_eq(ev.a, 200);
  expect_eq(creates, made + 1); /* shrinking reuses the buffer */
  win_present(id);
  serve();
  enum_all(recs);
  expect_eq(recs[0].buf_w, 200);

  win_close(id);
  serve();
  expect_eq(live_objects(), 0);
}

static void check_superseded_resize(void)
{
  int id = win_create(200, 100, "s");
  serve();
  set_geom(id, 400, 300);
  set_geom(id, 600, 500);

  struct win_event ev;
  expect_eq(win_poll(id, &ev), 1); /* the first offer was withdrawn, so only the second arrives */
  expect_eq(ev.type, WEV_RESIZE);
  expect_eq(ev.a, 600);
  expect_eq(ev.b, 500);
  expect_eq(win_poll(id, &ev), 0);
  win_present(id);
  serve();
  struct winrec recs[WM_LIST_MAX];
  enum_all(recs);
  expect_eq(recs[0].buf_w, 600);
  expect_eq(live_objects(), 1);
  win_close(id);
  serve();
  expect_eq(live_objects(), 0);
}

static void check_foreign_requests(void)
{
  int id = win_create(100, 100, "mine");
  serve();
  int raw = raw_open(OTHER_PID);
  raw_send(raw, WM_CLOSE, id, 0, 0);
  raw_send(raw, WM_RESIZABLE, id, 0, 0);
  raw_send(raw, WM_OWNS, -1, APP_PID, 0);
  raw_send(raw, WM_OWNS, -1, OTHER_PID, 0);
  serve();
  struct winrec recs[WM_LIST_MAX];
  expect_eq(enum_all(recs), 1);
  expect(recs[0].flags & UWIN_F_RESIZABLE);
  struct wm_msg m;
  expect(raw_recv(raw, &m) && m.type == WM_OWNS_REPLY && m.a == APP_PID && m.b == 1);
  expect(raw_recv(raw, &m) && m.type == WM_OWNS_REPLY && m.a == OTHER_PID && m.b == 0);

  struct wm_msg bad = {WM_PRESENT, 3, id, 0, 0, 0, 0};
  wmt_write(raw, &bad, (int)sizeof bad);
  serve();
  expect_eq(wmt_read_nb(raw, &m, (int)sizeof m), -1); /* dropped for the malformed message */
  wmt_close(raw);
  expect_eq(enum_all(recs), 1);

  win_close(id);
  serve();
}

static void check_dropped_client(void)
{
  int raw = raw_open(OTHER_PID);
  struct wm_msg_title t;
  memset(&t, 0, sizeof t);
  t.m = (struct wm_msg){WM_CREATE, (uint16_t)sizeof t, -1, 50, 40, 0, 0};
  wmt_write(raw, &t, (int)sizeof t);
  serve();
  struct wm_msg m;
  expect(raw_recv(raw, &m) && m.type == WM_CREATED && m.id >= 0);
  struct winrec recs[WM_LIST_MAX];
  expect_eq(enum_all(recs), 1);
  expect_eq(recs[0].owner_pid, OTHER_PID);
  wmt_close(raw);
  serve();
  expect_eq(enum_all(recs), 0);
  expect_eq(live_objects(), 0);

  /* The compositor going away closes every window of the app */
  int id = win_create(80, 60, "gone");
  serve();
  for (int i = 0; i < CHAN_MAX; i++)
    if (chans[i].used && chans[i].client_pid == APP_PID)
      chans[i].hangup = true;
  struct win_event ev;
  expect_eq(win_poll(id, &ev), 1);
  expect_eq(ev.type, WEV_CLOSE);
  win_close(id);
  serve();
}

static void check_parents(void)
{
  int a = win_create(100, 100, "a");
  int b = win_create(80, 60, "b");
  expect_eq(win_set_parent(b, a, 1), 0);
  expect_eq(win_set_parent(a, b, 0), -1); /* would be a cycle */
  expect_eq(win_set_parent(a, a, 0), -1);
  serve();
  struct winrec recs[WM_LIST_MAX];
  int n = enum_all(recs);
  struct winrec *rb = rec_of(recs, n, b);
  expect(rb && rb->parent == a && (rb->flags & UWIN_F_MODAL));

  win_close(a);
  serve();
  n = enum_all(recs);
  rb = rec_of(recs, n, b);
  expect(rb && rb->parent == -1 && !(rb->flags & UWIN_F_MODAL));
  win_close(b);
  serve();
  expect_eq(live_objects(), 0);
}

int main(void)
{
  int probe = host_memfd(4096);
  if (probe < 0) {
    printf("  SKIP needs memfd (Linux)\n");
    return 0;
  }
  host_close(probe);
  test("only a root compositor gets windows", check_root_only);
  test("create, share pixels, rename, close", check_create_share_close);
  test("present damage is merged, clipped and cleared", check_damage);
  test("events reach the app; hover moves coalesce", check_events);
  test("resize: the old buffer shows until the app draws the new size", check_resize);
  test("a withdrawn resize offer is skipped", check_superseded_resize);
  test("requests for another process's window are ignored", check_foreign_requests);
  test("a dropped client loses its windows; a lost compositor closes the app's", check_dropped_client);
  test("transient parents can't form a cycle and clear when the parent closes", check_parents);
  return 0;
}
