/* ipc.c — named local ports (see ipc.h).
 *
 * A connection is two pipe-pool rings: c2s (client->server) and s2c
 * (server->client). Each end is one FD_CHANNEL fd that reads its rx ring and
 * writes its tx ring, so the channel is symmetric and ordinary read()/write()
 * just work. The pipe pool already gives us blocking flow control and EOF on
 * the peer's close, so this layer is only a name registry plus a one-slot
 * connection handshake (serialized: one un-accepted connection per port at a
 * time, the rest wait — fine for a request/reply daemon like the clipboard).
 *
 * The port table is file-static and zero-initialized, so `used == 0` and
 * `pend == PEND_NONE` are the correct cold state with no init call.
 */

#include "ipc.h"
#include "kstring.h"
#include "pipe.h"
#include "timer.h"

#define IPC_MAX_PORTS 4
#define IPC_NAME_MAX 16

enum { PEND_NONE = 0, PEND_CLAIMED, PEND_READY }; /* zero = no pending conn */

struct port {
  int used;
  char name[IPC_NAME_MAX];
  int pend;     /* PEND_* — guards the single connection slot          */
  int c2s, s2c; /* the pending connection's pipe-pool ids (when READY)  */
};

static struct port ports[IPC_MAX_PORTS];

/* These run from the int 0x80 gate (IF=0) but yield via timer_sleep, which
 * re-enables interrupts; the explicit cli/sti below make each claim/publish
 * test-and-set atomic against a timer-driven context switch. */

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
    return -1; /* one server per name           */
  for (int i = 0; i < IPC_MAX_PORTS; i++) {
    if (!ports[i].used) {
      ports[i].used = 1;
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
  /* Release the server-side ends of a published-but-unaccepted connection
   * (the server reads c2s and writes s2c); the client still holds its own
   * ends and frees the rings when it closes. */
  if (p->pend == PEND_READY) {
    pipe_close(p->c2s, 0); /* server is c2s's reader            */
    pipe_close(p->s2c, 1); /* server is s2c's writer            */
  }
  p->used = 0;
  p->pend = PEND_NONE;
}

int ipc_connect(const char *name, int *rx, int *tx)
{
  struct port *p = find_port(name);
  if (!p)
    return -1; /* no server registered          */

  /* Claim the single connection slot (atomically), waiting if a prior
   * connection hasn't been accepted yet. */
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

  *rx = s2c; /* client reads server->client */
  *tx = c2s; /* client writes client->server */
  return 0;
}

int ipc_accept(int id, int *rx, int *tx)
{
  if (id < 0 || id >= IPC_MAX_PORTS || !ports[id].used)
    return -1;
  struct port *p = &ports[id];

  while (p->pend != PEND_READY) { /* park until a client publishes  */
    if (!p->used)
      return -1;
    timer_sleep(10); /* timer_sleep sti;hlt's: low CPU */
  }

  __asm__ volatile("cli");
  int c2s = p->c2s, s2c = p->s2c;
  p->pend = PEND_NONE; /* free the slot for the next     */
  __asm__ volatile("sti");

  *rx = c2s; /* server reads client->server */
  *tx = s2c; /* server writes server->client */
  return 0;
}
