/* pipe.c: fixed pool of pipe ring buffers; blocking ends yield to the scheduler. */

#include "pipe.h"
#include "sched.h"

#define MAX_PIPES 64 /* spawn_cap uses 1 each; an IPC channel or pty uses 2 (rx+tx) */
#define PIPE_SIZE 512

struct pipe {
  bool used;
  char buf[PIPE_SIZE];
  int head, tail, count;
  int readers, writers;
  struct pipe_tty tty;
};

static struct pipe pipes[MAX_PIPES];

int pipe_alloc(void)
{
  for (int i = 0; i < MAX_PIPES; i++) {
    if (!pipes[i].used) {
      pipes[i].used = true;
      pipes[i].head = pipes[i].tail = pipes[i].count = 0;
      pipes[i].readers = 1;
      pipes[i].writers = 1;
      pipes[i].tty = (struct pipe_tty){80, 24, 0};
      return i;
    }
  }
  return -1;
}

void pipe_ref(int id, int writer)
{
  if (writer)
    pipes[id].writers++;
  else
    pipes[id].readers++;
}

void pipe_close(int id, int writer)
{
  struct pipe *p = &pipes[id];
  if (writer)
    p->writers--;
  else
    p->readers--;
  if (p->readers <= 0 && p->writers <= 0)
    p->used = false; /* both ends gone: free the pipe */
}

int pipe_read(int id, char *buf, int len)
{
  struct pipe *p = &pipes[id];
  while (p->count == 0) {
    if (p->writers <= 0)
      return 0; /* EOF: drained and no writers left */
    if (sched_intr_pending())
      return -1;   /* EINTR: let the signal (SIGKILL included) be delivered */
    sched_yield(); /* let a writer run */
  }
  int n = 0;
  while (n < len && p->count > 0) {
    buf[n++] = p->buf[p->tail];
    p->tail = (p->tail + 1) % PIPE_SIZE;
    p->count--;
  }
  return n;
}

/* Non-blocking read: >0 bytes, 0 if empty with a writer attached, -1 on EOF. */
int pipe_read_nb(int id, char *buf, int len)
{
  struct pipe *p = &pipes[id];
  if (p->count == 0)
    return p->writers <= 0 ? -1 : 0;
  int n = 0;
  while (n < len && p->count > 0) {
    buf[n++] = p->buf[p->tail];
    p->tail = (p->tail + 1) % PIPE_SIZE;
    p->count--;
  }
  return n;
}

int pipe_write(int id, const char *buf, int len)
{
  struct pipe *p = &pipes[id];
  int n = 0;
  while (n < len) {
    while (p->count == PIPE_SIZE) {
      if (p->readers <= 0)
        return n; /* nobody reading; stop */
      if (sched_intr_pending())
        return n ? n : -1;
      sched_yield(); /* let the reader drain it */
    }
    p->buf[p->head] = buf[n++];
    p->head = (p->head + 1) % PIPE_SIZE;
    p->count++;
  }
  return n;
}

struct pipe_tty *pipe_tty(int id) { return &pipes[id].tty; }
