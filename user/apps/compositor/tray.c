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
  char action[TRAY_CMD_MAX], menu[TRAY_CMD_MAX];
  int pid;
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
  memmove(&items[i], &items[i + 1], (nitems - i - 1) * sizeof items[0]);
  nitems--;
}

static void serve(int ch)
{
  char op;
  if (!read_full(ch, &op, 1))
    return;
  struct tray_item t = {0};
  if (op == TRAY_OP_SET) {
    if (!read_full(ch, &t.pid, 4) || !rd_str(ch, t.id, sizeof t.id) || !rd_str(ch, t.icon, sizeof t.icon) ||
        !rd_str(ch, t.action, sizeof t.action) || !rd_str(ch, t.menu, sizeof t.menu) || !t.id[0])
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
    items[i] = t;
  } else if (nitems < TRAY_MAX) {
    items[nitems++] = t;
  }
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

bool tray_click(int mx, int my, bool right)
{
  if (my < screen_h - WM_BAR_H)
    return false;
  char cmd[TRAY_CMD_MAX];
  mutex_lock(&lock);
  int i = (mx - (tray_x1 - nitems * TRAY_SLOT)) / TRAY_SLOT;
  bool hit = mx < tray_x1 && mx >= tray_x1 - nitems * TRAY_SLOT && i < nitems;
  if (hit)
    strcpy(cmd, right ? items[i].menu : items[i].action);
  mutex_unlock(&lock);
  if (hit && cmd[0]) {
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
  return hit;
}
