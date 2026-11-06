/* ipc.c: named local ports (see ipc.h). Each connection is two pipe-pool rings (c2s, s2c). */

#include "ipc.h"
#include "kstring.h"
#include "pipe.h"
#include "timer.h"

#define IPC_MAX_PORTS 4
#define IPC_NAME_MAX 16

enum { PEND_NONE = 0, PEND_CLAIMED, PEND_READY }; /* zero = no pending conn */

struct port {
  bool used;
  char name[IPC_NAME_MAX];
  int pend;     /* PEND_*: guards the connection slot */
  int c2s, s2c; /* pending connection's pipe ids (READY) */
};

static struct port ports[IPC_MAX_PORTS];

/* Runs under IF=0 but yields via timer_sleep; cli/sti make claim/publish atomic. */
static struct port *find_port(const char *name)
{
  for (int i = 0; i < IPC_MAX_PORTS; i++)
    if (ports[i].used && streq(ports[i].name, name))
      return &ports[i];
  return 0;
}

int ipc_port_create(const char *name)
{
  if (find_port(name))
    return -1;
  for (int i = 0; i < IPC_MAX_PORTS; i++) {
    if (!ports[i].used) {
      ports[i].used = true;
      ports[i].pend = PEND_NONE;
      int j = 0;
      for (; name[j] && j < IPC_NAME_MAX - 1; j++)
        ports[i].name[j] = name[j];
      ports[i].name[j] = '\0';
      return i;
    }
  }
  return -1;
}

void ipc_port_destroy(int id)
{
  if (id < 0 || id >= IPC_MAX_PORTS || !ports[id].used)
    return;
  struct port *p = &ports[id];
  /* Release the server-side ends of an unaccepted connection. */
  if (p->pend == PEND_READY) {
    pipe_close(p->c2s, 0);
    pipe_close(p->s2c, 1);
  }
  p->used = false;
  p->pend = PEND_NONE;
}

int ipc_connect(const char *name, int *rx, int *tx)
{
  struct port *p = find_port(name);
  if (!p)
    return -1;

  /* Claim the single connection slot, waiting if one is pending. */
  for (;;) {
    __asm__ volatile("cli");
    if (p->used && p->pend == PEND_NONE) {
      p->pend = PEND_CLAIMED;
      __asm__ volatile("sti");
      break;
    }
    int gone = !p->used;
    __asm__ volatile("sti");
    if (gone)
      return -1;
    timer_sleep(5);
  }

  int c2s = pipe_alloc();
  int s2c = c2s < 0 ? -1 : pipe_alloc();
  if (c2s < 0 || s2c < 0) {
    if (c2s >= 0) {
      pipe_close(c2s, 0);
      pipe_close(c2s, 1);
    }
    __asm__ volatile("cli");
    p->pend = PEND_NONE;
    __asm__ volatile("sti");
    return -1;
  }

  /* Publish: the server's accept() will pick these up. */
  __asm__ volatile("cli");
  p->c2s = c2s;
  p->s2c = s2c;
  p->pend = PEND_READY;
  __asm__ volatile("sti");

  *rx = s2c;
  *tx = c2s;
  return 0;
}

int ipc_accept(int id, int *rx, int *tx)
{
  if (id < 0 || id >= IPC_MAX_PORTS || !ports[id].used)
    return -1;
  struct port *p = &ports[id];

  while (p->pend != PEND_READY) {
    if (!p->used)
      return -1;
    timer_sleep(10);
  }

  __asm__ volatile("cli");
  int c2s = p->c2s, s2c = p->s2c;
  p->pend = PEND_NONE;
  __asm__ volatile("sti");

  *rx = c2s;
  *tx = s2c;
  return 0;
}
