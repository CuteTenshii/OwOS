/* tray.c: client side of the tray protocol (see tray.h) */
#include "tray.h"
#include "owos.h"

static int write_full(int fd, const void *buf, int n)
{
  const char *p = (const char *)buf;
  int put = 0;
  while (put < n) {
    int w = write(fd, p + put, n - put);
    if (w <= 0)
      return 0;
    put += w;
  }
  return 1;
}

/* u8-length-prefixed string, truncated to cap - 1 bytes */
static int wr_str(int fd, const char *s, int cap)
{
  int n = s ? strlen(s) : 0;
  if (n > cap - 1)
    n = cap - 1;
  unsigned char b = (unsigned char)n;
  return write_full(fd, &b, 1) && (n == 0 || write_full(fd, s, n));
}

static int finish(int ch, int ok)
{
  char r = 0;
  ok = ok && read(ch, &r, 1) == 1 && r == TRAY_ACK;
  close(ch);
  return ok ? 0 : -1;
}

int tray_set(const struct tray_icon *t)
{
  if (!t || !t->id || !t->id[0])
    return -1;
  int ch = port_open(TRAY_PORT, 0);
  if (ch < 0)
    return -1;
  char op = TRAY_OP_SET;
  return finish(ch, write_full(ch, &op, 1) && write_full(ch, &t->pid, 4) && wr_str(ch, t->id, TRAY_ID_MAX) &&
                        wr_str(ch, t->icon, TRAY_ICON_MAX) && wr_str(ch, t->action, TRAY_CMD_MAX) &&
                        wr_str(ch, t->menu, TRAY_CMD_MAX));
}

int tray_remove(const char *id)
{
  if (!id || !id[0])
    return -1;
  int ch = port_open(TRAY_PORT, 0);
  if (ch < 0)
    return -1;
  char op = TRAY_OP_REMOVE;
  return finish(ch, write_full(ch, &op, 1) && wr_str(ch, id, TRAY_ID_MAX));
}
