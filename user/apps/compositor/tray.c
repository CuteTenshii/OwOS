/* tray.c: the system tray. A server thread owns the "tray" port and the icon table; the main
 * thread draws it in the taskbar and routes clicks */
#include "tray.h"
#include "comp.h"
#include "mutex.h"

#define TRAY_MAX 16
#define TRAY_SLOT (ICON_PX + 8)

struct tray_item {
  char id[TRAY_ID_MAX];
  char icon[TRAY_ICON_MAX];
  char action[TRAY_CMD_MAX];
  struct tray_entry *menu;
  int nmenu, pid;
};

static struct tray_item items[TRAY_MAX];
static int nitems;
static mutex_t lock = MUTEX_INIT;
static int tray_x1;
volatile unsigned tray_gen;

static int read_full(int fd, void *buf, int n)
{
  char *p = buf;
  for (int got = 0; got < n;) {
    int r = read(fd, p + got, n - got);
    if (r <= 0)
      return 0;
    got += r;
  }
  return 1;
}

static int rd_str(int fd, char *dst, int cap)
{
  unsigned char len;
  if (!read_full(fd, &len, 1))
    return 0;
  int di = 0;
  for (int i = 0; i < len; i++) {
    char c;
    if (!read_full(fd, &c, 1))
      return 0;
    if (di < cap - 1)
      dst[di++] = c;
  }
  dst[di] = '\0';
  return 1;
}

static int find_item(const char *id)
{
  for (int i = 0; i < nitems; i++)
    if (streq(items[i].id, id))
      return i;
  return -1;
}

static void item_remove(int i)
{
  free(items[i].menu);
  memmove(&items[i], &items[i + 1], (nitems - i - 1) * sizeof items[0]);
  nitems--;
}

static int read_menu(int ch, struct tray_item *t)
{
  unsigned char cnt[2];
  if (!read_full(ch, cnt, 2))
    return 0;
  int n = cnt[0] | cnt[1] << 8;
  if (n > TRAY_MENU_MAX)
    return 0;
  if (n && !(t->menu = malloc(n * sizeof *t->menu)))
    return 0;
  for (int i = 0; i < n; i++) {
    struct tray_entry *e = &t->menu[i];
    unsigned char fd[2];
    if (!read_full(ch, fd, 2) || !rd_str(ch, e->label, sizeof e->label) || !rd_str(ch, e->cmd, sizeof e->cmd)) {
      free(t->menu);
      t->menu = 0;
      return 0;
    }
    e->flags = fd[0];
    e->depth = fd[1] > (i ? e[-1].depth + 1 : 0) ? (i ? e[-1].depth + 1 : 0) : fd[1];
    t->nmenu = i + 1;
  }
  return 1;
}

static void serve(int ch)
{
  char op;
  if (!read_full(ch, &op, 1))
    return;
  struct tray_item t = {0};
  if (op == TRAY_OP_SET) {
    if (!read_full(ch, &t.pid, 4) || !rd_str(ch, t.id, sizeof t.id) || !rd_str(ch, t.icon, sizeof t.icon) ||
        !rd_str(ch, t.action, sizeof t.action) || !t.id[0])
      return;
    if (!read_menu(ch, &t))
      return;
  } else if (op == TRAY_OP_REMOVE) {
    if (!rd_str(ch, t.id, sizeof t.id))
      return;
  } else
    return;

  mutex_lock(&lock);
  int i = find_item(t.id);
  if (op == TRAY_OP_REMOVE) {
    if (i >= 0)
      item_remove(i);
  } else if (i >= 0) {
    free(items[i].menu);
    items[i] = t;
  } else if (nitems < TRAY_MAX) {
    items[nitems++] = t;
  } else
    free(t.menu);
  tray_gen++;
  mutex_unlock(&lock);
  char ack = TRAY_ACK;
  write(ch, &ack, 1);
}

static char server_stack[16384];
static void server_fn(void)
{
  int lst = port_open(TRAY_PORT, 1);
  if (lst < 0) {
    for (;;)
      msleep(1000);
  }
  for (;;) {
    int ch = port_accept(lst);
    if (ch < 0)
      continue;
    serve(ch);
    close(ch);
  }
}

void tray_start(void) { thread_create(server_fn, &server_stack[sizeof server_stack]); }

void tray_prune(void)
{
  mutex_lock(&lock);
  for (int i = 0; i < nitems;) {
    if (items[i].pid > 0 && kill(items[i].pid, 0) < 0) {
      item_remove(i);
      tray_gen++;
    } else
      i++;
  }
  mutex_unlock(&lock);
}

int tray_draw(int right, int y)
{
  mutex_lock(&lock);
  int left = right - nitems * TRAY_SLOT;
  tray_x1 = right;
  for (int i = 0; i < nitems; i++) {
    const unsigned *px = icon_lookup(items[i].icon);
    if (!px)
      px = icon_lookup("application-default");
    if (px)
      gfx_blit_argb(left + i * TRAY_SLOT + (TRAY_SLOT - ICON_PX) / 2, y + (WM_BAR_H - ICON_PX) / 2, ICON_PX, ICON_PX,
                    px);
  }
  mutex_unlock(&lock);
  return left;
}

static void run(char *cmd)
{
  char path[TRAY_CMD_MAX + 8];
  char *arg = 0;
  for (char *c = cmd; *c; c++)
    if (*c == ' ') {
      *c = '\0';
      arg = c + 1;
      break;
    }
  strcpy(path, cmd[0] == '/' ? "" : "/bin/");
  strcat(path, cmd);
  kid_track(spawn(path, arg));
}

void tray_menu_toggle(const char *owner, int idx)
{
  mutex_lock(&lock);
  int i = find_item(owner);
  if (i >= 0 && idx < items[i].nmenu && (items[i].menu[idx].flags & TRAY_ENTRY_CHECKABLE)) {
    items[i].menu[idx].flags ^= TRAY_ENTRY_CHECKED;
    tray_gen++;
  }
  mutex_unlock(&lock);
}

bool tray_click(int mx, int my, bool right)
{
  if (my < screen_h - WM_BAR_H)
    return false;
  char cmd[TRAY_CMD_MAX] = "", id[TRAY_ID_MAX];
  struct tray_entry *menu = 0;
  int nmenu = 0, cx = 0;
  mutex_lock(&lock);
  int i = (mx - (tray_x1 - nitems * TRAY_SLOT)) / TRAY_SLOT;
  bool hit = mx < tray_x1 && mx >= tray_x1 - nitems * TRAY_SLOT && i < nitems;
  if (hit && !right)
    strcpy(cmd, items[i].action);
  if (hit && right && items[i].nmenu && (menu = malloc(items[i].nmenu * sizeof *menu))) {
    nmenu = items[i].nmenu;
    memcpy(menu, items[i].menu, nmenu * sizeof *menu);
    strcpy(id, items[i].id);
    cx = tray_x1 - (nitems - i) * TRAY_SLOT + TRAY_SLOT / 2;
  }
  mutex_unlock(&lock);
  if (menu)
    tray_menu_open_at(cx, id, menu, nmenu);
  if (cmd[0])
    run(cmd);
  return hit;
}
